// Copyright 2017, James Sarrett.
// Copyright 2017, Bastiaan Olij.
// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
// Author: James Sarrett <jsarrett@gmail.com>
// Author: Bastiaan Olij <mux213@gmail.com>
// Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
// Author: Pete Black <pete.black@collabora.com>

#version 450


layout (binding = 0) uniform sampler2D tex_sampler;

/*!
 * @ref in_[r|g|b]_uv details:
 *
 * .xy = begin-warped UV, .zw = end-warped UV per color channel.
 * @see out_[r|g|b]_uv in mesh.vert	details.
 */
layout (location = 0)  in vec4 in_r_uv;
layout (location = 1)  in vec4 in_g_uv;
layout (location = 2)  in vec4 in_b_uv;

/*!
 * Strip blend factor from mesh.vert, unclamped so that
 * rasterizer interpolation hits t = 0 / t = 1 exactly on strip
 * boundaries (see mesh.vert for details).
*/
layout (location = 3) in float in_t;

layout (location = 0) out vec4 out_color;


void main()
{
	const float t = clamp(in_t, 0.0, 1.0);

	const vec2 r_uv = mix(in_r_uv.xy, in_r_uv.zw, t);
	const vec2 g_uv = mix(in_g_uv.xy, in_g_uv.zw, t);
	const vec2 b_uv = mix(in_b_uv.xy, in_b_uv.zw, t);

	const float r = texture(tex_sampler, r_uv).x;
	const float g = texture(tex_sampler, g_uv).y;
	const float b = texture(tex_sampler, b_uv).z;

	out_color = vec4(r, g, b, 1.0);
}
