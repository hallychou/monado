// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Native image typed helpers for the allocation collection.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup xrt
 */

#pragma once

#include "xrt/xrt_allocation_collection.h"
#include "xrt/xrt_compositor.h"


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @addtogroup xrt_iface
 * @{
 */

/*!
 * Typed helper to get all the native images from an allocation collection, see
 * @ref XRT_ALLOCATION_TYPE_NATIVE_IMAGE for ownership transfer semantics.
 *
 * @param xac The allocation collection.
 * @param[out] out_images Array of at least @ref xrt_allocation_collection::image_count elements.
 *
 * @public @memberof xrt_allocation_collection
 */
static inline xrt_result_t
xrt_allocation_collection_get_native_images(struct xrt_allocation_collection *xac, struct xrt_image_native *out_images)
{
	return xac->get_all_raw(xac, XRT_ALLOCATION_TYPE_NATIVE_IMAGE, out_images);
}

/*!
 * @}
 */

#ifdef __cplusplus
}
#endif
