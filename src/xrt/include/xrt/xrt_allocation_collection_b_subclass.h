// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DESIGN EXPLORATION (Option B): typed subclasses.
 *
 * This header is a design draft and is intentionally NOT wired into the build.
 * It explores giving the collection typed "subclasses" the way swapchains have
 * xrt_swapchain_vk / xrt_swapchain_gl etc.
 *
 * The neutral, cross-cutting ops (swapchain info, native images, CPU pointers,
 * DXGI / Win32 handle export) live on the BASE -- they are not specific to any
 * platform, so they must not be duplicated into a platform subclass. (An
 * earlier draft wrongly put get_cpu_pointers on the Vulkan subclass; that is
 * fixed here.) Platform-typed ops live on the subclasses.
 *
 * Discovery model (per review feedback): the @ref kind tag tells you which
 * subclass to downcast to; within each level, capability flags (has_*) tell you
 * which ops are valid as plain data, so support is never discovered by probing
 * a function pointer or by calling and getting an error.
 *
 * KNOWN PROBLEM (still): @ref kind forces a single identity. A real collection
 * is often several things at once (e.g. Vulkan images that also export native
 * handles), so you still need the runtime tag plus downcasting -- i.e. the enum
 * the redesign was trying to remove, reappearing as a downcast discriminator.
 *
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_limits.h"


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @addtogroup xrt_iface
 * @{
 */

/*!
 * Runtime discriminator needed to safely downcast the base to a concrete
 * subclass. (This is the enum the redesign was trying to avoid.)
 */
enum xrt_allocation_collection_b_kind
{
	XRT_AC_B_KIND_VK,
	XRT_AC_B_KIND_METAL,
	XRT_AC_B_KIND_D3D12,
};

/*!
 * Common base. Holds the neutral, cross-cutting ops shared by every subclass,
 * each gated by a has_* capability flag. Callers inspect @ref kind, then
 * downcast to the matching concrete struct below for platform-typed ops.
 */
struct xrt_allocation_collection_b
{
	struct xrt_reference reference;
	uint32_t image_count;

	enum xrt_allocation_collection_b_kind kind;

	bool has_swapchain_info; //!< get_swapchain_info is valid.
	bool has_native_images;  //!< get_native_images is valid (MOVE).
	bool has_cpu_pointers;   //!< get_cpu_pointers is valid (BORROW).
	bool has_dxgi_handles;   //!< get_dxgi_handles is valid (MOVE).
	bool has_win32_handles;  //!< get_win32_handles is valid (MOVE).

	xrt_result_t (*get_swapchain_info)(struct xrt_allocation_collection_b *xac,
	                                   struct xrt_swapchain_create_info *out_info);

	//! Lifetime: MOVE. Caller closes/unrefs all handles.
	xrt_result_t (*get_native_images)(struct xrt_allocation_collection_b *xac,
	                                  uint32_t image_count,
	                                  struct xrt_image_native *out_images);

	//! Lifetime: BORROW. Do not free/munmap. (Neutral: lives on the base.)
	xrt_result_t (*get_cpu_pointers)(struct xrt_allocation_collection_b *xac,
	                                 uint32_t image_count,
	                                 void **out_pointers);

	//! Lifetime: MOVE. DXGI shared resource handles (in xrt_image_native).
	xrt_result_t (*get_dxgi_handles)(struct xrt_allocation_collection_b *xac,
	                                 uint32_t image_count,
	                                 struct xrt_image_native *out_handles);

	//! Lifetime: MOVE. Opaque external-memory Win32 HANDLEs.
	xrt_result_t (*get_win32_handles)(struct xrt_allocation_collection_b *xac,
	                                  uint32_t image_count,
	                                  struct xrt_image_native *out_handles);

	void (*destroy)(struct xrt_allocation_collection_b *xac);
};

/*!
 * Vulkan subclass. Only Vulkan-typed ops live here. VkImage / vk_bundle would
 * force this struct into an optional, vulkan-aware header
 * (xrt_allocation_collection_b_vk.h) in a real version; shown as void* here to
 * keep the draft compiling.
 *
 * has_images and has_buffers may be mutually exclusive, and has_bundle is not
 * implied by either -- hence three independent flags.
 */
struct xrt_allocation_collection_b_vk
{
	struct xrt_allocation_collection_b base;

	bool has_images;  //!< get_images is valid.
	bool has_buffers; //!< get_buffers is valid.
	bool has_bundle;  //!< get_bundle is valid.

	//! Lifetime: BORROW. (Would be `VkImage *`.)
	xrt_result_t (*get_images)(struct xrt_allocation_collection_b_vk *xac, size_t element_size, void *ptr);

	//! Lifetime: BORROW. (Would be `VkBuffer *`.)
	xrt_result_t (*get_buffers)(struct xrt_allocation_collection_b_vk *xac, size_t element_size, void *ptr);

	//! Lifetime: BORROW. (Would be `struct vk_bundle **`.)
	xrt_result_t (*get_bundle)(struct xrt_allocation_collection_b_vk *xac, void **out);
};

/*!
 * Metal subclass.
 */
struct xrt_allocation_collection_b_metal
{
	struct xrt_allocation_collection_b base;

	bool has_textures;   //!< get_textures is valid.
	bool has_iosurfaces; //!< get_iosurfaces is valid.
	bool has_device;     //!< get_device is valid.

	//! Lifetime: BORROW. (Would be MTLTexture* / IOSurfaceRef* in a metal header.)
	xrt_result_t (*get_textures)(struct xrt_allocation_collection_b_metal *xac, size_t element_size, void *ptr);
	xrt_result_t (*get_iosurfaces)(struct xrt_allocation_collection_b_metal *xac, size_t element_size, void *ptr);
	xrt_result_t (*get_device)(struct xrt_allocation_collection_b_metal *xac, void **out);
};

/*!
 * D3D12 subclass. Only D3D12-typed ops live here; the neutral DXGI / Win32 /
 * native / CPU exports are on the base, NOT duplicated here. ID3D12Resource
 * would force this struct into an optional d3d12-aware header in a real version.
 */
struct xrt_allocation_collection_b_d3d12
{
	struct xrt_allocation_collection_b base;

	bool has_textures; //!< get_textures is valid.

	//! Lifetime: BORROW. (Would be `ID3D12Resource **`.) COM AddRef/Release if longer.
	xrt_result_t (*get_textures)(struct xrt_allocation_collection_b_d3d12 *xac, size_t element_size, void *ptr);
};

static inline void
xrt_allocation_collection_b_reference(struct xrt_allocation_collection_b **dst, struct xrt_allocation_collection_b *src)
{
	struct xrt_allocation_collection_b *old_dst = *dst;

	if (old_dst == src) {
		return;
	}

	if (src) {
		xrt_reference_inc(&src->reference);
	}

	*dst = src;

	if (old_dst) {
		if (xrt_reference_dec_and_is_zero(&old_dst->reference)) {
			old_dst->destroy(old_dst);
		}
	}
}

/*!
 * @}
 */

#ifdef __cplusplus
}
#endif
