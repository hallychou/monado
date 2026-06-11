// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DESIGN EXPLORATION (Option C): the D3D12 sub-interface.
 *
 * This header is a design draft and is intentionally NOT wired into the build.
 * Companion to @ref xrt_allocation_collection_c_com.h. It is allowed to include
 * the D3D12 headers, so the D3D12 handles get REAL types (ID3D12Resource) while
 * the neutral core header stays platform-agnostic. Only code that already deals
 * with D3D12 includes this header and calls query_interface(XRT_AC_IFACE_D3D12).
 *
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_allocation_collection_c_com.h"

// In a real version: #include <d3d12.h> to get ID3D12Resource. Forward declared
// here to keep the draft self-contained.
struct ID3D12Resource;


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * D3D12 sub-interface, returned by query_interface(XRT_AC_IFACE_D3D12).
 * Fully typed because this header may include the D3D12 headers.
 */
struct xrt_ac_d3d12_interface
{
	/*!
	 * Get all D3D12 texture handles. Lifetime: BORROW; the collection holds
	 * a COM reference. Call AddRef/Release if you need them longer.
	 *
	 * @param out_textures Array of at least image_count ID3D12Resource*.
	 */
	xrt_result_t (*get_textures)(struct xrt_allocation_collection_c *xac,
	                             uint32_t image_count,
	                             struct ID3D12Resource **out_textures);
};

/*!
 * Typed helper: query the D3D12 interface in one call.
 */
static inline xrt_result_t
xrt_allocation_collection_c_get_d3d12(struct xrt_allocation_collection_c *xac,
                                      const struct xrt_ac_d3d12_interface **out_d3d12)
{
	return xac->query_interface(xac, XRT_AC_IFACE_D3D12, (const void **)out_d3d12);
}

#ifdef __cplusplus
}
#endif
