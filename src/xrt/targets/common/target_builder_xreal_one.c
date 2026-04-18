// Copyright 2026, LeGrandToto
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XReal One Pro system builder.
 *
 * Unlike USB HID drivers, the XReal One Pro communicates over TCP.  There is
 * no USB device to enumerate, so estimate_system() probes the hardcoded IP
 * address with a short non-blocking connect attempt.
 *
 * TECHNICAL DEBT (Linux-only): uses POSIX sockets / poll(2).
 *
 * @ingroup drv_xreal_one
 */

#ifndef __linux__
#error "XReal One Pro driver requires Linux (POSIX sockets / poll(2))"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_system.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_trace_marker.h"

#include "target_builder_helpers.h"

#include "xreal_one/xreal_one_hmd.h"
#include "xreal_one/xreal_one_interface.h"


/*
 *
 * Module-level state.
 *
 */

DEBUG_GET_ONCE_LOG_OPTION(xreal_one_log, "XREAL_ONE_LOG", U_LOGGING_WARN)

static enum u_logging_level xreal_one_log_level;

#define XREAL_ONE_WARN(...)  U_LOG_IFL_W(xreal_one_log_level, __VA_ARGS__)
#define XREAL_ONE_ERROR(...) U_LOG_IFL_E(xreal_one_log_level, __VA_ARGS__)

static const char *driver_list[] = {
    "xreal_one",
};

/*
 * Probe timeout for estimate_system(): 500 ms.
 *
 * TECHNICAL DEBT: this adds up to 500 ms latency to Monado startup when the
 * glasses are not plugged in.  A future improvement could check for the RNDIS
 * network interface in /sys/class/net instead, which is instantaneous.
 */
#define PROBE_TIMEOUT_MS 500


/*
 *
 * Connectivity probe.
 *
 */

/*!
 * Try a non-blocking TCP connect to the glasses with a short timeout.
 * Returns true if the glasses accepted the connection.
 *
 * TECHNICAL DEBT: uses POSIX non-blocking connect + poll(2), Linux-only.
 */
static bool
xreal_one_probe_device(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return false;
	}

	// Switch to non-blocking so connect() returns immediately.
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(XREAL_ONE_PORT);
	inet_pton(AF_INET, XREAL_ONE_IP, &addr.sin_addr);

	int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	bool connected = false;

	if (ret == 0) {
		// Instant connect (unusual for TCP but valid).
		connected = true;
	} else if (errno == EINPROGRESS) {
		// Wait up to PROBE_TIMEOUT_MS for the connection to complete.
		struct pollfd pfd = {.fd = fd, .events = POLLOUT};
		int poll_ret      = poll(&pfd, 1, PROBE_TIMEOUT_MS);

		if (poll_ret > 0 && (pfd.revents & POLLOUT) && !(pfd.revents & POLLERR)) {
			// Verify there is no pending error on the socket.
			int sock_err  = 0;
			socklen_t len = sizeof(sock_err);
			getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len);
			connected = (sock_err == 0);
		}
	}

	close(fd);
	return connected;
}


/*
 *
 * xrt_builder callbacks.
 *
 */

static xrt_result_t
xreal_one_estimate_system(struct xrt_builder *xb,
                          cJSON *config,
                          struct xrt_prober *xp,
                          struct xrt_builder_estimate *estimate)
{
	U_ZERO(estimate);

	if (xreal_one_probe_device()) {
		estimate->certain.head = true;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
xreal_one_open_system_impl(struct xrt_builder *xb,
                           cJSON *config,
                           struct xrt_prober *xp,
                           struct xrt_tracking_origin *origin,
                           struct xrt_system_devices *xsysd,
                           struct xrt_frame_context *xfctx,
                           struct t_builder_roles_helper *tbrh)
{
	DRV_TRACE_MARKER();

	xreal_one_log_level = debug_get_log_option_xreal_one_log();

	struct xrt_device *dev = xreal_one_hmd_create_device(xreal_one_log_level);
	if (dev == NULL) {
		XREAL_ONE_ERROR("Failed to create XReal One Pro device");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	xsysd->static_xdevs[xsysd->static_xdev_count++] = dev;
	tbrh->head = dev;

	return XRT_SUCCESS;
}

static void
xreal_one_destroy(struct xrt_builder *xb)
{
	free(xb);
}


/*
 *
 * Exported constructor.
 *
 */

struct xrt_builder *
xreal_one_builder_create(void)
{
	struct t_builder *ub = U_TYPED_CALLOC(struct t_builder);

	ub->base.estimate_system        = xreal_one_estimate_system;
	ub->base.open_system            = t_builder_open_system_static_roles;
	ub->base.destroy                = xreal_one_destroy;
	ub->base.identifier             = "xreal_one";
	ub->base.name                   = "XReal One Pro";
	ub->base.driver_identifiers     = driver_list;
	ub->base.driver_identifier_count = ARRAY_SIZE(driver_list);

	ub->open_system_static_roles = xreal_one_open_system_impl;

	return &ub->base;
}
