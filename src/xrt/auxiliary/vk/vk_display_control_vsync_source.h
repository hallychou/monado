// Copyright 2019-2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  VK_EXT_display_control based vsync source implementation
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup aux_vk
 */

#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VK_KHR_display
#error "Vulkan headers missing VK_KHR_display"
#else

struct xrt_vsync_source;
struct vk_bundle;

struct xrt_vsync_source *
vk_display_control_vsync_source_create(struct vk_bundle *vk, //
                                       VkDisplayKHR display, //
                                       int64_t nominal_display_period_ns);

#endif

#ifdef __cplusplus
} // extern "C"
#endif
