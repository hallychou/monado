// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Helpers for @ref xrt_tracking_origin.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_tracking.h"


#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;


/*!
 * Initialize a tracking origin, to be called by whatever allocates the
 * tracking origin, IPC clients instead get the ID from the service. Clears
 * the tracking origin, assigns a newly generated @ref xrt_tracking_origin::id,
 * sets the type, resets the initial offset to identity and formats the name,
 * truncating it to fit in @ref XRT_TRACKING_NAME_LEN bytes.
 *
 * @ingroup aux_util
 */
void
u_tracking_origin_init(struct xrt_tracking_origin *xto, enum xrt_tracking_type type, const char *fmt, ...)
    XRT_PRINTF_FORMAT(3, 4);

/*!
 * Check that the tracking origin of the given device has an ID, logs an error
 * and asserts if it doesn't. A NULL device or one without a tracking origin
 * passes the check.
 *
 * @ingroup aux_util
 */
void
u_tracking_origin_check_xdev(const struct xrt_device *xdev);

/*!
 * Check that the tracking origins of all of the given devices have an ID,
 * logs a list of all of the devices that don't and then asserts. NULL devices
 * and devices without a tracking origin pass the check. @p xdevs may only be
 * NULL if @p xdev_count is zero.
 *
 * @ingroup aux_util
 */
void
u_tracking_origin_check_xdevs(struct xrt_device *const *xdevs, size_t xdev_count);


#ifdef __cplusplus
}
#endif
