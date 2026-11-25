// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Generator for per-process unique @ref xrt_tracking_origin_id values.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup aux_util
 */

#include "util/u_tracking_origin_id.h"
#include "util/u_limited_unique_id.h"


struct xrt_tracking_origin_id
u_tracking_origin_id_generate(void)
{
	/*
	 * Tracking origin IDs are only generated in one process, either the
	 * service for out-of-process or the app for in-process mode, so the
	 * per-process guarantee of the limited unique ID is enough. Sharing
	 * the generator also keeps them unique against device IDs and other
	 * users of it.
	 */
	return XRT_C11_COMPOUND(struct xrt_tracking_origin_id){
	    .val = u_limited_unique_id_get().data,
	};
}
