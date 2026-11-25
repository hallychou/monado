// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Generator for per-process unique @ref xrt_tracking_origin_id values.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_tracking.h"


#ifdef __cplusplus
extern "C" {
#endif


/*!
 * Generate a new per-process unique tracking origin ID.
 *
 * @ingroup aux_util
 */
struct xrt_tracking_origin_id
u_tracking_origin_id_generate(void);


#ifdef __cplusplus
}
#endif
