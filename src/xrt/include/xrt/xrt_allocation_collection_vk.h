// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Vulkan typed helpers for the allocation collection.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup xrt
 */

#pragma once

#include "xrt/xrt_allocation_collection.h"
#include "xrt/xrt_vulkan_includes.h"


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @addtogroup xrt_iface
 * @{
 */

/*!
 * Typed helper to get the @ref XRT_ALLOCATION_PROPERTY_VK_DEVICE property.
 *
 * @param xac The allocation collection.
 * @param[out] out_device The VkDevice the allocations were created on, see
 *                        @ref XRT_ALLOCATION_PROPERTY_VK_DEVICE for lifetime.
 *
 * @public @memberof xrt_allocation_collection
 */
static inline xrt_result_t
xrt_allocation_collection_get_vk_device(struct xrt_allocation_collection *xac, VkDevice *out_device)
{
	return xac->get_property_raw(xac, XRT_ALLOCATION_PROPERTY_VK_DEVICE, out_device);
}

/*!
 * Typed helper to get all the VkImages from an allocation collection, see
 * @ref XRT_ALLOCATION_TYPE_VULKAN_IMAGE for lifetime.
 *
 * Does type checking and ownership checking, the VkImages are only returned if
 * @p device is the VkDevice the images were created on, otherwise
 * @ref XRT_ERROR_INVALID_CONTEXT_FOR_TYPE is returned.
 *
 * @param xac The allocation collection.
 * @param device The VkDevice the caller will use the images with.
 * @param[out] out_images Array of at least @ref xrt_allocation_collection::image_count elements.
 *
 * @public @memberof xrt_allocation_collection
 */
static inline xrt_result_t
xrt_allocation_collection_get_vk_images(struct xrt_allocation_collection *xac, VkDevice device, VkImage *out_images)
{
	VkDevice xac_device = VK_NULL_HANDLE;
	xrt_result_t xret = xrt_allocation_collection_get_vk_device(xac, &xac_device);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	if (xac_device != device) {
		return XRT_ERROR_INVALID_CONTEXT_FOR_TYPE;
	}

	return xac->get_all_raw(xac, XRT_ALLOCATION_TYPE_VULKAN_IMAGE, out_images);
}

/*!
 * @}
 */

#ifdef __cplusplus
}
#endif
