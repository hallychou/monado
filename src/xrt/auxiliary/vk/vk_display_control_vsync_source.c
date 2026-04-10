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
#include "vk_display_control_vsync_source.h"

#include "xrt/xrt_vsync_source.h"

#include "util/u_logging.h"

#include "vk/vk_helpers.h"

#if defined(VK_EXT_display_control) && defined(VK_KHR_display)
#include "os/os_threading.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_trace_marker.h"
#include "util/u_thread_priority.h"

#include "util/u_vsync_event_source.h"

/*
 *
 * Logging.
 *
 */

DEBUG_GET_ONCE_LOG_OPTION(dc_log, "VSYNC_SOURCE_DC_LOG", U_LOGGING_WARN)

#define DC_LOG_T(dc, ...) U_LOG_IFL_T(dc->log_level, __VA_ARGS__)
#define DC_LOG_D(dc, ...) U_LOG_IFL_D(dc->log_level, __VA_ARGS__)
#define DC_LOG_I(dc, ...) U_LOG_IFL_I(dc->log_level, __VA_ARGS__)
#define DC_LOG_W(dc, ...) U_LOG_IFL_W(dc->log_level, __VA_ARGS__)
#define DC_LOG_E(dc, ...) U_LOG_IFL_E(dc->log_level, __VA_ARGS__)

/*
 *
 * Struct.
 *
 */

struct vk_display_control_vsync_source
{
	struct u_vsync_event_source base;

	struct vk_bundle *vk;

	VkDisplayKHR display;

	//! Thread waiting on vblank_event_fence (first pixel out).
	struct os_thread_helper event_thread;

	//! Nominal display period
	int64_t nominal_period_ns;

	int64_t last_timestamp_ns;

	uint64_t vsync_count;

	//! Set on first display event, until then registration failures are expected (display not active yet).
	bool first_display_event;

	enum u_logging_level log_level;
};

static inline struct vk_display_control_vsync_source *
vk_display_control_vsync_source(struct xrt_vsync_source *xvs)
{
	return (struct vk_display_control_vsync_source *)xvs;
}

static inline struct vk_bundle *
get_vk(struct vk_display_control_vsync_source *dvs)
{
	return dvs->vk;
}

static bool
vblank_event_func(struct vk_display_control_vsync_source *dvs, int64_t *out_timestamp_ns)
{
	struct vk_bundle *vk = get_vk(dvs);
	VkResult ret;

	VkDisplayEventInfoEXT event_info = {
	    .sType = VK_STRUCTURE_TYPE_DISPLAY_EVENT_INFO_EXT,
	    .displayEvent = VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT,
	};

	VkFence vblank_event_fence = VK_NULL_HANDLE;
	ret = vk->vkRegisterDisplayEventEXT(vk->device, dvs->display, &event_info, NULL, &vblank_event_fence);
	if (ret != VK_SUCCESS) {
		if (dvs->first_display_event) {
			DC_LOG_E(dvs, "vkRegisterDisplayEventEXT: %s", vk_str_result(ret));
		} else {
			DC_LOG_D(dvs, "vkRegisterDisplayEventEXT: %s (display not active yet?)", vk_str_result(ret));
		}
		return false;
	}

	// Name for debugging.
	VK_NAME_FENCE(vk, vblank_event_fence, "VSync DC VBlank");

	// Not scoped to not effect timing.
	COMP_TRACE_IDENT(vblank);

	// Do the wait.
	ret = vk->vkWaitForFences(vk->device, 1, &vblank_event_fence, true, time_s_to_ns(1));

	// As quickly as possible after the fence has fired.
	int64_t now_ns = os_monotonic_get_ns();

	bool valid = false;
	if (ret == VK_SUCCESS) {
		*out_timestamp_ns = now_ns;
		valid = true;
		dvs->first_display_event = true;
	} else if (ret == VK_TIMEOUT) {
		DC_LOG_W(dvs, "vkWaitForFences: VK_TIMEOUT");
	} else {
		DC_LOG_E(dvs, "vkWaitForFences: %s", vk_str_result(ret));
	}

	vk->vkDestroyFence(vk->device, vblank_event_fence, NULL);

	return valid;
}

static void *
run_vblank_event_thread(void *ptr)
{
	struct vk_display_control_vsync_source *dvs = (struct vk_display_control_vsync_source *)ptr;

	DC_LOG_D(dvs, "VSync event thread starting");

	const char *thread_name = "VBlank Events";

	U_TRACE_SET_THREAD_NAME(thread_name);
	os_thread_name_self(thread_name);

	u_try_to_set_realtime_priority_on_thread(U_LOGGING_INFO, thread_name);

	os_thread_helper_lock(&dvs->event_thread);

	while (os_thread_helper_is_running_locked(&dvs->event_thread)) {

		// Unlock while waiting.
		os_thread_helper_unlock(&dvs->event_thread);

		int64_t when_ns = 0;
		const bool valid = vblank_event_func(dvs, &when_ns);

		// Just keep swimming.
		os_thread_helper_lock(&dvs->event_thread);

		if (!valid) {
			os_thread_helper_unlock(&dvs->event_thread);
			/*!
			 * Drop the cached timestamp so the next valid event re-seeds
			 * the skip-first logic rather than reporting a period that
			 * spans the error window.
			 */
			dvs->last_timestamp_ns = 0;
			os_nanosleep(dvs->nominal_period_ns);
			os_thread_helper_lock(&dvs->event_thread);
			continue;
		}

		// skip the first one.
		if (dvs->last_timestamp_ns == 0) {
			dvs->last_timestamp_ns = when_ns;
			continue;
		}

		const int64_t period_ns = when_ns - dvs->last_timestamp_ns;
		dvs->last_timestamp_ns = when_ns;

		struct xrt_vsync_event new_vsync_event = {
		    .timestamp_ns = when_ns,
		    .period_ns = period_ns,
		    .vsync_count = ++dvs->vsync_count,
		};
		u_vsync_event_source_push(&dvs->base, &new_vsync_event);
	}

	os_thread_helper_unlock(&dvs->event_thread);

	return NULL;
}

static void
destroy(struct xrt_vsync_source *xvs)
{
	struct vk_display_control_vsync_source *dvs = vk_display_control_vsync_source(xvs);

	u_vsync_event_source_cancel(&dvs->base);

	os_thread_helper_destroy(&dvs->event_thread);

	u_vsync_event_source_fini(&dvs->base);

	free(dvs);
}

struct xrt_vsync_source *
vk_display_control_vsync_source_create(struct vk_bundle *vk, //
                                       VkDisplayKHR display, //
                                       int64_t nominal_display_period_ns)
{
	if (!vk->has_EXT_display_control) {
		U_LOG_E("VK_EXT_display_control is not supported.");
		return NULL;
	}

	if (display == VK_NULL_HANDLE) {
		U_LOG_E("vk_display_control_vsync_source_create: display parameter is null");
		return NULL;
	}

	if (nominal_display_period_ns <= 0) {
		U_LOG_E("vk_display_control_vsync_source_create: nominal_display_period_ns must be > 0");
		return NULL;
	}

	assert(display != VK_NULL_HANDLE);
	struct vk_display_control_vsync_source *dvs = U_TYPED_CALLOC(struct vk_display_control_vsync_source);

	u_vsync_event_source_init(&dvs->base, destroy);

	dvs->vk = vk;
	dvs->display = display;
	dvs->nominal_period_ns = nominal_display_period_ns;
	dvs->log_level = debug_get_log_option_dc_log();

	if (os_thread_helper_init(&dvs->event_thread) != 0 || //
	    os_thread_helper_start(&dvs->event_thread, run_vblank_event_thread, dvs) != 0) {
		U_LOG_E("Failed to start vsync looper thread");
		destroy(&dvs->base.base);
		return NULL;
	}

	return &dvs->base.base;
}

#elif defined(VK_KHR_display)
struct xrt_vsync_source *
vk_display_control_vsync_source_create(struct vk_bundle *vk, //
                                       VkDisplayKHR display, //
                                       int64_t nominal_display_period_ns)
{
	(void)vk;
	(void)display;
	(void)nominal_display_period_ns;
	U_LOG_E("Vulkan header version does not support VK_EXT_display_control");
	return NULL;
}
#endif
