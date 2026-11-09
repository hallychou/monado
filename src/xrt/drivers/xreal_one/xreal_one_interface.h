// Copyright 2026, LeGrandToto
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface for the XReal One Pro driver.
 * @ingroup drv_xreal_one
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_xreal_one XReal One Pro driver
 * @ingroup drv
 *
 * @brief Driver for the XReal One Pro AR glasses.
 *
 * IMU data is retrieved over TCP from the glasses, which present themselves as
 * a USB network adapter (RNDIS on Linux) with a link-local address.
 *
 * Technical debt:
 *  - Linux-only: uses POSIX sockets, poll(2). No Windows/macOS support.
 *  - Hardcoded IP/port (169.254.2.1:52998): no service discovery.
 *  - No factory calibration: gyro bias estimated at runtime from 500 samples.
 *  - HMD geometry (FOV, display size) hardcoded from community measurements.
 */

/*!
 * Builder setup for XReal One Pro glasses.
 *
 * @ingroup drv_xreal_one
 */
struct xrt_builder *
xreal_one_builder_create(void);

/*!
 * @dir drivers/xreal_one
 *
 * @brief xreal_one files.
 */

#ifdef __cplusplus
}
#endif
