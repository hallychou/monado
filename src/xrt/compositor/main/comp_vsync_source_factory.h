// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for vsync_source factory function.
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup comp_main
 */

#pragma once

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_vsync_source;
struct comp_target_swapchain;

xrt_result_t
comp_vsync_source_factory_create(struct comp_target_swapchain *cts, struct xrt_vsync_source **out_xvs);


#ifdef __cplusplus
} // extern "C"
#endif
