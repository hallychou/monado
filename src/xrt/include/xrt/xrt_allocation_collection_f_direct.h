// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DESIGN EXPLORATION (Option F): direct interface pointers, no query.
 *
 * This header is a design draft and is intentionally NOT wired into the build.
 *
 * Option F is a cross of C and E: like C (COM) it groups every concern into its
 * own cohesive interface struct, but like E it reaches them through DIRECT,
 * nullable pointers on the collection -- there is NO query_interface call and NO
 * interface-id enum. The collection only keeps a @p destroy of its own.
 *
 * Discovery model (satisfies the review constraints):
 *   - An interface is supported iff its pointer is non-NULL. Interface-object
 *     presence is the blessed coarse signal; there is no function-pointer
 *     probing of individual ops and no error-based "is this supported?" path.
 *   - Multi-op interfaces whose sub-capabilities can be exclusive or
 *     independent (e.g. Vulkan: VkImage vs VkBuffer, and vk_bundle) carry their
 *     own has_* capability flags as plain data; an op whose flag is true is
 *     guaranteed to succeed.
 *
 * Neutral interfaces are fully defined in this header. Platform interfaces are
 * forward-declared here and fully typed in their own optional headers
 * (xrt_allocation_collection_f_direct_vk.h, ..._d3d12.h), which may include
 * vulkan.h / d3d12.h, so this core stays platform-neutral.
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

struct xrt_allocation_collection_f;

//! Platform interfaces, fully defined in their own optional headers.
struct xrt_ac_f_vk;
struct xrt_ac_f_metal;
struct xrt_ac_f_d3d12;


/*
 *
 * Neutral interfaces (each cohesive; presence of the pointer == supported).
 *
 */

/*!
 * Swapchain create info interface.
 */
struct xrt_ac_f_info
{
	xrt_result_t (*get_swapchain_info)(struct xrt_allocation_collection_f *xac,
	                                   struct xrt_swapchain_create_info *out_info);
};

/*!
 * Native image handle interface.
 */
struct xrt_ac_f_native
{
	//! Lifetime: MOVE. On success ownership transfers to the caller; all-or-nothing.
	xrt_result_t (*get_images)(struct xrt_allocation_collection_f *xac,
	                           uint32_t image_count,
	                           struct xrt_image_native *out_images);
};

/*!
 * CPU-accessible host pointer interface.
 */
struct xrt_ac_f_cpu
{
	//! Lifetime: BORROW. The collection owns the mapping; do not free/munmap.
	xrt_result_t (*get_pointers)(struct xrt_allocation_collection_f *xac,
	                             uint32_t image_count,
	                             void **out_pointers);
};

/*!
 * DXGI shared resource handle interface (carried in @ref xrt_image_native,
 * is_dxgi_handle set).
 */
struct xrt_ac_f_dxgi
{
	//! Lifetime: MOVE. Caller must CloseHandle/unref every handle.
	xrt_result_t (*get_handles)(struct xrt_allocation_collection_f *xac,
	                            uint32_t image_count,
	                            struct xrt_image_native *out_handles);
};

/*!
 * Opaque external-memory Win32 HANDLE interface.
 */
struct xrt_ac_f_win32
{
	//! Lifetime: MOVE. Caller must CloseHandle/unref every handle.
	xrt_result_t (*get_handles)(struct xrt_allocation_collection_f *xac,
	                            uint32_t image_count,
	                            struct xrt_image_native *out_handles);
};


/*!
 * An allocation collection is a collection of images.
 *
 * Every capability is a nullable interface pointer below; a non-NULL pointer
 * means that interface is supported. The collection itself only owns @p destroy.
 *
 * This interface does not provide any synchronization.
 */
struct xrt_allocation_collection_f
{
	struct xrt_reference reference;
	uint32_t image_count;

	//! Neutral interfaces, or NULL.
	const struct xrt_ac_f_info *info;
	const struct xrt_ac_f_native *native;
	const struct xrt_ac_f_cpu *cpu;
	const struct xrt_ac_f_dxgi *dxgi;
	const struct xrt_ac_f_win32 *win32;

	//! Platform interfaces, or NULL (defined in the optional companion headers).
	const struct xrt_ac_f_vk *vk;
	const struct xrt_ac_f_metal *metal;
	const struct xrt_ac_f_d3d12 *d3d12;

	/*!
	 * Destroy the collection, called when the reference count reaches zero.
	 */
	void (*destroy)(struct xrt_allocation_collection_f *xac);
};

static inline void
xrt_allocation_collection_f_reference(struct xrt_allocation_collection_f **dst, struct xrt_allocation_collection_f *src)
{
	struct xrt_allocation_collection_f *old_dst = *dst;

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
