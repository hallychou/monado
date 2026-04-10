// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vsync_source factory function.
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup comp_main
 */
#include "main/comp_vsync_source_factory.h"
#include "main/comp_target_swapchain.h"
#include "main/comp_compositor.h"

#include "vk/vk_display_control_vsync_source.h"
#include "vk/vk_helpers.h"

#include "xrt/xrt_vsync_source.h"

static inline struct vk_bundle *
get_vk(struct comp_target_swapchain *cts)
{
	return &cts->base.c->base.vk;
}

static inline struct xrt_vsync_source *
comp_vk_display_control_vsync_source_create(struct comp_target_swapchain *cts)
{
#if defined(VK_KHR_surface)
	struct comp_target *ct = &cts->base;
	struct vk_bundle *vk = get_vk(cts);

	int64_t nominal_period_ns = 0;
	if (ct->get_current_refresh_rate != NULL) {
		float display_refresh_rate_hz = 0.f;
		xrt_result_t xres = comp_target_get_current_refresh_rate(ct, &display_refresh_rate_hz);
		if (xres == XRT_SUCCESS && display_refresh_rate_hz > 0.f) {
			nominal_period_ns = (int64_t)(1.0f / display_refresh_rate_hz * 1e+9f);
		}
	}

	if (nominal_period_ns == 0) {
		nominal_period_ns = ct->c->frame_interval_ns;
	}

	assert(nominal_period_ns != 0);
	return vk_display_control_vsync_source_create(vk, cts->display, nominal_period_ns);
#else
	(void)cts;
	return NULL;
#endif
}

xrt_result_t
comp_vsync_source_factory_create(struct comp_target_swapchain *cts, struct xrt_vsync_source **out_xvs)
{
#define FACTORY_CHECK_RET(fn)                                                                                          \
	do {                                                                                                           \
		struct xrt_vsync_source *xvs = fn;                                                                     \
		if (xvs != NULL) {                                                                                     \
			*out_xvs = xvs;                                                                                \
			return XRT_SUCCESS;                                                                            \
		}                                                                                                      \
	} while (false)

	FACTORY_CHECK_RET(comp_vk_display_control_vsync_source_create(cts));

#undef FACTORY_CHECK_RET
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}
