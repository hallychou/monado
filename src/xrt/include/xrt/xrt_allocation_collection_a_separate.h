// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DESIGN EXPLORATION (Option A): separate functions.
 *
 * This header is a design draft and is intentionally NOT wired into the build.
 * It explores replacing the generic enum + void* dispatchers (get_property /
 * get_all) with one function pointer per concern.
 *
 * Discovery model (per review feedback): the original "capability == non-NULL
 * function pointer" idea is NOT allowed -- you must not discover support by
 * probing a function pointer, nor by calling and inspecting an error. So this
 * variant now carries an explicit @ref xrt_allocation_collection_a::supported
 * descriptor (plain data) that you read up front; every function pointer is
 * always non-NULL, and an op whose flag is true is guaranteed to succeed.
 *
 * NOTE: this requirement removes Option A's main selling point. The `supported`
 * descriptor now duplicates the function list (every op needs a matching flag),
 * which is exactly the kind of parallel bookkeeping the "just separate
 * functions" idea was trying to avoid.
 *
 * The platform-neutral concerns are fully typed; the platform handle getters
 * still take void* + element_size, because this header must not include
 * vulkan.h / Metal / d3d12 headers.
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
 * An allocation collection is a collection of images.
 *
 * This interface does not provide any synchronization.
 */
struct xrt_allocation_collection_a
{
	struct xrt_reference reference;
	uint32_t image_count;

	/*!
	 * Capability descriptor (plain data). Read these BEFORE calling. An op
	 * whose flag is true is guaranteed to succeed; the function pointers are
	 * always non-NULL, so support is NEVER discovered from the pointer or
	 * from an error return.
	 *
	 * Note how each flag mirrors a function below -- e.g. vk_image and
	 * vk_buffer are separate (they may be mutually exclusive), and vk_bundle
	 * is independent of both.
	 */
	struct
	{
		bool swapchain_info;
		bool native_images;
		bool cpu_pointers;
		bool dxgi_handles;
		bool win32_handles;
		bool vk_image;
		bool vk_buffer;
		bool vk_bundle;
		bool metal_texture;
		bool iosurface;
		bool metal_device;
		bool d3d12_texture;
	} supported;


	/*
	 *
	 * Platform-neutral getters (fully typed).
	 *
	 */

	//! Valid iff supported.swapchain_info.
	xrt_result_t (*get_swapchain_info)(struct xrt_allocation_collection_a *xac,
	                                   struct xrt_swapchain_create_info *out_info);

	//! Valid iff supported.native_images. Lifetime: MOVE (caller closes/unrefs all).
	xrt_result_t (*get_native_images)(struct xrt_allocation_collection_a *xac,
	                                  uint32_t image_count,
	                                  struct xrt_image_native *out_images);

	//! Valid iff supported.cpu_pointers. Lifetime: BORROW (do not free/munmap).
	xrt_result_t (*get_cpu_pointers)(struct xrt_allocation_collection_a *xac,
	                                 uint32_t image_count,
	                                 void **out_pointers);

	//! Valid iff supported.dxgi_handles. Lifetime: MOVE (caller CloseHandle/unrefs).
	xrt_result_t (*get_dxgi_handles)(struct xrt_allocation_collection_a *xac,
	                                 uint32_t image_count,
	                                 struct xrt_image_native *out_handles);

	//! Valid iff supported.win32_handles. Lifetime: MOVE (caller CloseHandle/unrefs).
	xrt_result_t (*get_win32_handles)(struct xrt_allocation_collection_a *xac,
	                                  uint32_t image_count,
	                                  struct xrt_image_native *out_handles);


	/*
	 *
	 * Platform handle getters (void* because this header is platform-neutral).
	 *
	 */

	//! Valid iff supported.vk_image. element_size must be sizeof(VkImage). BORROW.
	xrt_result_t (*get_vk_images)(struct xrt_allocation_collection_a *xac, size_t element_size, void *ptr);

	//! Valid iff supported.vk_buffer. element_size must be sizeof(VkBuffer). BORROW.
	xrt_result_t (*get_vk_buffers)(struct xrt_allocation_collection_a *xac, size_t element_size, void *ptr);

	//! Valid iff supported.metal_texture. BORROW (retain/release if longer).
	xrt_result_t (*get_metal_textures)(struct xrt_allocation_collection_a *xac, size_t element_size, void *ptr);

	//! Valid iff supported.iosurface. BORROW (CFRetain/CFRelease if longer).
	xrt_result_t (*get_iosurfaces)(struct xrt_allocation_collection_a *xac, size_t element_size, void *ptr);

	//! Valid iff supported.d3d12_texture. element_size = sizeof(ID3D12Resource*). BORROW (COM).
	xrt_result_t (*get_d3d12_textures)(struct xrt_allocation_collection_a *xac, size_t element_size, void *ptr);


	/*
	 *
	 * Single-value platform "properties" (void** out).
	 *
	 */

	//! Valid iff supported.vk_bundle. Writes a `struct vk_bundle *`. BORROW.
	xrt_result_t (*get_vk_bundle)(struct xrt_allocation_collection_a *xac, void **out);

	//! Valid iff supported.metal_device. Writes the MTLDevice pointer. BORROW.
	xrt_result_t (*get_metal_device)(struct xrt_allocation_collection_a *xac, void **out);

	/*!
	 * Destroy the collection, called when the reference count reaches zero.
	 */
	void (*destroy)(struct xrt_allocation_collection_a *xac);
};

static inline void
xrt_allocation_collection_a_reference(struct xrt_allocation_collection_a **dst, struct xrt_allocation_collection_a *src)
{
	struct xrt_allocation_collection_a *old_dst = *dst;

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
