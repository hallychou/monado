// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Allocation collection over native images.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_allocation_collection.h"


#ifdef __cplusplus
extern "C" {
#endif

struct xrt_image_native;
struct xrt_swapchain_create_info;

/*!
 * Create an allocation collection that owns @p native_images.
 *
 * Supports @ref XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO and
 * @ref XRT_ALLOCATION_TYPE_NATIVE_IMAGE, every get_all call hands out new
 * references to the handles. Invalid handles are handed out as invalid, DXGI
 * handles are handed out as they are and never closed.
 *
 * On success the collection owns the handles and they are set to invalid in
 * @p native_images, on failure they are left untouched.
 *
 * @ingroup aux_util
 */
xrt_result_t
u_native_images_xac_create(const struct xrt_swapchain_create_info *info,
                           struct xrt_image_native *native_images,
                           uint32_t image_count,
                           struct xrt_allocation_collection **out_xac);


#ifdef __cplusplus
}
#endif
