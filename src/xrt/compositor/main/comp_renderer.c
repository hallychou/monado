// Copyright 2019-2026, Collabora, Ltd.
// Copyright 2024-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Compositor rendering code.
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Moshi Turner <moshiturner@protonmail.com>
 * @ingroup comp_main
 */

#include "render/render_interface.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_frame.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_vsync_source.h"
#include "xrt/xrt_results.h"

#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_matrix_2x2.h"
#include "math/m_space.h"

#include "util/u_misc.h"
#include "util/u_trace_marker.h"
#include "util/u_distortion_mesh.h"
#include "util/u_sink.h"
#include "util/u_var.h"
#include "util/u_frame_times_widget.h"
#include "util/u_debug.h"

#include "util/comp_render.h"
#include "util/comp_high_level_render.h"

#include "main/comp_frame.h"
#include "main/comp_mirror_to_debug_gui.h"

#ifdef XRT_FEATURE_WINDOW_PEEK
#include "main/comp_window_peek.h"
#endif

#include "vk/vk_helpers.h"
#include "vk/vk_cmd.h"
#include "vk/vk_image_readback_to_xf_pool.h"
#include "vk/vk_submit_helpers.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>

DEBUG_GET_ONCE_LOG_OPTION(comp_frame_lag_level, "XRT_COMP_FRAME_LAG_LOG_AS_LEVEL", U_LOGGING_WARN)
#define LOG_FRAME_LAG(...) U_LOG_IFL(debug_get_log_option_comp_frame_lag_level(), u_log_get_global_level(), __VA_ARGS__)

/*
 *
 * Helper macros for adding semaphores to lists.
 *
 */

#define ADD_WAIT(LIST, SEMAPHORE, STAGE, IS_TIMELINE)                                                                  \
	do {                                                                                                           \
		VkSemaphore _semaphore = SEMAPHORE;                                                                    \
		if (_semaphore != VK_NULL_HANDLE) {                                                                    \
			if (IS_TIMELINE) {                                                                             \
				vk_semaphore_list_wait_add_timeline(&LIST, _semaphore, (uint64_t)frame_id, STAGE);     \
			} else {                                                                                       \
				vk_semaphore_list_wait_add_binary(&LIST, _semaphore, STAGE);                           \
			}                                                                                              \
		}                                                                                                      \
	} while (false)

#define ADD_SIGNAL(LIST, SEMAPHORE, IS_TIMELINE)                                                                       \
	do {                                                                                                           \
		VkSemaphore _semaphore = SEMAPHORE;                                                                    \
		if (_semaphore != VK_NULL_HANDLE) {                                                                    \
			if (IS_TIMELINE) {                                                                             \
				vk_semaphore_list_signal_add_timeline(&LIST, _semaphore, (uint64_t)frame_id);          \
			} else {                                                                                       \
				vk_semaphore_list_signal_add_binary(&LIST, _semaphore);                                \
			}                                                                                              \
		}                                                                                                      \
	} while (false)


/*
 *
 * Private struct(s).
 *
 */

/*!
 * What is the source of the FoV values used for the final image that the
 * compositor produces and is sent to the hardware (or software).
 */
enum comp_target_fov_source
{
	/*!
	 * The FoV values used for the final target is taken from the
	 * distortion information on the @ref xrt_hmd_parts struct.
	 */
	COMP_TARGET_FOV_SOURCE_DISTORTION,

	/*!
	 * The FoV values used for the final target is taken from the
	 * those returned from the device's get_views.
	 */
	COMP_TARGET_FOV_SOURCE_DEVICE_VIEWS,
};

/*!
 * Holds associated vulkan objects and state to render with a distortion.
 *
 * @ingroup comp_main
 */
struct comp_renderer
{
	//! @name Durable members
	//! @brief These don't require the images to be created and don't depend on it.
	//! @{

	//! The compositor we were created by
	struct comp_compositor *c;
	struct comp_settings *settings;

	struct comp_mirror_to_debug_gui mirror_to_debug_gui;

	//! @}

	//! @name Image-dependent members
	//! @{

	//! Index of the current buffer/image
	int32_t acquired_buffer;

	//! Which buffer was last submitted and has a fence pending.
	int32_t fenced_buffer;

	/*!
	 * The render pass used to render to the target, it depends on the
	 * target's format so will be recreated each time the target changes.
	 */
	struct render_gfx_render_pass target_render_pass;

	/*!
	 * Array of "rendering" target resources equal in size to the number of
	 * comp_target images. Each target resources holds all of the resources
	 * needed to render to that target and its views.
	 */
	struct render_gfx_target_resources *rtr_array;

	/*!
	 * Array of fences equal in size to the number of comp_target images.
	 */
	VkFence *fences;

	/*!
	 * The number of renderings/fences we've created: set from comp_target when we use that data.
	 */
	uint32_t buffer_count;

	/*!
	 * Only relevant when using present modes from VK_KHR_shared_presentable_image,
	 * tracks whether we have waited on the shared present semaphore at least once,
	 * subsequents present waits are redundant and can be skipped.
	 */
	bool shared_present_semaphore_wait_once;

	VkResult (*dispatch_graphics)(struct comp_renderer *r,
	                              struct render_gfx *render,
	                              struct chl_frame_state *frame_state,
	                              enum comp_target_fov_source fov_source);

	//! @}
};


/*
 *
 * Functions.
 *
 */

static XRT_CHECK_RESULT VkResult
dispatch_gfx(struct comp_renderer *r,
             struct render_gfx *render,
             struct chl_frame_state *frame_state,
             enum comp_target_fov_source fov_source);

static XRT_CHECK_RESULT VkResult
dispatch_gfx_strip_render(struct comp_renderer *r,
                          struct render_gfx *render,
                          struct chl_frame_state *frame_state,
                          enum comp_target_fov_source fov_source);

static inline XRT_CHECK_RESULT VkResult
dispatch_graphics(struct comp_renderer *r,
                  struct render_gfx *render,
                  struct chl_frame_state *frame_state,
                  enum comp_target_fov_source fov_source)
{
	return (*r->dispatch_graphics)(r, render, frame_state, fov_source);
}

static void
renderer_wait_queue_idle(struct comp_renderer *r)
{
	COMP_TRACE_MARKER();
	struct vk_bundle *vk = &r->c->base.vk;

	vk_queue_lock(vk->main_queue);
	vk->vkQueueWaitIdle(vk->main_queue->queue);
	vk_queue_unlock(vk->main_queue);
}

static inline void
precise_sleep_until(int64_t target_ns, int64_t max_spin_ns)
{
	const int64_t now_ns = os_monotonic_get_ns();
	if (target_ns <= now_ns) {
		return;
	}
	const int64_t remaining_ns = target_ns - now_ns;
	if (remaining_ns > max_spin_ns) {
		os_nanosleep(remaining_ns - max_spin_ns);
	}
	while (os_monotonic_get_ns() < target_ns) {
		// tight spin...
	}
}

static void
calc_viewport_data(struct comp_renderer *r,
                   struct render_viewport_data out_viewport_data[XRT_MAX_VIEWS],
                   size_t view_count)
{
	struct comp_compositor *c = r->c;

	bool pre_rotate = false;
	if (r->c->target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
	    r->c->target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
		COMP_SPEW(c, "Swapping width and height, since we are pre rotating");
		pre_rotate = true;
	}

	int w_i32 = pre_rotate ? r->c->xdev->hmd->screens[0].h_pixels : r->c->xdev->hmd->screens[0].w_pixels;
	int h_i32 = pre_rotate ? r->c->xdev->hmd->screens[0].w_pixels : r->c->xdev->hmd->screens[0].h_pixels;

	float scale_x = (float)r->c->target->width / (float)w_i32;
	float scale_y = (float)r->c->target->height / (float)h_i32;

	for (uint32_t i = 0; i < view_count; ++i) {
		struct xrt_view *v = &r->c->xdev->hmd->views[i];
		if (pre_rotate) {
			out_viewport_data[i] = (struct render_viewport_data){
			    .x = (uint32_t)(v->viewport.y_pixels * scale_x),
			    .y = (uint32_t)(v->viewport.x_pixels * scale_y),
			    .w = (uint32_t)(v->viewport.h_pixels * scale_x),
			    .h = (uint32_t)(v->viewport.w_pixels * scale_y),
			};
		} else {
			out_viewport_data[i] = (struct render_viewport_data){
			    .x = (uint32_t)(v->viewport.x_pixels * scale_x),
			    .y = (uint32_t)(v->viewport.y_pixels * scale_y),
			    .w = (uint32_t)(v->viewport.w_pixels * scale_x),
			    .h = (uint32_t)(v->viewport.h_pixels * scale_y),
			};
		}
	}
}

static inline void
calc_strip_render_area(struct comp_renderer *r,
                       uint32_t strip,
                       uint32_t strip_count,
                       const struct xrt_device_compositor_info *scanout_info,
                       VkRect2D *out_render_area)
{
	assert(strip_count > 0 && strip < strip_count);

	const uint32_t target_w = r->c->target->width;
	const uint32_t target_h = r->c->target->height;

	switch (scanout_info->scanout_direction) {
	case XRT_SCANOUT_DIRECTION_LEFT_TO_RIGHT: {
		const uint32_t sx = (strip * target_w) / strip_count;
		const uint32_t ex = ((strip + 1) * target_w) / strip_count;
		*out_render_area = (VkRect2D){
		    .offset = {.x = (int32_t)sx, .y = 0},
		    .extent = {.width = ex - sx, .height = target_h},
		};
		break;
	}
	case XRT_SCANOUT_DIRECTION_TOP_TO_BOTTOM: {
		const uint32_t sy = (strip * target_h) / strip_count;
		const uint32_t ey = ((strip + 1) * target_h) / strip_count;
		*out_render_area = (VkRect2D){
		    .offset = {.x = 0, .y = (int32_t)sy},
		    .extent = {.width = target_w, .height = ey - sy},
		};
		break;
	}
	default: {
		*out_render_area = (VkRect2D){
		    .offset = {.x = 0, .y = 0},
		    .extent = {.width = target_w, .height = target_h},
		};
		break;
	}
	}
}

static inline void
calc_strip_view_scissors(uint32_t view_count,
                         const VkRect2D *strip_render_area,
                         const struct render_viewport_data view_viewports[XRT_MAX_VIEWS],
                         render_scissor_data_t out_scissor_data[XRT_MAX_VIEWS])
{
	const uint32_t b_x0 = (uint32_t)strip_render_area->offset.x;
	const uint32_t b_y0 = (uint32_t)strip_render_area->offset.y;
	const uint32_t b_x1 = b_x0 + strip_render_area->extent.width;
	const uint32_t b_y1 = b_y0 + strip_render_area->extent.height;

	for (uint32_t i = 0; i < view_count; ++i) {
		const struct render_viewport_data *v = &view_viewports[i];
		const uint32_t v_x1 = v->x + v->w;
		const uint32_t v_y1 = v->y + v->h;

		const uint32_t x0 = MAX(v->x, b_x0);
		const uint32_t y0 = MAX(v->y, b_y0);
		const uint32_t x1 = MIN(v_x1, b_x1);
		const uint32_t y1 = MIN(v_y1, b_y1);

		out_scissor_data[i] = (render_scissor_data_t){
		    .x = x0,
		    .y = y0,
		    .w = x1 > x0 ? x1 - x0 : 0,
		    .h = y1 > y0 ? y1 - y0 : 0,
		};
	}
}

static void
calc_vertex_rot_data(struct comp_renderer *r, struct xrt_matrix_2x2 out_vertex_rots[XRT_MAX_VIEWS], size_t view_count)
{
	bool pre_rotate = false;
	if (r->c->target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
	    r->c->target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
		COMP_SPEW(r->c, "Swapping width and height, since we are pre rotating");
		pre_rotate = true;
	}

	const struct xrt_matrix_2x2 rotation_90_cw = {{
	    .vecs =
	        {
	            {0, 1},
	            {-1, 0},
	        },
	}};

	for (uint32_t i = 0; i < view_count; i++) {
		// Get the view.
		struct xrt_view *v = &r->c->xdev->hmd->views[i];

		// Copy data.
		struct xrt_matrix_2x2 rot = v->rot;

		// Should we rotate.
		if (pre_rotate) {
			m_mat2x2_multiply(&rot, &rotation_90_cw, &rot);
		}

		out_vertex_rots[i] = rot;
	}
}

static void
calc_pose_data_at_time_window(struct comp_renderer *r,
                              int64_t begin_timestamp_ns,
                              int64_t end_timestamp_ns,
                              enum comp_target_fov_source fov_source,
                              struct xrt_fov out_fovs[XRT_MAX_VIEWS],
                              struct xrt_pose out_world_scanout_begin[XRT_MAX_VIEWS],
                              struct xrt_pose out_world_scanout_end[XRT_MAX_VIEWS],
                              struct xrt_pose out_eye[XRT_MAX_VIEWS],
                              uint32_t view_count)
{
	COMP_TRACE_MARKER();

	assert(begin_timestamp_ns != 0 && end_timestamp_ns != 0);

	const struct xrt_vec3 default_eye_relation = {
	    0.063000f, /*! @todo get actual ipd_meters */
	    0.0f,
	    0.0f,
	};

	struct xrt_space_relation head_relation[2] = XRT_SPACE_RELATION_ZERO;
	struct xrt_fov xdev_fovs[XRT_MAX_VIEWS] = XRT_STRUCT_INIT;
	struct xrt_pose xdev_poses[2][XRT_MAX_VIEWS] = XRT_STRUCT_INIT;

	// Determine view type based on view count
	enum xrt_view_type view_type = (view_count == 1) ? XRT_VIEW_TYPE_MONO : XRT_VIEW_TYPE_STEREO;

	// Pose at beginning of scanout
	xrt_result_t xret = xrt_device_get_view_poses( //
	    r->c->xdev,                                //
	    &default_eye_relation,                     //
	    begin_timestamp_ns,                        // at_timestamp_ns
	    view_type,                                 //
	    view_count,                                //
	    &head_relation[0],                         // out_head_relation
	    xdev_fovs,                                 // out_fovs
	    xdev_poses[0]);                            //
	if (xret != XRT_SUCCESS) {
		struct u_pp_sink_stack_only sink;
		u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);
		u_pp_xrt_result(dg, xret);
		U_LOG_E("xrt_device_get_view_poses failed: %s", sink.buffer);
		return;
	}

	// Pose at end of scanout
	if (end_timestamp_ns != begin_timestamp_ns) {
		xret = xrt_device_get_view_poses( //
		    r->c->xdev,                   //
		    &default_eye_relation,        //
		    end_timestamp_ns,             // at_timestamp_ns
		    view_type,                    //
		    view_count,                   //
		    &head_relation[1],            // out_head_relation
		    xdev_fovs,                    // out_fovs
		    xdev_poses[1]);               // out_poses
		if (xret != XRT_SUCCESS) {
			struct u_pp_sink_stack_only sink;
			u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);
			u_pp_xrt_result(dg, xret);
			U_LOG_E("xrt_device_get_view_poses failed: %s", sink.buffer);
			return;
		}
	} else {
		for (size_t i = 0; i < XRT_MAX_VIEWS; ++i) {
			xdev_poses[1][i] = xdev_poses[0][i];
		}
		head_relation[1] = head_relation[0];
	}

	struct xrt_fov dist_fov[XRT_MAX_VIEWS] = XRT_STRUCT_INIT;
	for (uint32_t i = 0; i < view_count; i++) {
		dist_fov[i] = r->c->xdev->hmd->distortion.fov[i];
	}

	bool use_xdev = false; // Probably what we want.

	switch (fov_source) {
	case COMP_TARGET_FOV_SOURCE_DISTORTION: use_xdev = false; break;
	case COMP_TARGET_FOV_SOURCE_DEVICE_VIEWS: use_xdev = true; break;
	}

	for (uint32_t i = 0; i < view_count; i++) {
		const struct xrt_fov fov = use_xdev ? xdev_fovs[i] : dist_fov[i];
		const struct xrt_pose eye_pose_scanout_start = xdev_poses[0][i];
		const struct xrt_pose eye_pose_scanout_end = xdev_poses[1][i];

		struct xrt_space_relation result_scanout_start = {0};
		struct xrt_space_relation result_scanout_end = {0};
		struct xrt_relation_chain xrc = {0};

		m_relation_chain_push_pose_if_not_identity(&xrc, &eye_pose_scanout_start);
		m_relation_chain_push_relation(&xrc, &head_relation[0]);
		m_relation_chain_resolve(&xrc, &result_scanout_start);

		xrc = (struct xrt_relation_chain){0};

		m_relation_chain_push_pose_if_not_identity(&xrc, &eye_pose_scanout_end);
		m_relation_chain_push_relation(&xrc, &head_relation[1]);
		m_relation_chain_resolve(&xrc, &result_scanout_end);

		// Results to callers.
		out_fovs[i] = fov;
		out_world_scanout_begin[i] = result_scanout_start.pose;
		out_world_scanout_end[i] = result_scanout_end.pose;
		out_eye[i] = eye_pose_scanout_start;

		// For remote rendering targets.
		r->c->base.frame_params.fovs[i] = fov;
		r->c->base.frame_params.poses[i] = result_scanout_start.pose;
	}
}

static inline void
calc_pose_data_at_time(struct comp_renderer *r,
                       int64_t timestamp_ns,
                       enum comp_target_fov_source fov_source,
                       struct xrt_fov out_fovs[XRT_MAX_VIEWS],
                       struct xrt_pose out_world_scanout_begin[XRT_MAX_VIEWS],
                       struct xrt_pose out_eye[XRT_MAX_VIEWS],
                       uint32_t view_count)
{
	struct xrt_pose unused_world_scanout_end[XRT_MAX_VIEWS];
	calc_pose_data_at_time_window(r,                        //
	                              timestamp_ns,             //
	                              timestamp_ns,             //
	                              fov_source,               //
	                              out_fovs,                 //
	                              out_world_scanout_begin,  //
	                              unused_world_scanout_end, //
	                              out_eye,                  //
	                              view_count);              //
}

static inline void
calc_pose_data(struct comp_renderer *r,
               enum comp_target_fov_source fov_source,
               struct xrt_fov out_fovs[XRT_MAX_VIEWS],
               struct xrt_pose out_world_scanout_begin[XRT_MAX_VIEWS],
               struct xrt_pose out_world_scanout_end[XRT_MAX_VIEWS],
               struct xrt_pose out_eye[XRT_MAX_VIEWS],
               uint32_t view_count)
{
	int64_t scanout_time_ns = 0;
	if (r->c->xdev->supported.compositor_info) {
		struct xrt_device_compositor_mode compositor_mode = {
		    .frame_interval_ns = r->c->frame_interval_ns,
		};
		struct xrt_device_compositor_info device_compositor_info = {0};
		xrt_result_t xret = xrt_device_get_compositor_info( //
		    r->c->xdev,                                     //
		    &compositor_mode,                               //
		    &device_compositor_info);                       //

		if (xret != XRT_SUCCESS) {
			COMP_WARN(r->c, "xrt_device_get_compositor_info failed, assuming 0 scanout time");
		} else if (device_compositor_info.panel_refresh_type == XRT_PANEL_REFRESH_TYPE_ROLLING &&
		           device_compositor_info.scanout_direction == XRT_SCANOUT_DIRECTION_TOP_TO_BOTTOM) {
			scanout_time_ns = device_compositor_info.scanout_time_ns;
		} else {
			COMP_SPEW(r->c,
			          "Unable to apply scanout compensation as only DIRECTION_TOP_TO_BOTTOM is "
			          "supported");
		}
	}

	const int64_t begin_timestamp_ns = r->c->frame.rendering.predicted_display_time_ns;
	const int64_t end_timestamp_ns = begin_timestamp_ns + scanout_time_ns;

	calc_pose_data_at_time_window(r,                       //
	                              begin_timestamp_ns,      //
	                              end_timestamp_ns,        //
	                              fov_source,              //
	                              out_fovs,                //
	                              out_world_scanout_begin, //
	                              out_world_scanout_end,   //
	                              out_eye,                 //
	                              view_count);             //
}

//! @pre comp_target_has_images(r->c->target)
static void
renderer_build_rendering_target_resources(struct comp_renderer *r,
                                          struct render_gfx_target_resources *rtr,
                                          uint32_t index)
{
	COMP_TRACE_MARKER();

	struct comp_compositor *c = r->c;

	VkImageView image_view = r->c->target->images[index].view;
	VkExtent2D extent = {r->c->target->width, r->c->target->height};

	render_gfx_target_resources_init( //
	    rtr,                          //
	    &c->nr,                       //
	    &r->target_render_pass,       //
	    image_view,                   //
	    extent);                      //
}

/*!
 * @pre comp_target_has_images(r->c->target)
 * Update r->buffer_count before calling.
 */
static void
renderer_create_renderings_and_fences(struct comp_renderer *r)
{
	assert(r->fences == NULL);
	if (r->buffer_count == 0) {
		COMP_ERROR(r->c, "Requested 0 command buffers.");
		return;
	}

	COMP_DEBUG(r->c, "Allocating %d Command Buffers.", r->buffer_count);

	struct vk_bundle *vk = &r->c->base.vk;

	bool use_compute = r->settings->use_compute;
	if (!use_compute) {
		r->rtr_array = U_TYPED_ARRAY_CALLOC(struct render_gfx_target_resources, r->buffer_count);

		render_gfx_render_pass_init(       //
		    &r->target_render_pass,        // rgrp
		    &r->c->nr,                     // struct render_resources
		    r->c->target->format,          //
		    r->c->target->present_load_op, // load_op
		    r->c->target->final_layout);   // final_layout

		for (uint32_t i = 0; i < r->buffer_count; ++i) {
			renderer_build_rendering_target_resources(r, &r->rtr_array[i], i);
		}
	}

	r->fences = U_TYPED_ARRAY_CALLOC(VkFence, r->buffer_count);

	for (uint32_t i = 0; i < r->buffer_count; i++) {
		VkFenceCreateInfo fence_info = {
		    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
		};

		VkResult ret = vk->vkCreateFence( //
		    vk->device,                   //
		    &fence_info,                  //
		    NULL,                         //
		    &r->fences[i]);               //
		if (ret != VK_SUCCESS) {
			COMP_ERROR(r->c, "vkCreateFence: %s", vk_result_string(ret));
		}

		char buf[] = "Comp Renderer X_XXXX_XXXX";
		snprintf(buf, ARRAY_SIZE(buf), "Comp Renderer %u", i);
		VK_NAME_FENCE(vk, r->fences[i], buf);
	}
}

static void
renderer_close_renderings_and_fences(struct comp_renderer *r)
{
	struct vk_bundle *vk = &r->c->base.vk;
	// Renderings
	if (r->buffer_count > 0 && r->rtr_array != NULL) {
		for (uint32_t i = 0; i < r->buffer_count; i++) {
			render_gfx_target_resources_fini(&r->rtr_array[i]);
		}

		// Close the render pass used for rendering to the target.
		render_gfx_render_pass_fini(&r->target_render_pass);

		free(r->rtr_array);
		r->rtr_array = NULL;
	}

	// Fences
	if (r->buffer_count > 0 && r->fences != NULL) {
		for (uint32_t i = 0; i < r->buffer_count; i++) {
			vk->vkDestroyFence(vk->device, r->fences[i], NULL);
			r->fences[i] = VK_NULL_HANDLE;
		}
		free(r->fences);
		r->fences = NULL;
	}

	r->buffer_count = 0;
	r->acquired_buffer = -1;
	r->fenced_buffer = -1;
}

/*!
 * @brief Ensure that target images and renderings are created, if possible.
 *
 * @param r Self pointer
 * @param force_recreate If true, will tear down and re-create images and renderings, e.g. for a resize
 *
 * @returns true if images and renderings are ready and created.
 *
 * @private @memberof comp_renderer
 * @ingroup comp_main
 */
static bool
renderer_ensure_images_and_renderings(struct comp_renderer *r, bool force_recreate)
{
	struct comp_compositor *c = r->c;
	struct comp_target *target = c->target;

	if (!comp_target_check_ready(target)) {
		// Not ready, so can't render anything.
		return false;
	}

	// We will create images if we don't have any images or if we were told to recreate them.
	bool create = force_recreate || !comp_target_has_images(target) || (r->buffer_count == 0);
	if (!create) {
		return true;
	}

	COMP_DEBUG(c, "Creating images and renderings (force_recreate: %s).", force_recreate ? "true" : "false");

	/*
	 * This makes sure that any pending command buffer has completed
	 * and all resources referred by it can now be manipulated. This
	 * make sure that validation doesn't complain. This is done
	 * during resize so isn't time critical.
	 */
	renderer_wait_queue_idle(r);

	// Make we sure we destroy all dependent things before creating new images.
	renderer_close_renderings_and_fences(r);

	VkImageUsageFlags image_usage = 0;
	if (r->settings->use_compute) {
		image_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	} else {
		image_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}

	if (c->peek) {
		image_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	struct comp_target_create_images_info info = {
	    .extent =
	        {
	            .width = r->c->settings.preferred.width,
	            .height = r->c->settings.preferred.height,
	        },
	    .image_usage = image_usage,
	    .color_space = r->settings->color_space,
	    .present_mode = r->settings->present_mode,
	};

	static_assert(ARRAY_SIZE(info.formats) == ARRAY_SIZE(r->c->settings.formats), "Miss-match format array sizes");
	for (uint32_t i = 0; i < r->c->settings.format_count; i++) {
		info.formats[info.format_count++] = r->c->settings.formats[i];
	}

	comp_target_create_images(target, &info, r->c->base.vk.main_queue);

	bool pre_rotate = false;
	if (target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
	    target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
		pre_rotate = true;
	}

	// @todo: is it safe to fail here?
	if (!render_distortion_images_ensure(&r->c->nr, &r->c->base.vk, r->c->xdev, pre_rotate))
		return false;

	r->buffer_count = r->c->target->image_count;

	renderer_create_renderings_and_fences(r);

	assert(r->buffer_count != 0);

	r->dispatch_graphics = dispatch_gfx;

	if (!c->settings.disable_gfx_strip_render && //
	    comp_target_is_shared_presentable_image(target)) {

		struct xrt_device_compositor_mode compositor_mode = {
		    .frame_interval_ns = r->c->frame_interval_ns,
		};
		struct xrt_device_compositor_info device_compositor_info = {0};
		const xrt_result_t xres = xrt_device_get_compositor_info(r->c->xdev,                   //
		                                                         &compositor_mode,             //
		                                                         &device_compositor_info);     //
		if (xres == XRT_SUCCESS &&                                                             //
		    device_compositor_info.scanout_direction == XRT_SCANOUT_DIRECTION_TOP_TO_BOTTOM && //
		    device_compositor_info.strip_render.strip_count > 0) {
			r->dispatch_graphics = dispatch_gfx_strip_render;
			COMP_DEBUG(r->c, "using single-buffered strip render path");
		}
	}

	return true;
}

//! Create renderer and initialize non-image-dependent members
static void
renderer_init(struct comp_renderer *r, struct comp_compositor *c, VkExtent2D scratch_extent)
{
	COMP_TRACE_MARKER();

	r->c = c;
	r->settings = &c->settings;

	r->acquired_buffer = -1;
	r->fenced_buffer = -1;
	r->rtr_array = NULL;
	//! default to standard dispatch_graphics function
	r->dispatch_graphics = dispatch_gfx;

	// Setup the scratch images.
	bool bret = chl_scratch_ensure( //
	    &c->scratch,                // scratch
	    &c->nr,                     // struct render_resources
	    c->nr.view_count,           // view_count
	    scratch_extent,             // extent
	    VK_FORMAT_R8G8B8A8_SRGB);   // format
	if (!bret) {
		COMP_ERROR(c, "chl_scratch_ensure: false");
		assert(bret && "Whelp, can't return a error. But should never really fail.");
	}

	// Try to early-allocate these, in case we can.
	renderer_ensure_images_and_renderings(r, false);

	struct vk_bundle *vk = &r->c->base.vk;

	VkResult ret = comp_mirror_init( //
	    &r->mirror_to_debug_gui,     //
	    vk,                          //
	    &c->shaders,                 //
	    scratch_extent);             //
	if (ret != VK_SUCCESS) {
		COMP_ERROR(c, "comp_mirror_init: %s", vk_result_string(ret));
		assert(false && "Whelp, can't return a error. But should never really fail.");
	}
}

static void
renderer_wait_for_last_fence(struct comp_renderer *r)
{
	COMP_TRACE_MARKER();

	if (r->fenced_buffer < 0) {
		return;
	}

	struct vk_bundle *vk = &r->c->base.vk;
	VkResult ret;

	ret = vk->vkWaitForFences(vk->device, 1, &r->fences[r->fenced_buffer], VK_TRUE, UINT64_MAX);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(r->c, "vkWaitForFences: %s", vk_result_string(ret));
	}

	r->fenced_buffer = -1;
}

static inline bool
requires_present_acquire_wait(struct comp_renderer *r)
{
	/*!
	 * With shared presentable images there is only one image
	 * shared between the presentation layer and the compositor,
	 *
	 * We only need to wait on the first "acquire image",
	 * subsueqent frames waiting is redundant.
	 */
	if (comp_target_is_shared_presentable_image(r->c->target)) {
		if (r->shared_present_semaphore_wait_once) {
			return false;
		}
		r->shared_present_semaphore_wait_once = true;
	}

	return true;
}

static XRT_CHECK_RESULT VkResult
renderer_submit_queue(struct comp_renderer *r, VkCommandBuffer cmd, VkPipelineStageFlags pipeline_stage_flag)
{
	COMP_TRACE_MARKER();

	struct vk_bundle *vk = &r->c->base.vk;
	int64_t frame_id = r->c->frame.rendering.id;
	VkResult ret;

	assert(frame_id >= 0);


	/*
	 * Wait for previous frame's work to complete.
	 */

	// Wait for the last fence, if any.
	renderer_wait_for_last_fence(r);
	assert(r->fenced_buffer < 0);

	assert(r->acquired_buffer >= 0);
	ret = vk->vkResetFences(vk->device, 1, &r->fences[r->acquired_buffer]);
	VK_CHK_AND_RET(ret, "vkResetFences");


	/*
	 * Semaphore and queue submit info setup.
	 */

	struct comp_target *ct = r->c->target;
	struct vk_semaphore_list_wait wait_sems = XRT_STRUCT_INIT;
	struct vk_semaphore_list_signal signal_sems = XRT_STRUCT_INIT;
	struct vk_submit_info_builder builder = XRT_STRUCT_INIT;

	if (requires_present_acquire_wait(r)) {
		ADD_WAIT(wait_sems, ct->semaphores.present_complete, pipeline_stage_flag, false);
	}

	// Add signal semaphore (render_complete to target).
	ADD_SIGNAL(signal_sems, ct->semaphores.render_complete, ct->semaphores.render_complete_is_timeline);

	// Build the submit info struct, handles all of the semaphores for us.
	vk_submit_info_builder_prepare( //
	    &builder,                   //
	    &wait_sems,                 //
	    &cmd,                       //
	    1,                          //
	    &signal_sems,               //
	    NULL);                      //

	// Everything prepared, now we are submitting.
	comp_target_mark_submit_begin(ct, frame_id, os_monotonic_get_ns());

	/*
	 * The renderer command buffer pool is only accessed from one thread,
	 * this satisfies the `_locked` requirement of the function. This lets
	 * us avoid taking a lot of locks. The queue lock will be taken by
	 * @ref vk_cmd_submit_locked tho.
	 */
	ret = vk_cmd_submit_locked(vk, vk->main_queue, 1, &builder.submit_info, r->fences[r->acquired_buffer]);

	// We have now completed the submit, even if we failed.
	comp_target_mark_submit_end(ct, frame_id, os_monotonic_get_ns());

	// Check after marking as submit complete.
	VK_CHK_AND_RET(ret, "vk_cmd_submit_locked");

	// This buffer now have a pending fence.
	r->fenced_buffer = r->acquired_buffer;

	return ret;
}

static void
renderer_acquire_swapchain_image(struct comp_renderer *r)
{
	COMP_TRACE_MARKER();

	uint32_t buffer_index = 0;
	VkResult ret;

	assert(r->acquired_buffer < 0);

	if (!renderer_ensure_images_and_renderings(r, false)) {
		// Not ready yet.
		return;
	}
	ret = comp_target_acquire(r->c->target, &buffer_index);

	if ((ret == VK_ERROR_OUT_OF_DATE_KHR) || (ret == VK_SUBOPTIMAL_KHR)) {
		COMP_DEBUG(r->c, "Received %s.", vk_result_string(ret));

		if (!renderer_ensure_images_and_renderings(r, true)) {
			// Failed on force recreate.
			COMP_ERROR(r->c,
			           "renderer_acquire_swapchain_image: comp_target_acquire was out of date, force "
			           "re-create image and renderings failed. Probably the target disappeared.");
			return;
		}

		/* Acquire image again to silence validation error */
		ret = comp_target_acquire(r->c->target, &buffer_index);
		if (ret != VK_SUCCESS) {
			COMP_ERROR(r->c, "comp_target_acquire: %s", vk_result_string(ret));
		}
	} else if (ret != VK_SUCCESS) {
		COMP_ERROR(r->c, "comp_target_acquire: %s", vk_result_string(ret));
	}

	r->acquired_buffer = buffer_index;
}

static void
renderer_resize(struct comp_renderer *r)
{
	if (!comp_target_check_ready(r->c->target)) {
		// Can't create images right now.
		// Just close any existing renderings.
		renderer_close_renderings_and_fences(r);
		return;
	}
	// Force recreate.
	renderer_ensure_images_and_renderings(r, true);
}

static void
renderer_present_swapchain_image(struct comp_renderer *r, uint64_t desired_present_time_ns, uint64_t present_slop_ns)
{
	COMP_TRACE_MARKER();

	VkResult ret;

	assert(!comp_frame_is_invalid_locked(&r->c->frame.rendering));
	uint64_t render_complete_signal_value = (uint64_t)r->c->frame.rendering.id;

	ret = comp_target_present(        //
	    r->c->target,                 //
	    r->c->base.vk.main_queue,     //
	    r->acquired_buffer,           //
	    render_complete_signal_value, //
	    desired_present_time_ns,      //
	    present_slop_ns);             //
	r->acquired_buffer = -1;

	if (ret == VK_ERROR_OUT_OF_DATE_KHR || ret == VK_SUBOPTIMAL_KHR) {
		renderer_resize(r);
		return;
	}
	if (ret != VK_SUCCESS) {
		COMP_ERROR(r->c, "vk_swapchain_present: %s", vk_result_string(ret));
	}
}

static void
renderer_wait_for_present(struct comp_renderer *r, uint64_t desired_present_time_ns)
{
	struct comp_compositor *c = r->c;

	if (!comp_target_check_ready(c->target)) {
		return;
	}

	// For estimating frame misses.
	uint64_t before_ns = os_monotonic_get_ns();

	if (c->target->wait_for_present_supported) {
		// reasonable timeout
		time_duration_ns timeout_ns = c->frame_interval_ns * 2.5f;

		// @note we don't actually care about the return value, just swallow errors, anything *critical* that
		// may be returned will be handled quite soon by later calls
		VkResult result = comp_target_wait_for_present(c->target, timeout_ns);
		(void)result;

		assert(result != VK_ERROR_EXTENSION_NOT_PRESENT);
	} else {
		/*
		 * For direct mode this makes us wait until the last frame has been
		 * actually shown to the user, this avoids us missing that we have
		 * missed a frame and miss-predicting the next frame.
		 *
		 * Not all drivers follow this behaviour, so KHR_present_wait
		 * should be preferred in all circumstances.
		 *
		 * Only do this if we are ready.
		 */

		// Do the acquire
		renderer_acquire_swapchain_image(r);
	}

	// How long did it take?
	uint64_t after_ns = os_monotonic_get_ns();

	/*
	 * Make sure we at least waited 1ms before warning. Then check
	 * if we are more then 1ms behind when we wanted to present.
	 */
	if (before_ns + U_TIME_1MS_IN_NS < after_ns && //
	    desired_present_time_ns + U_TIME_1MS_IN_NS < after_ns) {
		uint64_t diff_ns = after_ns - desired_present_time_ns;
		double diff_ms_f = time_ns_to_ms_f(diff_ns);
		LOG_FRAME_LAG("Compositor probably missed frame by %.2fms", diff_ms_f);
	}
}

static void
renderer_fini(struct comp_renderer *r)
{
	struct vk_bundle *vk = &r->c->base.vk;

	// Command buffers
	renderer_close_renderings_and_fences(r);

	// Do before layer render just in case it holds any references.
	comp_mirror_fini(&r->mirror_to_debug_gui, vk);

	// Do this after the layer renderer.
	chl_scratch_free_resources(&r->c->scratch, &r->c->nr);
}


/*
 *
 * Graphics
 *
 */

/*!
 * @pre render_gfx_init(render, &c->nr)
 */
static XRT_CHECK_RESULT VkResult
dispatch_gfx(struct comp_renderer *r,
             struct render_gfx *render,
             struct chl_frame_state *frame_state,
             enum comp_target_fov_source fov_source)
{
	COMP_TRACE_MARKER();

	struct comp_compositor *c = r->c;
	struct vk_bundle *vk = &c->base.vk;
	VkResult ret;

	// Basics
	const struct comp_layer *layers = c->base.layer_accum.layers;
	uint32_t layer_count = c->base.layer_accum.layer_count;

	// Resources for the distortion render target.
	struct render_gfx_target_resources *rtr = &r->rtr_array[r->acquired_buffer];

	// Viewport information.
	struct render_viewport_data viewport_datas[XRT_MAX_VIEWS];
	calc_viewport_data(r, viewport_datas, render->r->view_count);

	// Vertex rotation information.
	struct xrt_matrix_2x2 vertex_rots[XRT_MAX_VIEWS];
	calc_vertex_rot_data(r, vertex_rots, render->r->view_count);

	// Device view information.
	struct xrt_fov fovs[XRT_MAX_VIEWS];
	struct xrt_pose world_poses_scanout_begin[XRT_MAX_VIEWS];
	struct xrt_pose world_poses_scanout_end[XRT_MAX_VIEWS];
	struct xrt_pose eye_poses[XRT_MAX_VIEWS];
	calc_pose_data(                //
	    r,                         //
	    fov_source,                //
	    fovs,                      //
	    world_poses_scanout_begin, //
	    world_poses_scanout_end,   //
	    eye_poses,                 //
	    render->r->view_count);    //

	// Does everything.
	chl_frame_state_gfx_default_pipeline( //
	    frame_state,                      //
	    render,                           //
	    layers,                           //
	    layer_count,                      //
	    world_poses_scanout_begin,        //
	    eye_poses,                        //
	    fovs,                             //
	    rtr,                              //
	    viewport_datas,                   //
	    vertex_rots);                     //

	// Everything is ready, submit to the queue.
	ret = renderer_submit_queue(r, render->r->cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	VK_CHK_AND_RET(ret, "renderer_submit_queue");

	return ret;
}

static inline XRT_CHECK_RESULT VkResult
gfx_strip_render_clear(struct comp_renderer *r,
                       struct render_gfx *render,
                       struct chl_frame_state *frame_state,
                       const struct xrt_matrix_2x2 vertex_rots[XRT_MAX_VIEWS],
                       const struct render_viewport_data target_viewport_datas[XRT_MAX_VIEWS],
                       const struct xrt_pose world_poses[XRT_MAX_VIEWS],
                       const struct xrt_pose eye_poses[XRT_MAX_VIEWS],
                       const struct xrt_fov fovs[XRT_MAX_VIEWS])
{
	COMP_TRACE_MARKER();
	assert(r->acquired_buffer >= 0);

	struct render_gfx_target_resources *target_rtr = &r->rtr_array[r->acquired_buffer];

	chl_frame_state_gfx_set_views( //
	    frame_state,               //
	    world_poses,               //
	    eye_poses,                 //
	    fovs,                      //
	    0);                        //

	chl_frame_state_gfx_set_target( //
	    frame_state,                //
	    target_rtr,                 //
	    target_viewport_datas,      //
	    vertex_rots);               //

	// Start the compute pipeline.
	render_gfx_begin(render);

	// Build the command buffer.
	comp_render_gfx_clear(render, &frame_state->data);

	// Make the command buffer submittable.
	render_gfx_end(render);

	struct vk_bundle *vk = &r->c->base.vk;
	// Everything is ready, submit to the queue.
	const VkResult ret = renderer_submit_queue(r, render->r->cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	VK_CHK_AND_RET(ret, "renderer_submit_queue");

	return ret;
}

static inline XRT_CHECK_RESULT VkResult
gfx_strip_render_squash_layers_submit(struct comp_renderer *r, VkCommandBuffer cmd)
{
	COMP_TRACE_MARKER();

	const VkPipelineStageFlags pipeline_stage_flag = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

	VkResult ret = VK_SUCCESS;
	struct vk_bundle *vk = &r->c->base.vk;

	const int64_t frame_id = r->c->frame.rendering.id;
	assert(frame_id >= 0);

	/*
	 * Wait for previous frame's work to complete.
	 */

	// Wait for the last fence, if any.
	renderer_wait_for_last_fence(r);
	assert(r->fenced_buffer < 0);

	assert(r->acquired_buffer >= 0);
	ret = vk->vkResetFences(vk->device, 1, &r->fences[r->acquired_buffer]);
	VK_CHK_AND_RET(ret, "vkResetFences");


	/*
	 * Semaphore and queue submit info setup.
	 */

	struct comp_target *ct = r->c->target;
	struct vk_semaphore_list_wait wait_sems = XRT_STRUCT_INIT;
	struct vk_semaphore_list_signal signal_sems = XRT_STRUCT_INIT;
	struct vk_submit_info_builder builder = XRT_STRUCT_INIT;

	if (requires_present_acquire_wait(r)) {
		ADD_WAIT(wait_sems, ct->semaphores.present_complete, pipeline_stage_flag, false);
	}

	// Add signal semaphore (layer_squash_complete to target).
	ADD_SIGNAL(signal_sems, ct->semaphores.layer_squash_complete, ct->semaphores.render_complete_is_timeline);

	// Build the submit info struct, handles all of the semaphores for us.
	vk_submit_info_builder_prepare( //
	    &builder,                   //
	    &wait_sems,                 //
	    &cmd,                       //
	    1,                          //
	    &signal_sems,               //
	    NULL);                      //

	// Everything prepared, now we are submitting.
	comp_target_mark_submit_begin(ct, frame_id, os_monotonic_get_ns());

	/*
	 * The renderer command buffer pool is only accessed from one thread,
	 * this satisfies the `_locked` requirement of the function. This lets
	 * us avoid taking a lot of locks. The queue lock will be taken by
	 * @ref vk_cmd_submit_locked tho.
	 */
	ret = vk_cmd_submit_locked(vk, vk->main_queue, 1, &builder.submit_info, r->fences[r->acquired_buffer]);

	// We have now completed the submit, even if we failed.
	comp_target_mark_submit_end(ct, frame_id, os_monotonic_get_ns());

	// Check after marking as submit complete.
	VK_CHK_AND_RET(ret, "vk_cmd_submit_locked");

	// This buffer now have a pending fence.
	r->fenced_buffer = r->acquired_buffer;

	return ret;
}

static inline XRT_CHECK_RESULT VkResult
gfx_strip_render_squash_layers(struct comp_renderer *r,
                               struct render_gfx *render,
                               struct chl_frame_state *frame_state,
                               const struct xrt_matrix_2x2 vertex_rots[XRT_MAX_VIEWS],
                               const struct render_viewport_data target_viewport_datas[XRT_MAX_VIEWS],
                               const struct xrt_pose world_poses[XRT_MAX_VIEWS],
                               const struct xrt_pose eye_poses[XRT_MAX_VIEWS],
                               const struct xrt_fov fovs[XRT_MAX_VIEWS])
{
	COMP_TRACE_MARKER();

	const struct comp_layer *layers = r->c->base.layer_accum.layers;
	const uint32_t layer_count = r->c->base.layer_accum.layer_count;

	assert(r->acquired_buffer >= 0);
	struct render_gfx_target_resources *target_rtr = &r->rtr_array[r->acquired_buffer];

	const VkImageLayout transition_to = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	chl_frame_state_gfx_set_views( //
	    frame_state,               //
	    world_poses,               //
	    eye_poses,                 //
	    fovs,                      //
	    layer_count);              //

	chl_frame_state_gfx_set_target( //
	    frame_state,                //
	    target_rtr,                 //
	    target_viewport_datas,      //
	    vertex_rots);               //

	// Start the compute pipeline.
	render_gfx_begin(render);

	comp_render_gfx_layers( //
	    render,             //
	    layers,             //
	    layer_count,        //
	    &frame_state->data, //
	    transition_to);     //

	// Make the command buffer submittable.
	render_gfx_end(render);

	VkResult ret = VK_SUCCESS;
	struct vk_bundle *vk = &r->c->base.vk;
	// Everything is ready, submit to the queue.
	ret = gfx_strip_render_squash_layers_submit(r,               //
	                                            render->r->cmd); //
	VK_CHK_AND_RET(ret, "gfx_strip_render_squash_layers_submit");

	return ret;
}

/*!
 * @brief Implements single-buffered strip rendering
 *
 * As described by:
 * @link https://blog.imaginationtech.com/reducing-latency-in-vr-by-using-single-buffered-strip-rendering/
 *
 * @pre render_gfx_init(render, &c->nr), r->acquired_buffer >= 0,
 *      scanout_info->strip_count > 0, layer_accum.layer_count > 0.
 */
static inline XRT_CHECK_RESULT VkResult
gfx_strip_render_strip_distortion(struct comp_renderer *r,
                                  struct render_gfx *render,
                                  struct chl_frame_state *frame_state,
                                  struct xrt_device_compositor_info *scanout_info,
                                  enum comp_target_fov_source fov_source,
                                  const struct xrt_matrix_2x2 vertex_rots[XRT_MAX_VIEWS],
                                  const struct render_viewport_data target_viewport_datas[XRT_MAX_VIEWS],
                                  struct xrt_pose world_poses_scanout_begin[XRT_MAX_VIEWS],
                                  struct xrt_pose eye_poses[XRT_MAX_VIEWS],
                                  struct xrt_fov fovs[XRT_MAX_VIEWS])
{
	COMP_TRACE_MARKER();

	/*!
	 * Currently only top to bottom scanout directions are supported. If this changes, make sure
	 * to update the runtime check in @ref renderer_ensure_images_and_renderings
	 */
	assert(scanout_info != NULL && //
	       scanout_info->scanout_direction == XRT_SCANOUT_DIRECTION_TOP_TO_BOTTOM);

	VkResult ret = VK_SUCCESS;

	// Basics
	struct vk_bundle *vk = &r->c->base.vk;
	struct comp_target *ct = r->c->target;

	assert(r->acquired_buffer >= 0);
	struct render_gfx_target_resources *target_rtr = &r->rtr_array[r->acquired_buffer];

	render_scissor_data_t target_scissor_datas[XRT_MAX_VIEWS];

	const int64_t frame_id = r->c->frame.rendering.id;
	assert(frame_id >= 0);

	const uint32_t layer_count = r->c->base.layer_accum.layer_count;
	//! should not get here with zero layer submissions
	assert(layer_count > 0);

	int64_t scanout_time_ns = scanout_info->scanout_time_ns;
	//! If unknown (0), estimate at 95% of nominal_period/frame_interval.
	if (scanout_time_ns == 0) {
		scanout_time_ns = (int64_t)(r->c->frame_interval_ns * 0.95);
	}
	assert(scanout_time_ns != 0);

	const uint32_t strip_count = scanout_info->strip_render.strip_count;
	//! clang-tidy complains about dvidie by zero despite the earlier checks and asserts.
	if (strip_count == 0) {
		COMP_ERROR(r->c, "strip_count is 0, can't do strip rendering.");
		return VK_ERROR_UNKNOWN;
	}

	assert(strip_count > 0);
	assert(strip_count <= render->r->strip_distortion.strip_count);
	const int64_t strip_duration_ns = scanout_time_ns / (int64_t)strip_count;

	const bool fast_path = frame_state->data.fast_path;
	const bool do_timewarp = frame_state->data.do_timewarp;
	const bool using_layer_squasher = !fast_path;

	const bool sem_wait_for_present_acquire = requires_present_acquire_wait(r);

	// Save render_gfx scratch state; restored at cleanup.
	const VkQueryPool main_query_pool = render->r->query_pool;
	const VkCommandPool main_cmd_pool = render->r->cmd_pool;
	const VkCommandBuffer main_cmd = render->r->cmd;
	const VkRect2D main_render_area = target_rtr->render_area;

	// Pad so the beam has clearly left a strip before we start writing it.
	const int64_t safety_margin_ns = U_TIME_1MS_IN_NS / 2;

	const int64_t max_spin_ns =
#ifdef XRT_OS_ANDROID
	    1500 * U_TIME_1US_IN_NS;
#else
	    250 * U_TIME_1US_IN_NS;
#endif

	/*!
	 * Wait for layer squasher to finish before the vsync wait, so strip 0 can't
	 * queue behind a late squash and tear on the next refresh.
	 *
	 * I don't particularly like this but short of time for more
	 * sophisticated solutions that eliminates tearing on high
	 * layer squasher loads should be fine for now.
	 */
	if (using_layer_squasher) {
		renderer_wait_for_last_fence(r);
	}

	//! For beam-chasing, anchor on the vsync that just fired
	struct xrt_vsync_event vs = {0};
	const xrt_result_t xres = comp_target_wait_next_vsync(ct, &vs);
	if (xres != XRT_SUCCESS) {
		COMP_WARN(r->c, "comp_target_wait_next_vsync failed: %d", xres);
		return VK_ERROR_UNKNOWN;
	}

	const int64_t period_ns = vs.period_ns;
	const int64_t current_vsync_ns = vs.timestamp_ns;
	const int64_t target_vsync_ns = current_vsync_ns + period_ns;

	/*!
	 * One shared warp reference for all strips,
	 * sampled at mid-scanout to centre the residual motion error.
	 *
	 * Ideally we would late-latch a new pose per strip
	 * however this will cause seams between strips during
	 * head-rotations.
	 *
	 * There are various ways to try handling this,
	 * all with different trade-offs that are not acceptable
	 * to me from testing.
	 *
	 * The only other option i have yet to try is to modify the
	 * mesh shader to do intra-strip interpolation with two poses.
	 *
	 * On the plus side it does mean there is only one pose query
	 * instead of if the strip_count is 8...
	 *
	 */
	const int64_t mid_scanout_ns = target_vsync_ns + scanout_time_ns / 2;
	calc_pose_data_at_time(        //
	    r,                         //
	    mid_scanout_ns,            //
	    fov_source,                //
	    fovs,                      //
	    world_poses_scanout_begin, //
	    eye_poses,                 //
	    render->r->view_count);    //

	for (uint32_t strip = 0; strip < strip_count; ++strip) {
		const VkFence strip_fence = render->r->strip_distortion.fences[strip];
		/*!
		 *  Wait for this strip's previous frame execution to finish,
		 *  before any timing calculations.
		 */
		ret = vk->vkWaitForFences(vk->device, 1, &strip_fence, VK_TRUE, UINT64_MAX);
		VK_CHK_WITH_GOTO(ret, "vkWaitForFences", restore_cleanup);

		// Wake once the beam has left this strip on the current refresh.
		const int64_t wake_at_ns =
		    current_vsync_ns + (int64_t)(strip + 1) * strip_duration_ns + safety_margin_ns;
		precise_sleep_until(wake_at_ns, max_spin_ns);

		// Point render_gfx at this strip's resources.
		const VkCommandBuffer strip_cmd = render->r->strip_distortion.cmds[strip];
		render->r->query_pool = render->r->strip_distortion.query_pools[strip];
		render->r->cmd_pool = render->r->strip_distortion.cmd_pools[strip];
		render->r->cmd = strip_cmd;

		comp_render_initial_init( //
		    &frame_state->data,   //
		    fast_path,            //
		    do_timewarp);         //

		calc_strip_render_area(r,                         //
		                       strip,                     //
		                       strip_count,               //
		                       scanout_info,              //
		                       &target_rtr->render_area); //

		calc_strip_view_scissors(render->r->view_count,    //
		                         &target_rtr->render_area, //
		                         target_viewport_datas,    //
		                         target_scissor_datas);    //

		chl_frame_state_gfx_set_views( //
		    frame_state,               //
		    world_poses_scanout_begin, //
		    eye_poses,                 //
		    fovs,                      //
		    layer_count);              //

		chl_frame_state_gfx_set_target_scissored( //
		    frame_state,                          //
		    target_rtr,                           //
		    vertex_rots,                          //
		    target_viewport_datas,                //
		    target_scissor_datas);                //

		// Start the graphics pipeline.
		render_gfx_begin(render);

		if (using_layer_squasher) {
			comp_render_gfx_distortion_after_squash( //
			    render,                              //
			    &frame_state->data);                 //
		} else {
			comp_render_gfx_distortion_fast_path(  //
			    render,                            //
			    &r->c->base.layer_accum.layers[0], //
			    &frame_state->data);               //
		}

		// Make the command buffer submittable.
		render_gfx_end(render);

		/*!
		 * Only strip 0 waits (squash or present-acquire); only the last
		 * strip signals render_complete. Middle strips are ordered by the
		 * queue (same queue, disjoint pixels so non-overlapping).
		 */
		struct vk_semaphore_list_wait wait_sems = XRT_STRUCT_INIT;
		struct vk_semaphore_list_signal signal_sems = XRT_STRUCT_INIT;
		struct vk_submit_info_builder builder = XRT_STRUCT_INIT;

		if (strip == 0) {
			if (using_layer_squasher) {
				assert(!sem_wait_for_present_acquire);
				ADD_WAIT(wait_sems, ct->semaphores.layer_squash_complete,
				         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				         ct->semaphores.render_complete_is_timeline);

			} else if (sem_wait_for_present_acquire) {
				ADD_WAIT(wait_sems, ct->semaphores.present_complete,
				         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, false);
			}
		}
		if ((strip + 1) == strip_count) {
			// Add signal semaphore (render_complete to target).
			ADD_SIGNAL(signal_sems, ct->semaphores.render_complete,
			           ct->semaphores.render_complete_is_timeline);
		}

		// Build the submit info struct, handles all of the semaphores for us.
		vk_submit_info_builder_prepare( //
		    &builder,                   //
		    &wait_sems,                 //
		    &strip_cmd,                 //
		    1,                          //
		    &signal_sems,               //
		    NULL);                      //

		ret = vk->vkResetFences(vk->device, 1, &strip_fence);
		VK_CHK_WITH_GOTO(ret, "vkResetFences", restore_cleanup);

		ret = vk_cmd_submit_locked(vk, vk->main_queue, 1, &builder.submit_info, strip_fence);
		VK_CHK_WITH_GOTO(ret, "vk_cmd_submit_locked", restore_cleanup);
	}

restore_cleanup:

	target_rtr->render_area = main_render_area;
	render->r->cmd = main_cmd;
	render->r->cmd_pool = main_cmd_pool;
	render->r->query_pool = main_query_pool;

	return ret;
}

/*!
 * @brief A specialized variant of @ref dispatch_gfx for front-buffer rendering
 *
 * Aims to reduce / eliminate screen tearing when using present modes from the
 * VK_KHR_shared_presentable_image extension
 *
 * Compared to @ref dispatch_gfx, @ref dispatch_gfx_strip_render strips the single
 * combined command buffer containing the commands of layer squashing and distortion passes into
 * multiple command buffers:
 *    * Fast-path:   N distortion strips
 * 	  * Normal-path: 1 + N distortion strips
 * where
 *    N = @xrt_device_compositor_info::strip_count
 *
 * @see gfx_strip_render_strip_distortion for details.
 *
 * @pre render_gfx_init(render, &c->nr)
 */
static XRT_CHECK_RESULT VkResult
dispatch_gfx_strip_render(struct comp_renderer *r,
                          struct render_gfx *render,
                          struct chl_frame_state *frame_state,
                          enum comp_target_fov_source fov_source)
{
	COMP_TRACE_MARKER();

	VkResult ret = VK_SUCCESS;

	// Basics
	struct comp_compositor *c = r->c;
	struct vk_bundle *vk = &c->base.vk;

	// Viewport information.
	struct render_viewport_data target_viewport_datas[XRT_MAX_VIEWS];
	calc_viewport_data(r, target_viewport_datas, render->r->view_count);

	// Vertex rotation information.
	struct xrt_matrix_2x2 vertex_rots[XRT_MAX_VIEWS];
	calc_vertex_rot_data(r, vertex_rots, render->r->view_count);

	// Device view information.
	struct xrt_fov fovs[XRT_MAX_VIEWS];
	struct xrt_pose world_poses_scanout_begin[XRT_MAX_VIEWS];
	struct xrt_pose eye_poses[XRT_MAX_VIEWS];

	//! Only clear screen and return if no layers.
	if (c->base.layer_accum.layer_count == 0) {
		return gfx_strip_render_clear(r,                         //
		                              render,                    //
		                              frame_state,               //
		                              vertex_rots,               //
		                              target_viewport_datas,     //
		                              world_poses_scanout_begin, //
		                              eye_poses,                 //
		                              fovs);                     //
	}

	struct xrt_device_compositor_info scanout_info = {
	    .scanout_direction = XRT_SCANOUT_DIRECTION_TOP_TO_BOTTOM,
	    .scanout_time_ns = 0,
	};
	if (r->c->xdev->supported.compositor_info) {
		const struct xrt_device_compositor_mode compositor_mode = {
		    .frame_interval_ns = r->c->frame_interval_ns,
		};
		xrt_result_t xret = xrt_device_get_compositor_info( //
		    r->c->xdev,                                     //
		    &compositor_mode,                               //
		    &scanout_info);                                 //
		if (xret != XRT_SUCCESS) {
			scanout_info.scanout_time_ns = 0;
		}
	}

	const bool using_layer_squasher = !frame_state->data.fast_path;

	if (using_layer_squasher) {
		calc_pose_data_at_time(                              //
		    r,                                               //
		    r->c->frame.rendering.predicted_display_time_ns, //
		    fov_source,                                      //
		    fovs,                                            //
		    world_poses_scanout_begin,                       //
		    eye_poses,                                       //
		    render->r->view_count);                          //

		ret = gfx_strip_render_squash_layers(r,                         //
		                                     render,                    //
		                                     frame_state,               //
		                                     vertex_rots,               //
		                                     target_viewport_datas,     //
		                                     world_poses_scanout_begin, //
		                                     eye_poses,                 //
		                                     fovs);                     //
		VK_CHK_AND_RET(ret, "gfx_strip_render_squash_layers");
	}

	ret = gfx_strip_render_strip_distortion(r,                         //
	                                        render,                    //
	                                        frame_state,               //
	                                        &scanout_info,             //
	                                        fov_source,                //
	                                        vertex_rots,               //
	                                        target_viewport_datas,     //
	                                        world_poses_scanout_begin, //
	                                        eye_poses,                 //
	                                        fovs);                     //
	VK_CHK_AND_RET(ret, "gfx_strip_render_strip_distortion");
	return ret;
}


/*
 *
 * Compute
 *
 */

/*!
 * @pre render_compute_init(render, &c->nr)
 */
static XRT_CHECK_RESULT VkResult
dispatch_compute(struct comp_renderer *r,
                 struct render_compute *render,
                 struct chl_frame_state *frame_state,
                 enum comp_target_fov_source fov_source)
{
	COMP_TRACE_MARKER();

	struct comp_compositor *c = r->c;
	struct vk_bundle *vk = &c->base.vk;
	VkResult ret;

	// Basics
	const struct comp_layer *layers = c->base.layer_accum.layers;
	uint32_t layer_count = c->base.layer_accum.layer_count;

	// Device view information.
	struct xrt_fov fovs[XRT_MAX_VIEWS];
	struct xrt_pose world_poses_scanout_begin[XRT_MAX_VIEWS];
	struct xrt_pose world_poses_scanout_end[XRT_MAX_VIEWS];
	struct xrt_pose eye_poses[XRT_MAX_VIEWS];
	calc_pose_data(                //
	    r,                         //
	    fov_source,                //
	    fovs,                      //
	    world_poses_scanout_begin, //
	    world_poses_scanout_end,   //
	    eye_poses,                 //
	    render->r->view_count);    //

	// Target Vulkan resources..
	VkImage target_image = r->c->target->images[r->acquired_buffer].handle;
	VkImageView target_storage_view = r->c->target->images[r->acquired_buffer].view;
	VkImageLayout target_final_layout = r->c->target->final_layout;

	// Target view information.
	struct render_viewport_data target_viewport_datas[XRT_MAX_VIEWS];
	calc_viewport_data(r, target_viewport_datas, render->r->view_count);

	// Does everything.
	chl_frame_state_cs_default_pipeline( //
	    frame_state,                     //
	    render,                          //
	    layers,                          //
	    layer_count,                     //
	    world_poses_scanout_begin,       //
	    world_poses_scanout_end,         //
	    eye_poses,                       //
	    fovs,                            //
	    target_image,                    //
	    target_storage_view,             //
	    target_final_layout,             //
	    target_viewport_datas);          //

	// Everything is ready, submit to the queue.
	ret = renderer_submit_queue(r, render->r->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	VK_CHK_AND_RET(ret, "renderer_submit_queue");

	return ret;
}


/*
 *
 * Interface functions.
 *
 */

XRT_CHECK_RESULT xrt_result_t
comp_renderer_draw(struct comp_renderer *r)
{
	COMP_TRACE_MARKER();

	struct comp_target *ct = r->c->target;
	struct comp_compositor *c = r->c;

	// Check that we don't have any bad data.
	assert(!comp_frame_is_invalid_locked(&c->frame.waited));
	assert(comp_frame_is_invalid_locked(&c->frame.rendering));

	// Move waited frame to rendering frame, clear waited.
	comp_frame_move_and_clear_locked(&c->frame.rendering, &c->frame.waited);

	// Tell the target we are starting to render, for frame timing.
	comp_target_mark_begin(ct, c->frame.rendering.id, os_monotonic_get_ns());

	// Are we ready to render? No - skip rendering.
	if (!comp_target_check_ready(r->c->target)) {
		// Need to emulate rendering for the timing.
		//! @todo This should be discard.
		comp_target_mark_submit_begin(ct, c->frame.rendering.id, os_monotonic_get_ns());
		comp_target_mark_submit_end(ct, c->frame.rendering.id, os_monotonic_get_ns());

		// Clear the rendering frame.
		comp_frame_clear_locked(&c->frame.rendering);
		return XRT_SUCCESS;
	}

	comp_target_flush(ct);

	comp_target_update_timings(ct);

	if (r->acquired_buffer < 0) {
		// Ensures that renderings are created.
		renderer_acquire_swapchain_image(r);
	}

	comp_target_update_timings(ct);

	// Hardcoded for now.
	const uint32_t view_count = c->nr.view_count;
	enum comp_target_fov_source fov_source = COMP_TARGET_FOV_SOURCE_DISTORTION;

	bool fast_path = c->base.frame_params.one_projection_layer_fast_path;
	bool do_timewarp = !c->debug.atw_off;

	// Consistency check.
	assert(!fast_path || c->base.layer_accum.layer_count >= 1);

	// For scratch image debugging.
	struct chl_frame_state frame_state;
	chl_frame_state_init( //
	    &frame_state,     //
	    &c->nr,           //
	    view_count,       //
	    do_timewarp,      //
	    fast_path,        //
	    &c->scratch);     //

	bool use_compute = r->settings->use_compute;
	struct render_gfx render_g = {0};
	struct render_compute render_c = {0};

	VkResult res = VK_SUCCESS;
	if (use_compute) {
		render_compute_init(&render_c, &c->nr);
		res = dispatch_compute(r, &render_c, &frame_state, fov_source);
	} else {
		render_gfx_init(&render_g, &c->nr);
		res = dispatch_graphics(r, &render_g, &frame_state, fov_source);
	}
	if (res != VK_SUCCESS) {
		return XRT_ERROR_VULKAN;
	}

#ifdef XRT_FEATURE_WINDOW_PEEK
	if (c->peek) {
		switch (comp_window_peek_get_eye(c->peek)) {
		case COMP_WINDOW_PEEK_EYE_LEFT: {
			uint32_t scratch_index = frame_state.scratch_state.views[0].index;
			struct comp_scratch_single_images *view = &c->scratch.views[0].cssi;

			comp_window_peek_blit(                 //
			    c->peek,                           //
			    view->images[scratch_index].image, //
			    view->info.width,                  //
			    view->info.height);                //
		} break;
		case COMP_WINDOW_PEEK_EYE_RIGHT: {
			uint32_t scratch_index = frame_state.scratch_state.views[1].index;
			struct comp_scratch_single_images *view = &c->scratch.views[1].cssi;

			comp_window_peek_blit(                 //
			    c->peek,                           //
			    view->images[scratch_index].image, //
			    view->info.width,                  //
			    view->info.height);                //
		} break;
		case COMP_WINDOW_PEEK_EYE_BOTH:
			/* TODO: display the undistorted image */
			comp_window_peek_blit(c->peek, c->target->images[r->acquired_buffer].handle, c->target->width,
			                      c->target->height);
			break;
		}
	}
#endif

	renderer_present_swapchain_image(r, c->frame.rendering.desired_present_time_ns,
	                                 c->frame.rendering.present_slop_ns);

	// Save for timestamps below.
	uint64_t frame_id = c->frame.rendering.id;
	uint64_t desired_present_time_ns = c->frame.rendering.desired_present_time_ns;
	uint64_t predicted_display_time_ns = c->frame.rendering.predicted_display_time_ns;

	// Clear the rendered frame.
	comp_frame_clear_locked(&c->frame.rendering);

	xrt_result_t xret = XRT_SUCCESS;
	comp_mirror_fixup_ui_state(&r->mirror_to_debug_gui, c);
	if (comp_mirror_is_ready_and_active(&r->mirror_to_debug_gui, c, predicted_display_time_ns)) {

		uint32_t scratch_index = frame_state.scratch_state.views[0].index;
		struct comp_scratch_single_images *view = &c->scratch.views[0].cssi;
		struct render_scratch_color_image *rsci = &view->images[scratch_index];
		VkExtent2D extent = {view->info.width, view->info.width};

		// Used for both, want clamp to edge to no bring in black.
		VkSampler clamp_to_edge = c->nr.samplers.clamp_to_edge;

		// Covers the whole view.
		struct xrt_normalized_rect rect = {0, 0, 1.0f, 1.0f};

		xret = comp_mirror_do_blit(    //
		    &r->mirror_to_debug_gui,   //
		    &c->base.vk,               //
		    frame_id,                  //
		    predicted_display_time_ns, //
		    rsci->image,               //
		    rsci->srgb_view,           //
		    clamp_to_edge,             //
		    extent,                    //
		    rect);                     //
	}

	/*
	 * This fixes a lot of validation issues as it makes sure that the
	 * command buffer has completed and all resources referred by it can
	 * now be manipulated.
	 *
	 * This is done after a swap so isn't time critical.
	 */
	renderer_wait_queue_idle(r);

	/*
	 * Free any resources and finalize the scratch images,
	 * which sends them send to debug UI if it is active.
	 */
	chl_frame_state_fini(&frame_state);

	// Check timestamps.
	if (xret == XRT_SUCCESS) {
		/*
		 * Get timestamps of GPU work (if available).
		 */

		uint64_t gpu_start_ns, gpu_end_ns;
		if (render_resources_get_timestamps(&c->nr, &gpu_start_ns, &gpu_end_ns)) {
			uint64_t now_ns = os_monotonic_get_ns();
			comp_target_info_gpu(ct, frame_id, gpu_start_ns, gpu_end_ns, now_ns);
		}
	}


	/*
	 * Free resources.
	 */

	if (use_compute) {
		render_compute_fini(&render_c);
	} else {
		render_gfx_fini(&render_g);
	}

	renderer_wait_for_present(r, desired_present_time_ns);

	comp_target_update_timings(ct);

	return xret;
}

struct comp_renderer *
comp_renderer_create(struct comp_compositor *c, VkExtent2D scratch_extent)
{
	struct comp_renderer *r = U_TYPED_CALLOC(struct comp_renderer);

	renderer_init(r, c, scratch_extent);

	return r;
}

void
comp_renderer_destroy(struct comp_renderer **ptr_r)
{
	if (ptr_r == NULL) {
		return;
	}

	struct comp_renderer *r = *ptr_r;
	if (r == NULL) {
		return;
	}

	renderer_fini(r);

	free(r);
	*ptr_r = NULL;
}

void
comp_renderer_add_debug_vars(struct comp_renderer *self)
{
	struct comp_renderer *r = self;

	comp_mirror_add_debug_vars(&r->mirror_to_debug_gui, r->c);
}
