// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  AChoreographer based vsync source implementation
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup aux_android
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_vsync_source;

struct xrt_vsync_source *
android_choreographer_vsync_source_create(void);

#ifdef __cplusplus
} // extern "C"
#endif
