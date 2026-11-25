// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Tests for util/u_tracking_origin_id.h
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 */

#include "xrt/xrt_device.h"

#include "util/u_device.h"
#include "util/u_device_id.h"
#include "util/u_tracking_origin_id.h"

#include "catch_amalgamated.hpp"


TEST_CASE("generate")
{
	xrt_tracking_origin_id a = u_tracking_origin_id_generate();
	xrt_tracking_origin_id b = u_tracking_origin_id_generate();
	xrt_device_id c = u_device_id_generate();

	CHECK(a.val != 0);
	CHECK(b.val != 0);
	CHECK(a.val != b.val);
	CHECK(a.val != c.val);
	CHECK(b.val != c.val);
}

TEST_CASE("u_device_allocate")
{
	xrt_device *xdev = U_DEVICE_ALLOCATE(xrt_device, U_DEVICE_ALLOC_TRACKING_NONE, 0, 0);
	REQUIRE(xdev != nullptr);
	REQUIRE(xdev->tracking_origin != nullptr);
	CHECK(xdev->tracking_origin->id.val != 0);
	CHECK(xdev->tracking_origin->id.val != xdev->id.val);
	u_device_free(xdev);
}
