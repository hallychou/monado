// Copyright 2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Stereographic unprojection
 * @author Moshi Turner <moshiturner@protonmail.com>
 * @ingroup tracking
 */

#pragma once
#include "math/m_eigen_interop.hpp"

static inline Eigen::Vector3f
stereographic_unprojection(float sg_x, float sg_y)
{
	const float X = sg_x;
	const float Y = sg_y;

	const float denom = (1 + X * X + Y * Y);

	const float x = (2 * X) / denom;
	const float y = (2 * Y) / denom;
	const float z = (-1 + X * X + Y * Y) / denom;

	// forward is -z
	return {x, y, z};
	// return {x / -z, y / -z};
}
