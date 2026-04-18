// Copyright 2026, LeGrandToto
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XReal One Pro HMD driver implementation.
 *
 * Reads IMU data from the glasses over TCP, feeds it into Monado's 3-DOF
 * fusion, and exposes an xrt_device for use as an OpenXR head tracker.
 *
 * TECHNICAL DEBT (Linux-only):
 *  - Uses POSIX sockets / poll(2). Porting to Windows requires winsock2.
 *    Guard with #ifdef __linux__ / #error for other platforms.
 *
 * @ingroup drv_xreal_one
 */

#ifndef __linux__
#error "XReal One Pro driver requires Linux (POSIX sockets / poll(2))"
#endif

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_defines.h"

#include "os/os_time.h"
#include "os/os_threading.h"

#include "math/m_api.h"
#include "math/m_imu_3dof.h"
#include "math/m_mathinclude.h"
#include "math/m_relation_history.h"

#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_trace_marker.h"
#include "util/u_var.h"

#include "xreal_one_hmd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


/*
 *
 * Logging helpers.
 *
 */

#define XREAL_ONE_DEBUG(hmd, ...) U_LOG_XDEV_IFL_D(&hmd->base, hmd->log_level, __VA_ARGS__)
#define XREAL_ONE_INFO(hmd, ...)  U_LOG_XDEV_IFL_I(&hmd->base, hmd->log_level, __VA_ARGS__)
#define XREAL_ONE_WARN(hmd, ...)  U_LOG_XDEV_IFL_W(&hmd->base, hmd->log_level, __VA_ARGS__)
#define XREAL_ONE_ERROR(hmd, ...) U_LOG_XDEV_IFL_E(&hmd->base, hmd->log_level, __VA_ARGS__)


/*
 *
 * Protocol magic constants (defined in header as extern, initialised here).
 *
 */

const uint8_t XREAL_ONE_HEADER_MAGIC[XREAL_ONE_HEADER_SIZE]         = {0x27, 0x36, 0x00, 0x00, 0x00, 0x80};
const uint8_t XREAL_ONE_SENSOR_MSG_MAGIC[XREAL_ONE_SENSOR_MSG_SIZE] = {0x00, 0x40, 0x1F, 0x00, 0x00, 0x40};

/*
 * Receive ring buffer.  TCP is a byte stream: we accumulate data here and
 * carve out messages by finding consecutive HEADER_MAGIC occurrences.
 *
 * 4 KB holds > 30 full messages at ~133 bytes each — more than enough for one
 * recv() burst at 1 kHz.
 */
#define RECV_BUF_SIZE 4096


/*
 *
 * Private struct.
 *
 */

/*!
 * Private struct for the xreal_one device.
 *
 * @ingroup drv_xreal_one
 * @implements xrt_device
 */
struct xreal_one_hmd
{
	struct xrt_device base;

	//! Only touched from the reader thread.
	int sock_fd;

	//! Guards fusion, relation_hist, and read.*.
	struct os_mutex data_mutex;

	//! Reader thread handle.
	struct os_thread_helper oth;

	//! Sensor fusion state. Protected by data_mutex.
	struct m_imu_3dof fusion;

	//! Pose history for time-correct queries. Protected by data_mutex.
	struct m_relation_history *relation_hist;

	//! Last successfully decoded IMU sample (for the debug UI).
	struct
	{
		struct xrt_vec3 gyro;      //!< rad/s, Monado world frame
		struct xrt_vec3 accel;     //!< m/s², Monado world frame
		struct xrt_vec3 raw_gyro;  //!< rad/s, raw wire values (gx, gy, gz)
		struct xrt_vec3 raw_accel; //!< g, raw wire values (ax, ay, az)
	} read;

	//! Monotonic time of the last sample pushed to fusion.
	timepoint_ns last_sensor_time;

	//! Gyro bias calibration — accumulate first N samples while still.
	struct
	{
		struct xrt_vec3 sum;
		uint32_t count;
		bool done;
	} calib;

	enum u_logging_level log_level;
};


/*
 *
 * Helper casts.
 *
 */

static inline struct xreal_one_hmd *
xreal_one_hmd(struct xrt_device *xdev)
{
	return (struct xreal_one_hmd *)xdev;
}


/*
 *
 * Packet parsing.
 *
 */

/*!
 * Read a little-endian IEEE 754 float from @p buf.
 *
 * TECHNICAL DEBT: assumes host is little-endian (true for x86 / ARM64 Linux).
 * If ever ported to a big-endian platform, add a byte-swap here.
 */
static inline float
read_float_le(const uint8_t *buf)
{
	float v;
	memcpy(&v, buf, sizeof(v));
	return v;
}

/*!
 * Locate the first occurrence of XREAL_ONE_HEADER_MAGIC in [@p buf, @p buf+len).
 * Returns the offset, or -1 if not found.
 */
static ssize_t
find_header(const uint8_t *buf, size_t len)
{
	if (len < XREAL_ONE_HEADER_SIZE) {
		return -1;
	}

	for (size_t i = 0; i <= len - XREAL_ONE_HEADER_SIZE; i++) {
		if (memcmp(buf + i, XREAL_ONE_HEADER_MAGIC, XREAL_ONE_HEADER_SIZE) == 0) {
			return (ssize_t)i;
		}
	}

	return -1;
}

bool
xreal_one_parse_message(const uint8_t *msg, size_t len, struct xreal_one_parsed_sample *out_sample)
{
	if (len < XREAL_ONE_MIN_MSG_LEN) {
		return false;
	}

	// Validate the sensor-message ID marker.
	if (memcmp(msg + XREAL_ONE_SENSOR_MSG_OFFSET, XREAL_ONE_SENSOR_MSG_MAGIC, XREAL_ONE_SENSOR_MSG_SIZE) != 0) {
		return false;
	}

	const uint8_t *imu = msg + XREAL_ONE_IMU_OFFSET;

	// Wire order (from the Python demo's decode_imu_data):
	//   values[0] = gx,  values[1] = gy,  values[2] = gz
	//   values[3] = az,  values[4] = ay,  values[5] = ax
	out_sample->gx = read_float_le(imu + 0);
	out_sample->gy = read_float_le(imu + 4);
	out_sample->gz = read_float_le(imu + 8);
	out_sample->az = read_float_le(imu + 12);
	out_sample->ay = read_float_le(imu + 16);
	out_sample->ax = read_float_le(imu + 20);

	return true;
}


/*
 *
 * Fusion update.
 *
 */

/*!
 * Convert a parsed IMU sample to Monado's coordinate system and push it into
 * the 3-DOF fusion.
 *
 * TECHNICAL DEBT: coordinate mapping and unit assumptions need hardware
 * validation.
 *
 * Unit assumptions (derived from Python head_tracker.py):
 *  - Gyro: deg/s  → converted to rad/s for m_imu_3dof.
 *  - Accel: g     → converted to m/s² for m_imu_3dof.
 *
 * Axis mapping (from head_tracker.py: gx=pitch, gy=yaw, gz=roll):
 *  - Sensor gx (pitch) → Monado +X  (head nodding)
 *  - Sensor gy (yaw)   → Monado +Y  (head turning)
 *  - Sensor gz (roll)  → Monado +Z  (head tilting)
 *
 * TECHNICAL DEBT: signs and which sensor axis maps to which world axis have
 * not been confirmed against physical hardware.  Adjust the transform below
 * until the virtual head pose matches physical head movement.
 */
static void
update_fusion_locked(struct xreal_one_hmd *hmd, const struct xreal_one_parsed_sample *s, timepoint_ns timestamp_ns)
{
	const float G_TO_MS2 = 9.80665f;

	// Wire gyro axes: gx=pitch, gy=yaw, gz=roll (confirmed by Python demo).
	// Monado frame:   X=right, Y=up, Z=backward.
	// Yaw negated: Monado +Y = turning left; hardware +gy = turning right → negate.
	struct xrt_vec3 gyro = {
	    .x = s->gx,    // pitch: right axis matches
	    .y = -s->gy,   // yaw:   gy = rotation around up; negate for Monado convention
	    .z = -s->gz,   // roll:  gz = rotation around forward → Monado -Z; sign TBD
	};

	struct xrt_vec3 accel = {
	    .x = s->ax * G_TO_MS2,
	    .y = s->az * G_TO_MS2,   // XReal Z(up) → Monado Y(up)
	    .z = -s->ay * G_TO_MS2,  // XReal Y(forward) → Monado -Z(backward)
	};

	hmd->read.raw_gyro  = (struct xrt_vec3){s->gx, s->gy, s->gz};
	hmd->read.raw_accel = (struct xrt_vec3){s->ax, s->ay, s->az};
	hmd->read.gyro      = gyro;
	hmd->read.accel     = accel;

	m_imu_3dof_update(&hmd->fusion, timestamp_ns, &accel, &gyro);
}

static void
push_sample(struct xreal_one_hmd *hmd, const struct xreal_one_parsed_sample *s)
{
	timepoint_ns now_ns = (timepoint_ns)os_monotonic_get_ns();

	// Ensure timestamps strictly increase so the fusion never sees backwards time.
	if (hmd->last_sensor_time >= now_ns) {
		now_ns = hmd->last_sensor_time + 1;
	}
	hmd->last_sensor_time = now_ns;

	struct xrt_space_relation rel;
	U_ZERO(&rel);
	rel.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                     XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);

	os_mutex_lock(&hmd->data_mutex);

	// Collect ~500 stationary samples at startup to calibrate gyro bias.
	// The remapped gyro values (same frame as fusion) are accumulated here.
	if (!hmd->calib.done) {
		hmd->calib.sum.x += s->gx;
		hmd->calib.sum.y += -s->gy;   // matches gyro.y
		hmd->calib.sum.z += -s->gz;   // matches gyro.z
		hmd->calib.count++;

		if (hmd->calib.count >= 500) {
			float n = (float)hmd->calib.count;
			hmd->fusion.gyro_bias.value.x = hmd->calib.sum.x / n;
			hmd->fusion.gyro_bias.value.y = hmd->calib.sum.y / n;
			hmd->fusion.gyro_bias.value.z = hmd->calib.sum.z / n;
			hmd->calib.done = true;
			XREAL_ONE_INFO(hmd, "gyro bias calibrated: (%.4f, %.4f, %.4f) rad/s",
			               hmd->fusion.gyro_bias.value.x, hmd->fusion.gyro_bias.value.y,
			               hmd->fusion.gyro_bias.value.z);
		}
	}

	update_fusion_locked(hmd, s, now_ns);
	rel.pose.orientation = hmd->fusion.rot;
	os_mutex_unlock(&hmd->data_mutex);

	m_relation_history_push(hmd->relation_hist, &rel, now_ns);
}


/*
 *
 * Receive buffer processing.
 *
 * The buffer holds a sliding window of raw TCP bytes.  We search for pairs of
 * HEADER_MAGIC occurrences; the bytes between them form one complete message.
 */

/*!
 * Scan @p buf for complete messages, dispatch each to push_sample, and
 * compact the buffer so only unprocessed bytes remain.
 *
 * Returns the number of bytes consumed (i.e. the new start offset).
 */
static size_t
process_recv_buffer(struct xreal_one_hmd *hmd, uint8_t *buf, size_t len)
{
	size_t consumed = 0;

	while (true) {
		// Find the start of the first message.
		ssize_t h1 = find_header(buf + consumed, len - consumed);
		if (h1 < 0) {
			// No header at all — discard everything except the last
			// HEADER_SIZE-1 bytes (a header might be split across
			// future recv() calls).
			if (len - consumed >= XREAL_ONE_HEADER_SIZE) {
				consumed = len - (XREAL_ONE_HEADER_SIZE - 1);
			}
			break;
		}
		consumed += (size_t)h1;

		// Find the start of the next message (which marks the end of this one).
		ssize_t h2 = find_header(buf + consumed + XREAL_ONE_HEADER_SIZE,
		                          len - consumed - XREAL_ONE_HEADER_SIZE);
		if (h2 < 0) {
			// Current message not yet fully received — wait for more data.
			break;
		}

		// Message = buf[consumed .. consumed+HEADER_SIZE+h2)
		size_t msg_start = consumed;
		size_t msg_len   = (size_t)XREAL_ONE_HEADER_SIZE + (size_t)h2;

		struct xreal_one_parsed_sample sample;
		if (xreal_one_parse_message(buf + msg_start, msg_len, &sample)) {
			push_sample(hmd, &sample);
		} else {
			// Dump the 6 bytes at the sensor-message-ID offset so we can
			// diagnose whether it's a different packet type or a wrong offset.
			if (msg_len >= XREAL_ONE_SENSOR_MSG_OFFSET + XREAL_ONE_SENSOR_MSG_SIZE) {
				const uint8_t *at = buf + msg_start + XREAL_ONE_SENSOR_MSG_OFFSET;
				XREAL_ONE_DEBUG(hmd,
				                "Dropped malformed message (%zu bytes); "
				                "bytes@%d: %02x %02x %02x %02x %02x %02x",
				                msg_len, XREAL_ONE_SENSOR_MSG_OFFSET,
				                at[0], at[1], at[2], at[3], at[4], at[5]);
			} else {
				XREAL_ONE_DEBUG(hmd, "Dropped short message (%zu bytes, need %d)",
				                msg_len, XREAL_ONE_MIN_MSG_LEN);
			}
		}

		consumed += msg_len;
	}

	return consumed;
}


/*
 *
 * TCP reader thread.
 *
 */

/*!
 * Attempt a blocking TCP connection to the glasses.
 * Returns the connected socket fd on success, -1 on failure.
 */
static int
tcp_connect(struct xreal_one_hmd *hmd)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		XREAL_ONE_ERROR(hmd, "socket(): %s", strerror(errno));
		return -1;
	}

	// 5-second connect timeout via SO_SNDTIMEO (simpler than non-blocking + poll for connect).
	struct timeval tv_conn = {.tv_sec = 5, .tv_usec = 0};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_conn, sizeof(tv_conn));

	// Short recv timeout so the thread loop can check the stop flag regularly.
	struct timeval tv_recv = {.tv_sec = 0, .tv_usec = XREAL_ONE_RECV_TIMEOUT_MS * 1000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(XREAL_ONE_PORT);
	if (inet_pton(AF_INET, XREAL_ONE_IP, &addr.sin_addr) != 1) {
		XREAL_ONE_ERROR(hmd, "inet_pton() failed for " XREAL_ONE_IP);
		close(fd);
		return -1;
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		XREAL_ONE_DEBUG(hmd, "connect() to " XREAL_ONE_IP ":%d failed: %s", XREAL_ONE_PORT, strerror(errno));
		close(fd);
		return -1;
	}

	XREAL_ONE_INFO(hmd, "Connected to XReal One Pro at " XREAL_ONE_IP ":%d", XREAL_ONE_PORT);
	return fd;
}

static void *
read_thread(void *ptr)
{
	U_TRACE_SET_THREAD_NAME("XReal One Pro");

	struct xreal_one_hmd *hmd = (struct xreal_one_hmd *)ptr;

	uint8_t  recv_buf[RECV_BUF_SIZE];
	size_t   recv_buf_len = 0;

	while (true) {
		os_thread_helper_lock(&hmd->oth);
		bool running = os_thread_helper_is_running_locked(&hmd->oth);
		os_thread_helper_unlock(&hmd->oth);

		if (!running) {
			break;
		}

		int fd = tcp_connect(hmd);
		if (fd < 0) {
			// Retry after 1 s.
			os_nanosleep(U_TIME_1S_IN_NS);
			continue;
		}

		hmd->sock_fd  = fd;
		recv_buf_len  = 0;

		while (true) {
			os_thread_helper_lock(&hmd->oth);
			running = os_thread_helper_is_running_locked(&hmd->oth);
			os_thread_helper_unlock(&hmd->oth);

			if (!running) {
				break;
			}

			size_t space = RECV_BUF_SIZE - recv_buf_len;
			if (space == 0) {
				// Buffer full with no parseable message — something is wrong.
				// Drop everything and start fresh.
				XREAL_ONE_WARN(hmd, "Receive buffer full, resetting");
				recv_buf_len = 0;
				space        = RECV_BUF_SIZE;
			}

			ssize_t n = recv(fd, recv_buf + recv_buf_len, space, 0);
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					// recv() timed out — normal, loop to check stop flag.
					continue;
				}
				XREAL_ONE_WARN(hmd, "recv() error: %s — reconnecting", strerror(errno));
				break;
			}
			if (n == 0) {
				XREAL_ONE_INFO(hmd, "Connection closed by device — reconnecting");
				break;
			}

			recv_buf_len += (size_t)n;

			size_t consumed = process_recv_buffer(hmd, recv_buf, recv_buf_len);
			if (consumed > 0) {
				recv_buf_len -= consumed;
				memmove(recv_buf, recv_buf + consumed, recv_buf_len);
			}
		}

		close(fd);
		hmd->sock_fd = -1;

		if (!running) {
			break;
		}

		// Brief pause before reconnect to avoid hammering the device.
		os_nanosleep(200 * U_TIME_1MS_IN_NS);
	}

	return NULL;
}


/*
 *
 * xrt_device callbacks.
 *
 */

static xrt_result_t
xreal_one_hmd_update_inputs(struct xrt_device *xdev)
{
	(void)xdev;
	return XRT_SUCCESS;
}

static xrt_result_t
xreal_one_hmd_get_tracked_pose(struct xrt_device *xdev,
                               enum xrt_input_name name,
                               int64_t at_timestamp_ns,
                               struct xrt_space_relation *out_relation)
{
	struct xreal_one_hmd *hmd = xreal_one_hmd(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		U_LOG_XDEV_UNSUPPORTED_INPUT(&hmd->base, hmd->log_level, name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	const enum xrt_space_relation_flags flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);

	struct xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
	relation.relation_flags            = flags;

	m_relation_history_get(hmd->relation_hist, at_timestamp_ns, &relation);
	relation.relation_flags = flags; // Restore after history_get.

	// Guard against degenerate quaternion before handing to the compositor.
	struct xrt_quat *q = &relation.pose.orientation;
	if (math_quat_dot(q, q) > 0.0f) {
		math_quat_normalize(q);
	} else {
		q->w = 1.0f;
	}

	*out_relation = relation;
	return XRT_SUCCESS;
}

static void
xreal_one_hmd_destroy(struct xrt_device *xdev)
{
	struct xreal_one_hmd *hmd = xreal_one_hmd(xdev);

	u_var_remove_root(hmd);

	// Stop the reader thread first; it owns sock_fd.
	os_thread_helper_stop_and_wait(&hmd->oth);

	if (hmd->sock_fd >= 0) {
		close(hmd->sock_fd);
		hmd->sock_fd = -1;
	}

	m_relation_history_destroy(&hmd->relation_hist);
	m_imu_3dof_close(&hmd->fusion);

	os_thread_helper_destroy(&hmd->oth);
	os_mutex_destroy(&hmd->data_mutex);

	u_device_free(&hmd->base);
}


/*
 *
 * Display geometry setup.
 *
 * TECHNICAL DEBT: all values below are community-sourced approximations for
 * the XReal One Pro.  They should be replaced with authoritative measurements
 * once available.
 *
 *  - Per-eye resolution 1920×1080, side-by-side stereo (3840×1080 framebuffer)
 *  - Horizontal FOV ~46° per eye (same optics family as XReal Air)
 *  - Refresh rate 60 Hz
 *  - No lens distortion correction (glasses are see-through, minimal distortion)
 */
static bool
setup_display(struct xreal_one_hmd *hmd)
{
	struct u_device_simple_info info;
	U_ZERO(&info);

	info.display.w_pixels = 1920;
	info.display.h_pixels = 1080;

	// Physical display panel size (approximate).
	info.display.w_meters = 0.13f;
	info.display.h_meters = 0.07f;

	// Side-by-side stereo: double horizontal extent.
	info.display.w_pixels *= 2;
	info.display.w_meters *= 2.0f;

	info.lens_horizontal_separation_meters = 0.13f; // 2 × 0.065 m IPD estimate
	info.lens_vertical_position_meters     = 0.035f;

	info.fov[0] = (float)(46.0 * (M_PI / 180.0));
	info.fov[1] = (float)(46.0 * (M_PI / 180.0));

	if (!u_device_setup_split_side_by_side(&hmd->base, &info)) {
		return false;
	}

	hmd->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 60.0f);

	return true;
}


/*
 *
 * Exported device constructor.
 *
 */

struct xrt_device *
xreal_one_hmd_create_device(enum u_logging_level log_level)
{
	enum u_device_alloc_flags flags =
	    (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);

	struct xreal_one_hmd *hmd = U_DEVICE_ALLOCATE(struct xreal_one_hmd, flags, 1, 0);

	hmd->log_level = log_level;
	hmd->sock_fd   = -1;

	hmd->base.update_inputs    = xreal_one_hmd_update_inputs;
	hmd->base.get_tracked_pose = xreal_one_hmd_get_tracked_pose;
	hmd->base.get_view_poses   = u_device_get_view_poses;
	hmd->base.compute_distortion = u_distortion_mesh_none;
	hmd->base.destroy          = xreal_one_hmd_destroy;

	hmd->base.name                          = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type                   = XRT_DEVICE_TYPE_HMD;
	hmd->base.inputs[0].name               = XRT_INPUT_GENERIC_HEAD_POSE;
	hmd->base.supported.orientation_tracking = true;
	hmd->base.supported.position_tracking    = false;

	snprintf(hmd->base.str,    XRT_DEVICE_NAME_LEN, "XReal One Pro");
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "XReal One Pro");

	// Init synchronisation primitives before any possible teardown path.
	os_mutex_init(&hmd->data_mutex);
	os_thread_helper_init(&hmd->oth);

	u_distortion_mesh_set_none(&hmd->base);

	m_imu_3dof_init(&hmd->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_300MS);
	m_relation_history_create(&hmd->relation_hist);

	if (!setup_display(hmd)) {
		XREAL_ONE_ERROR(hmd, "Failed to set up display geometry");
		goto cleanup;
	}

	// Start the reader thread.
	if (os_thread_helper_start(&hmd->oth, read_thread, hmd) < 0) {
		XREAL_ONE_ERROR(hmd, "Failed to start reader thread");
		goto cleanup;
	}

	// Debug UI variables.
	u_var_add_root(hmd, "XReal One Pro", true);
	u_var_add_ro_vec3_f32(hmd, &hmd->read.raw_gyro,  "raw.gyro  (gx,gy,gz rad/s)");
	u_var_add_ro_vec3_f32(hmd, &hmd->read.raw_accel, "raw.accel (ax,ay,az g)");
	u_var_add_ro_vec3_f32(hmd, &hmd->read.gyro,      "mapped.gyro  (rad/s, Monado frame)");
	u_var_add_ro_vec3_f32(hmd, &hmd->read.accel,     "mapped.accel (m/s², Monado frame)");
	u_var_add_log_level(hmd, &hmd->log_level, "Log level");
	m_imu_3dof_add_vars(&hmd->fusion, hmd, "Fusion");

	if (hmd->log_level <= U_LOGGING_DEBUG) {
		u_device_dump_config(&hmd->base, __func__, "XReal One Pro");
	}

	XREAL_ONE_DEBUG(hmd, "Device created — connecting to " XREAL_ONE_IP ":%d in background", XREAL_ONE_PORT);

	return &hmd->base;

cleanup:
	// Tear down whatever we managed to initialise.
	u_var_remove_root(hmd);
	os_thread_helper_stop_and_wait(&hmd->oth);
	m_relation_history_destroy(&hmd->relation_hist);
	m_imu_3dof_close(&hmd->fusion);
	os_thread_helper_destroy(&hmd->oth);
	os_mutex_destroy(&hmd->data_mutex);
	u_device_free(&hmd->base);

	return NULL;
}
