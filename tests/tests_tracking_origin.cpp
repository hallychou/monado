// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Tests for util/u_tracking_origin.h
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 */

#include "xrt/xrt_device.h"

#include "util/u_misc.h"
#include "util/u_tracking_origin.h"

#include "catch_amalgamated.hpp"

#include <cstring>
#include <string>


TEST_CASE("init")
{
	xrt_tracking_origin a = {};
	xrt_tracking_origin b = {};
	a.initial_offset.position.y = 5.0f;

	u_tracking_origin_init(&a, XRT_TRACKING_TYPE_LIGHTHOUSE, "Origin %d", 1);
	u_tracking_origin_init(&b, XRT_TRACKING_TYPE_OTHER, "Origin %d", 2);

	CHECK(a.id.val != 0);
	CHECK(b.id.val != 0);
	CHECK(a.id.val != b.id.val);

	CHECK(a.type == XRT_TRACKING_TYPE_LIGHTHOUSE);
	CHECK(b.type == XRT_TRACKING_TYPE_OTHER);

	CHECK(std::string(a.name) == "Origin 1");
	CHECK(std::string(b.name) == "Origin 2");

	CHECK(a.initial_offset.position.x == 0.0f);
	CHECK(a.initial_offset.position.y == 0.0f);
	CHECK(a.initial_offset.position.z == 0.0f);
	CHECK(a.initial_offset.orientation.x == 0.0f);
	CHECK(a.initial_offset.orientation.y == 0.0f);
	CHECK(a.initial_offset.orientation.z == 0.0f);
	CHECK(a.initial_offset.orientation.w == 1.0f);

	SECTION("again gives a new ID")
	{
		uint64_t first = a.id.val;
		u_tracking_origin_init(&a, XRT_TRACKING_TYPE_NONE, "Again");
		CHECK(a.id.val != 0);
		CHECK(a.id.val != first);
	}
}

TEST_CASE("init_truncates_name")
{
	std::string long_name(XRT_TRACKING_NAME_LEN * 2, 'x');

	xrt_tracking_origin xto = {};
	u_tracking_origin_init(&xto, XRT_TRACKING_TYPE_NONE, "%s", long_name.c_str());

	CHECK(std::strlen(xto.name) == XRT_TRACKING_NAME_LEN - 1);
}

TEST_CASE("check_xdev")
{
	xrt_tracking_origin xto = {};
	u_tracking_origin_init(&xto, XRT_TRACKING_TYPE_NONE, "Origin");

	xrt_device with_origin = {};
	xrt_device no_origin = {};
	with_origin.tracking_origin = &xto;

	u_tracking_origin_check_xdev(&with_origin);
	u_tracking_origin_check_xdev(&no_origin);
	u_tracking_origin_check_xdev(nullptr);
}

TEST_CASE("check_xdevs")
{
	xrt_tracking_origin shared = {};
	xrt_tracking_origin own = {};
	u_tracking_origin_init(&shared, XRT_TRACKING_TYPE_NONE, "Shared");
	u_tracking_origin_init(&own, XRT_TRACKING_TYPE_NONE, "Own");

	xrt_device a = {};
	xrt_device b = {};
	xrt_device c = {};
	xrt_device no_origin = {};
	a.tracking_origin = &shared;
	b.tracking_origin = &shared;
	c.tracking_origin = &own;

	xrt_device *xdevs[] = {&a, nullptr, &b, &no_origin, &c};
	u_tracking_origin_check_xdevs(xdevs, ARRAY_SIZE(xdevs));
	u_tracking_origin_check_xdevs(nullptr, 0);
}
