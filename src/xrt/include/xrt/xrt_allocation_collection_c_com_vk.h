// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DESIGN EXPLORATION (Option C): the Vulkan sub-interface.
 *
 * This header is a design draft and is intentionally NOT wired into the build.
 * It is the companion to @ref xrt_allocation_collection_c_com.h and shows the
 * key payoff of the COM model: this optional header is allowed to include
 * vulkan.h, so the Vulkan handles get REAL types (VkImage, struct vk_bundle)
 * while the neutral core header stays platform-agnostic.
 *
 * Only code that already deals with Vulkan includes this header and calls
 * query_interface(XRT_AC_IFACE_VK).
 *
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_allocation_collection_c_com.h"

// In a real version this would be: #include "vk/vk_helpers.h" / <vulkan/vulkan.h>
// to get VkImage, VkBuffer and struct vk_bundle. Shown as forward decls to keep
// the draft self-contained.
typedef struct VkImage_T *VkImage;
typedef struct VkBuffer_T *VkBuffer;
struct vk_bundle;


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Vulkan sub-interface, returned by query_interface(XRT_AC_IFACE_VK).
 * Fully typed because this header may include the Vulkan headers.
 *
 * The capability flags describe what is obtainable; read them before calling.
 * They exist because the sub-capabilities are independent / exclusive:
 * @ref has_images and @ref has_buffers may be mutually exclusive, and
 * @ref has_bundle is not implied by either. An op whose flag is true is
 * guaranteed to succeed.
 */
struct xrt_ac_vk_interface
{
	bool has_images;  //!< get_images is valid.
	bool has_buffers; //!< get_buffers is valid.
	bool has_bundle;  //!< get_bundle is valid.

	/*!
	 * Get all VkImage handles. Valid iff @ref has_images.
	 * Lifetime: BORROW; the collection owns them. Do not vkDestroyImage.
	 *
	 * @param out_images Array of at least image_count VkImage.
	 */
	xrt_result_t (*get_images)(struct xrt_allocation_collection_c *xac, uint32_t image_count, VkImage *out_images);

	/*!
	 * Get all VkBuffer handles. Valid iff @ref has_buffers.
	 * Lifetime: BORROW; the collection owns them. Do not vkDestroyBuffer.
	 */
	xrt_result_t (*get_buffers)(struct xrt_allocation_collection_c *xac,
	                            uint32_t image_count,
	                            VkBuffer *out_buffers);

	/*!
	 * Get the vk_bundle that backs this collection. Valid iff @ref has_bundle.
	 * Lifetime: BORROW.
	 */
	xrt_result_t (*get_bundle)(struct xrt_allocation_collection_c *xac, struct vk_bundle **out_vk);
};

/*!
 * Typed helper: query the Vulkan interface in one call.
 */
static inline xrt_result_t
xrt_allocation_collection_c_get_vk(struct xrt_allocation_collection_c *xac, const struct xrt_ac_vk_interface **out_vk)
{
	return xac->query_interface(xac, XRT_AC_IFACE_VK, (const void **)out_vk);
}

#ifdef __cplusplus
}
#endif
