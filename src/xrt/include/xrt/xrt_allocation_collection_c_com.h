// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DESIGN EXPLORATION (Option C): COM-style queryable interfaces.
 *
 * This header is a design draft and is intentionally NOT wired into the build.
 * It explores a COM-like model: a small base with a single @p query_interface
 * entry point that hands back cohesive, typed sub-interfaces grouped by
 * concern / platform.
 *
 * Strengths: a collection can expose several interfaces at once (fixes Option
 * B's single-identity problem), and each interface can live in its OWN optional
 * header that is allowed to include vulkan.h / Metal headers — so the platform
 * handles get real types without polluting this neutral core. See the companion
 * @ref xrt_allocation_collection_c_com_vk.h.
 *
 * Discovery model (per review feedback): availability of an interface is read
 * as DATA from @ref xrt_allocation_collection_c::supported_interfaces up front.
 * @p query_interface is then only ever called for a listed id and always
 * succeeds for one -- it is NOT used as an error-based "is this supported?"
 * probe. Multi-op interfaces additionally carry their own capability flags
 * (e.g. @ref xrt_ac_vk_interface::has_images / has_buffers / has_bundle),
 * because those sub-capabilities can be exclusive or independent.
 *
 * Cost: @p query_interface needs an identifier, so the central
 * @ref xrt_ac_interface_id enum comes back (the thing some variants try to
 * avoid), plus a query-then-call indirection. Note that requiring a data list
 * of available interfaces brings back an enum array + count very much like the
 * original supported_types[]/supported_properties[] design, so under this
 * discovery constraint Option C converges toward "Option E plus an extra
 * indirection".
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
 * Interface identifier passed to @ref xrt_allocation_collection_c::query_interface.
 *
 * The concrete sub-interface struct for each id is declared in its own header
 * (e.g. @ref xrt_ac_vk_interface in xrt_allocation_collection_c_com_vk.h). The
 * neutral interfaces below can stay in this header.
 */
enum xrt_ac_interface_id
{
	XRT_AC_IFACE_INFO,   //!< -> struct xrt_ac_info_interface (this header)
	XRT_AC_IFACE_NATIVE, //!< -> struct xrt_ac_native_interface (this header)
	XRT_AC_IFACE_CPU,    //!< -> struct xrt_ac_cpu_interface (this header)
	XRT_AC_IFACE_DXGI,   //!< -> struct xrt_ac_dxgi_interface (this header)
	XRT_AC_IFACE_WIN32,  //!< -> struct xrt_ac_win32_interface (this header)
	XRT_AC_IFACE_VK,     //!< -> struct xrt_ac_vk_interface (vk header)
	XRT_AC_IFACE_METAL,  //!< -> struct xrt_ac_metal_interface (metal header)
	XRT_AC_IFACE_D3D12,  //!< -> struct xrt_ac_d3d12_interface (d3d12 header)
};

struct xrt_allocation_collection_c;

/*!
 * Neutral interface: swapchain create info.
 */
struct xrt_ac_info_interface
{
	xrt_result_t (*get_swapchain_info)(struct xrt_allocation_collection_c *xac,
	                                   struct xrt_swapchain_create_info *out_info);
};

/*!
 * Neutral interface: native image handles (MOVE semantics, see below).
 */
struct xrt_ac_native_interface
{
	/*!
	 * Lifetime: MOVE. On success ownership of all handles transfers to the
	 * caller. All-or-nothing.
	 */
	xrt_result_t (*get_images)(struct xrt_allocation_collection_c *xac,
	                           uint32_t image_count,
	                           struct xrt_image_native *out_images);
};

/*!
 * Neutral interface: CPU-accessible host pointers.
 */
struct xrt_ac_cpu_interface
{
	//! Lifetime: BORROW. The collection owns the mapping; do not free/munmap.
	xrt_result_t (*get_pointers)(struct xrt_allocation_collection_c *xac,
	                             uint32_t image_count,
	                             void **out_pointers);
};

/*!
 * Neutral interface: DXGI shared resource handles (carried in
 * @ref xrt_image_native, is_dxgi_handle set).
 */
struct xrt_ac_dxgi_interface
{
	//! Lifetime: MOVE. Caller must CloseHandle/unref every handle.
	xrt_result_t (*get_handles)(struct xrt_allocation_collection_c *xac,
	                            uint32_t image_count,
	                            struct xrt_image_native *out_handles);
};

/*!
 * Neutral interface: opaque external-memory Win32 HANDLEs.
 */
struct xrt_ac_win32_interface
{
	//! Lifetime: MOVE. Caller must CloseHandle/unref every handle.
	xrt_result_t (*get_handles)(struct xrt_allocation_collection_c *xac,
	                            uint32_t image_count,
	                            struct xrt_image_native *out_handles);
};

/*!
 * Base object. Everything is reached through @p query_interface.
 *
 * The returned interface pointer BORROWS the lifetime of the collection: it is
 * valid only while the collection is alive and shares its reference count. The
 * caller does not release the interface separately.
 */
struct xrt_allocation_collection_c
{
	struct xrt_reference reference;
	uint32_t image_count;

	/*!
	 * Data list of which interfaces this collection implements. Read this up
	 * front to discover support; do not probe via @p query_interface errors.
	 * (This array + count is essentially the original supported_*[] pattern.)
	 */
	enum xrt_ac_interface_id supported_interfaces[8];
	uint32_t supported_interface_count;

	/*!
	 * Return a pointer to the requested interface. Only valid for an @p iid
	 * present in @ref supported_interfaces, in which case it always succeeds.
	 * @p out_iface is cast to the concrete interface struct for @p iid.
	 */
	xrt_result_t (*query_interface)(struct xrt_allocation_collection_c *xac,
	                                enum xrt_ac_interface_id iid,
	                                const void **out_iface);

	void (*destroy)(struct xrt_allocation_collection_c *xac);
};

/*!
 * Typed convenience wrapper around @p query_interface.
 */
static inline xrt_result_t
xrt_allocation_collection_c_query(struct xrt_allocation_collection_c *xac,
                                  enum xrt_ac_interface_id iid,
                                  const void **out_iface)
{
	return xac->query_interface(xac, iid, out_iface);
}

static inline void
xrt_allocation_collection_c_reference(struct xrt_allocation_collection_c **dst, struct xrt_allocation_collection_c *src)
{
	struct xrt_allocation_collection_c *old_dst = *dst;

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
