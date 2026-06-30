// Copyright 2019-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
// Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
// Author: Pete Black <pete.black@collabora.com>
// Author: Jakob Bornecrantz <jakob@collabora.com>

#version 450

// Should we do timewarp.
layout(constant_id = 0) const bool do_timewarp = false;

layout (binding = 1, std140) uniform Config
{
	vec4 vertex_rot;
	vec4 post_transform;

	// Only used for timewarp.
	vec4 pre_transform;

	mat4 transform_timewarp_scanout_begin;
	mat4 transform_timewarp_scanout_end;

	//! Strip extent on the scan axis in NDC: .x = min, .y = extent., .zw unused.
	vec4 strip_range;

	//! 0 = y (top-to-bottom scan), 1 = x (left-to-right scan).
	uint scan_axis;
} ubo;

layout (location = 0)  in vec4 in_pos_ruv;
layout (location = 1)  in vec4 in_guv_buv;

/*!
 * @ref out_[r|g|b]_uv details:
 *
 * .xy = begin-warped UV, .zw = end-warped UV per color channel.
 *
 * mesh.frag blends these using the interpolated @ref out_t.
 *
 * Why per-fragment: For strip-rendering, if we blended per-vertex,
 * triangles crossing a strip boundary would produce slightly different
 * UVs in adjacent render passes causing a visual seam.
 *
 * Deferring the blend to the fragment shader guarantees (bit-)identical output
 * at the shared boundary row.
 *
 * For non-timewarp or non-strip-rendering case, .xy == .zw, so the blend is a no-op.
 *
 */
layout (location = 0) out vec4 out_r_uv;
layout (location = 1) out vec4 out_g_uv;
layout (location = 2) out vec4 out_b_uv;

/*!
 * @ref out_t details:
 *
 * Unclamped strip blend factor. Rasterizer interpolates linearly; the
 * mesh.frag clamps to [0, 1]. Leaving it unclamped per-vertex
 * for a triangle spanning a strip boundary,
 *
 * Unclamped per-vertex t's interpolate linearly through the boundary,
 * so the boundary fragment gets exactly
 * t = 1 (strip N-1) or t = 0 (strip N) — picking the shared matrix.
 *
 */
layout (location = 3) out float out_t;

out gl_PerVertex
{
	vec4 gl_Position;
};


vec2 transform_uv_subimage(vec2 uv)
{
	// To deal with OpenGL flip and sub image view.
	return fma(uv, ubo.post_transform.zw, ubo.post_transform.xy);
}

vec2 transform_uv_timewarp(vec2 uv, mat4 warp_transform)
{
	vec4 values = vec4(uv, -1, 1);

	// From uv to tan angle (tangent space).
	values.xy = fma(values.xy, ubo.pre_transform.zw, ubo.pre_transform.xy);
	values.y = -values.y; // Flip to OpenXR coordinate system.

	// Timewarp.
	values = warp_transform * values;
	values.xy = values.xy * (1.0 / max(values.w, 0.00001));

	// From [-1, 1] to [0, 1]
	values.xy = values.xy * 0.5 + 0.5;

	// To deal with OpenGL flip and sub image view.
	values.xy = fma(values.xy, ubo.post_transform.zw, ubo.post_transform.xy);

	// Done.
	return values.xy;
}

/*!
 * Returns a begin & end-warped UVs packed into a vec4 (.xy = begin, .zw = end).
 * @see out_[r|g|b]_uv details
 */
vec4 transform_uv_pair(vec2 uv)
{
	if (do_timewarp) {
		return vec4(transform_uv_timewarp(uv, ubo.transform_timewarp_scanout_begin),
					transform_uv_timewarp(uv, ubo.transform_timewarp_scanout_end));
	} else {
		const vec2 sub = transform_uv_subimage(uv);
		return vec4(sub, sub);
	}
}

void main()
{
	const mat2x2 rot = {
		ubo.vertex_rot.xy,
		ubo.vertex_rot.zw
	};

	const vec2 pos = rot * in_pos_ruv.xy;
	gl_Position = vec4(pos, 0.0f, 1.0f);

	const float axis_pos = (ubo.scan_axis == 0u) ? pos.y : pos.x;
	out_t = (axis_pos - ubo.strip_range.x) / ubo.strip_range.y;

	out_r_uv = transform_uv_pair(in_pos_ruv.zw);
	out_g_uv = transform_uv_pair(in_guv_buv.xy);
	out_b_uv = transform_uv_pair(in_guv_buv.zw);
}
