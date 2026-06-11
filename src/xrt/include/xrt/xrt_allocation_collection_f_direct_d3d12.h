// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DESIGN EXPLORATION (Option F): the D3D12 interface.
 *
 * This header is a design draft and is intentionally NOT wired into the build.
 * Companion to @ref xrt_allocation_collection_f_direct.h. It is allowed to
 * include the D3D12 headers, so the D3D12 handles get REAL types while the core
 * header stays neutral.
 *
 * The base struct holds a `const struct xrt_ac_f_d3d12 *d3d12;` member (NULL if
 * unsupported). Consumers do: if (xac->d3d12) xac->d3d12->get_textures(...);
 *
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_allocation_collection_f_direct.h"

// In a real version: #include <d3d12.h> to get ID3D12Resource.
struct ID3D12Resource;


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * D3D12 interface. A non-NULL @ref xrt_allocation_collection_f::d3d12 points at
 * one of these. Fully typed because this header may include the D3D12 headers.
 */
struct xrt_ac_f_d3d12
{
	/*!
	 * Get all D3D12 texture handles. Lifetime: BORROW; the collection holds a
	 * COM reference. Call AddRef/Release if you need them longer.
	 *
	 * @param image_count  Must equal xac->image_count.
	 * @param out_textures Array of at least image_count ID3D12Resource*.
	 */
	xrt_result_t (*get_textures)(struct xrt_allocation_collection_f *xac,
	                             uint32_t image_count,
	                             struct ID3D12Resource **out_textures);
};

#ifdef __cplusplus
}
#endif
