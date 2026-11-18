// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Helpers for @ref xrt_allocation_collection.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_allocation_collection.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Is @p prop in the supported properties of @p xac.
 *
 * @ingroup aux_util
 */
static inline bool
u_allocation_collection_supports_property(const struct xrt_allocation_collection *xac,
                                          enum xrt_allocation_property prop)
{
	for (uint32_t i = 0; i < xac->supported_properties_count; i++) {
		if (xac->supported_properties[i].property == prop) {
			return true;
		}
	}
	return false;
}

/*!
 * Is @p type in the supported allocation types of @p xac.
 *
 * @ingroup aux_util
 */
static inline bool
u_allocation_collection_supports_type(const struct xrt_allocation_collection *xac, enum xrt_allocation_type type)
{
	for (uint32_t i = 0; i < xac->supported_types_count; i++) {
		if (xac->supported_types[i].type == type) {
			return true;
		}
	}
	return false;
}

#ifdef __cplusplus
}
#endif
