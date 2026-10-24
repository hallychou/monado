// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DESIGN EXPLORATION (Option E): the recommended mix.
 *
 * This header is a design draft and is intentionally NOT wired into the build.
 *
 * Discovery model (per review feedback):
 *   - Support is described by EXPLICIT DATA you read before calling: the
 *     @ref xrt_allocation_collection_e::supported flags for neutral ops, and a
 *     per-interface capability descriptor for platform ops (e.g.
 *     @ref xrt_ac_vk::has_images / has_buffers / has_bundle).
 *   - You NEVER discover support by probing whether a function pointer is set,
 *     and you NEVER discover it by calling and inspecting an error code. Every
 *     advertised op is guaranteed to succeed; a correct caller never sees an
 *     "unsupported" error.
 *   - The PRESENCE of a sub-interface object (xac->vk != NULL) is an allowed
 *     coarse signal ("this collection is Vulkan-backed"). But the fine-grained,
 *     possibly-exclusive capabilities inside it (VkImage vs VkBuffer, and
 *     whether a vk_bundle is obtainable) are described by data fields, because
 *     e.g. VkBuffer and VkImage may be mutually exclusive and a vk_bundle is
 *     not guaranteed just because images/buffers are.
 *
 * The platform structs are fully typed in their own optional headers
 * (xrt_allocation_collection_e_mix_vk.h, ..._d3d12.h); this core stays neutral.
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

//! Platform sub-interfaces, fully defined in their own optional headers.
struct xrt_ac_vk;
struct xrt_ac_metal;
struct xrt_ac_d3d12;

/*!
 * An allocation collection is a collection of images.
 *
 * This interface does not provide any synchronization.
 */
struct xrt_allocation_collection_e
{
	struct xrt_reference reference;
	uint32_t image_count;

	/*!
	 * Capability descriptor for the neutral ops below (plain data). Read
	 * these BEFORE calling; an op whose flag is true is guaranteed to
	 * succeed. The matching function pointer is always non-NULL regardless,
	 * so support is discovered from these flags, never from the pointer.
	 */
	struct
	{
		bool swapchain_info;
		bool native_images;
		bool cpu_pointers;
		bool dxgi_handles;
		bool win32_handles;
	} supported;


	/*
	 *
	 * Platform-neutral ops. Always callable when the matching `supported`
	 * flag is set. These are also the only members that cross IPC.
	 *
	 */

	/*!
	 * Copy the @ref xrt_swapchain_create_info used to create this
	 * collection. Valid iff @ref supported.swapchain_info.
	 */
	xrt_result_t (*get_swapchain_info)(struct xrt_allocation_collection_e *xac,
	                                   struct xrt_swapchain_create_info *out_info);

	/*!
	 * Get all native image handles. Valid iff @ref supported.native_images.
	 *
	 * Lifetime: MOVE. On success ownership of all handles transfers to the
	 * caller, who MUST close/unref every handle. All-or-nothing.
	 */
	xrt_result_t (*get_native_images)(struct xrt_allocation_collection_e *xac,
	                                  uint32_t image_count,
	                                  struct xrt_image_native *out_images);

	/*!
	 * Get all CPU-accessible host pointers. Valid iff
	 * @ref supported.cpu_pointers. Lifetime: BORROW (do not free/munmap).
	 */
	xrt_result_t (*get_cpu_pointers)(struct xrt_allocation_collection_e *xac,
	                                 uint32_t image_count,
	                                 void **out_pointers);

	/*!
	 * Get all DXGI shared resource handles (carried in @ref xrt_image_native,
	 * is_dxgi_handle set). Valid iff @ref supported.dxgi_handles.
	 * Lifetime: MOVE (caller CloseHandle/unref).
	 */
	xrt_result_t (*get_dxgi_handles)(struct xrt_allocation_collection_e *xac,
	                                 uint32_t image_count,
	                                 struct xrt_image_native *out_handles);

	/*!
	 * Get all opaque external-memory Win32 HANDLEs (carried in
	 * @ref xrt_image_native). Valid iff @ref supported.win32_handles.
	 * Lifetime: MOVE (caller CloseHandle/unref).
	 */
	xrt_result_t (*get_win32_handles)(struct xrt_allocation_collection_e *xac,
	                                  uint32_t image_count,
	                                  struct xrt_image_native *out_handles);


	/*
	 *
	 * Platform sub-interfaces. A non-NULL pointer is the coarse "this
	 * collection is X-backed" signal; the fine-grained caps live as data
	 * inside each interface struct (see the companion headers).
	 *
	 */

	//! Vulkan access, or NULL. Defined in xrt_allocation_collection_e_mix_vk.h.
	const struct xrt_ac_vk *vk;

	//! Metal access, or NULL. Defined in xrt_allocation_collection_e_mix_metal.h.
	const struct xrt_ac_metal *metal;

	//! D3D12 access, or NULL. Defined in xrt_allocation_collection_e_mix_d3d12.h.
	const struct xrt_ac_d3d12 *d3d12;


	/*!
	 * Destroy the collection, called when the reference count reaches zero.
	 */
	void (*destroy)(struct xrt_allocation_collection_e *xac);
};

static inline void
xrt_allocation_collection_e_reference(struct xrt_allocation_collection_e **dst, struct xrt_allocation_collection_e *src)
{
	struct xrt_allocation_collection_e *old_dst = *dst;

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
