// Copyright 2026, LeGrandToto
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Internal header for the XReal One Pro HMD driver.
 * @ingroup drv_xreal_one
 */

#pragma once

#include "util/u_logging.h"
#include "xrt/xrt_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Connection constants.
 */

//! IP address of the glasses' USB network interface (RNDIS link-local).
#define XREAL_ONE_IP "169.254.2.1"

//! TCP port the glasses' IMU service listens on.
#define XREAL_ONE_PORT 52998

//! Timeout (ms) used for recv() and for connection probing.
#define XREAL_ONE_RECV_TIMEOUT_MS 200

/*
 * Packet protocol constants.
 *
 * The glasses stream a continuous sequence of binary messages over TCP.
 * Message boundaries are detected by searching for the 6-byte header magic.
 * The footer constant is present in the wire format but is NOT used as a
 * framing delimiter (the next header is used instead), so it is defined here
 * only for documentation / future validation use.
 *
 * Wire layout of one message (offsets from the start of HEADER):
 *
 *  Offset  Size  Field
 *  ------  ----  -----
 *       0     6  HEADER magic
 *       6     8  Session ID
 *      14     8  Device timestamp (units unknown; not currently used)
 *      22     2  Invariant
 *      24    10  Static data
 *      34     4  Gyro X  (float LE, deg/s)   \
 *      38     4  Gyro Y  (float LE, deg/s)    |
 *      42     4  Gyro Z  (float LE, deg/s)    |  IMU block
 *      46     4  Accel Z (float LE, g)        |
 *      50     4  Accel Y (float LE, g)        |
 *      54     4  Accel X (float LE, g)       /
 *      58    20  Date / info
 *      78     6  Sensor-message ID magic
 *      84    19  Footer magic
 *      103  ~30  Unknown trailing bytes
 *
 * TECHNICAL DEBT: unit assumptions (deg/s, g) are inferred from the Python
 * demo's head_tracker.py and common ICM-42688-P defaults. They should be
 * verified against hardware captures.
 */

#define XREAL_ONE_HEADER_SIZE 6
#define XREAL_ONE_SENSOR_MSG_SIZE 6

//! Offset of the first IMU float from the start of the framed message.
#define XREAL_ONE_IMU_OFFSET 34

//! Offset of the sensor-message ID magic, used for packet validation.
//! Wire order after IMU block: DATE_INFO (20 bytes) then SENSOR_MSG (6 bytes).
#define XREAL_ONE_SENSOR_MSG_OFFSET 78

//! Minimum plausible message length (must reach the sensor-message ID).
#define XREAL_ONE_MIN_MSG_LEN (XREAL_ONE_SENSOR_MSG_OFFSET + XREAL_ONE_SENSOR_MSG_SIZE)

extern const uint8_t XREAL_ONE_HEADER_MAGIC[XREAL_ONE_HEADER_SIZE];
extern const uint8_t XREAL_ONE_SENSOR_MSG_MAGIC[XREAL_ONE_SENSOR_MSG_SIZE];

/*
 * IMU sample (parsed from one message).
 */

/*!
 * One IMU sample extracted from a wire message.
 *
 * All values use the device's native coordinate frame. Axis mapping to
 * OpenXR/Monado world-space is applied later in xreal_one_hmd.c.
 *
 * TECHNICAL DEBT: the native → world-space mapping was derived from the Python
 * demo's head_tracker.py (gx=pitch, gy=yaw, gz=roll) and has NOT been
 * verified with physical hardware.
 */
struct xreal_one_parsed_sample
{
	float gx; //!< Gyro X (pitch rate, deg/s)
	float gy; //!< Gyro Y (yaw rate,   deg/s)
	float gz; //!< Gyro Z (roll rate,  deg/s)
	float ax; //!< Accel X (g)
	float ay; //!< Accel Y (g)
	float az; //!< Accel Z (g)
};

/*
 * Exported functions.
 */

/*!
 * Create the XReal One Pro HMD device.
 *
 * Spawns the TCP reader thread internally. Returns NULL on failure.
 *
 * @ingroup drv_xreal_one
 */
struct xrt_device *
xreal_one_hmd_create_device(enum u_logging_level log_level);

/*!
 * Parse one wire message into a sample. Returns false if the message is too
 * short or the sensor-message ID magic does not match.
 *
 * @ingroup drv_xreal_one
 */
bool
xreal_one_parse_message(const uint8_t *msg, size_t len, struct xreal_one_parsed_sample *out_sample);

#ifdef __cplusplus
}
#endif
