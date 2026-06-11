// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DESIGN EXPLORATION (Option F): the Vulkan interface.
 *
 * This header is a design draft and is intentionally NOT wired into the build.
 * Companion to @ref xrt_allocation_collection_f_direct.h. It is allowed to
 * include vulkan.h, so the Vulkan handles get REAL types while the core header
 * stays neutral.
 *
 * The base struct holds a `const struct xrt_ac_f_vk *vk;` member (NULL if
 * unsupported). Consumers do: if (xac->vk && xac->vk->has_images) { ... }
 *
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_allocation_collection_f_direct.h"

// In a real version: #include <vulkan/vulkan.h> + struct vk_bundle.
typedef struct VkImage_T *VkImage;
typedef struct VkBuffer_T *VkBuffer;
struct vk_bundle;


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Vulkan interface. A non-NULL @ref xrt_allocation_collection_f::vk means "this
 * collection is Vulkan-backed". Fully typed because this header may include the
 * Vulkan headers.
 *
 * The capability flags describe what is obtainable; read them before calling.
 * @ref has_images and @ref has_buffers may be mutually exclusive, and
 * @ref has_bundle is not implied by either. An op whose flag is true is
 * guaranteed to succeed.
 */
struct xrt_ac_f_vk
{
	bool has_images;  //!< get_images is valid.
	bool has_buffers; //!< get_buffers is valid.
	bool has_bundle;  //!< get_bundle is valid.

	/*!
	 * Get all VkImage handles. Valid iff @ref has_images.
	 * Lifetime: BORROW; the collection owns them. Do not vkDestroyImage.
	 */
	xrt_result_t (*get_images)(struct xrt_allocation_collection_f *xac, uint32_t image_count, VkImage *out_images);

	/*!
	 * Get all VkBuffer handles. Valid iff @ref has_buffers.
	 * Lifetime: BORROW; the collection owns them. Do not vkDestroyBuffer.
	 */
	xrt_result_t (*get_buffers)(struct xrt_allocation_collection_f *xac,
	                            uint32_t image_count,
	                            VkBuffer *out_buffers);

	/*!
	 * Get the vk_bundle that backs this collection. Valid iff @ref has_bundle.
	 * Lifetime: BORROW.
	 */
	xrt_result_t (*get_bundle)(struct xrt_allocation_collection_f *xac, struct vk_bundle **out_vk);
};

#ifdef __cplusplus
}
#endif
