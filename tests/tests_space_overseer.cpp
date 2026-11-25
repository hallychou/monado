// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Tests for adding and removing tracking origins on b_space_overseer.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_tracking.h"

#include "util/u_device_id.h"
#include "util/u_tracking_origin.h"

#include "b_space_overseer.h"

#include "catch_amalgamated.hpp"

#include <cinttypes>
#include <cstdio>


namespace {

struct Fixture
{
	xrt_space_overseer *xso = nullptr;

	Fixture()
	{
		xso = (xrt_space_overseer *)b_space_overseer_create(nullptr);
	}

	~Fixture()
	{
		xrt_space_overseer_destroy(&xso);
	}
};

xrt_pose
make_pose(float x, float y, float z)
{
	xrt_pose pose = XRT_POSE_IDENTITY;
	pose.position.x = x;
	pose.position.y = y;
	pose.position.z = z;
	return pose;
}

void
init_origin(xrt_tracking_origin &xto, enum xrt_tracking_type type, const xrt_pose &initial_offset)
{
	u_tracking_origin_init(&xto, type, "Test Origin");
	xto.initial_offset = initial_offset;
}

xrt_result_t
get_identity_tracked_pose(xrt_device *xdev,
                          enum xrt_input_name name,
                          int64_t at_timestamp_ns,
                          xrt_space_relation *out_relation)
{
	*out_relation = {};
	out_relation->pose = XRT_POSE_IDENTITY;
	out_relation->relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                               XRT_SPACE_RELATION_POSITION_VALID_BIT);
	return XRT_SUCCESS;
}

void
init_device(xrt_device &xdev, xrt_tracking_origin *xto)
{
	xdev = {};
	xdev.tracking_origin = xto;
	xdev.get_tracked_pose = get_identity_tracked_pose;
	u_device_id_assign(&xdev);
	snprintf(xdev.str, sizeof(xdev.str), "Device %" PRIu64, xdev.id.val);
}

xrt_space_relation
locate_device_in_root(xrt_space_overseer *xso, xrt_device *xdev)
{
	xrt_pose identity = make_pose(0.0f, 0.0f, 0.0f);
	xrt_space_relation rel = {};
	xrt_result_t xret = xrt_space_overseer_locate_device(xso, xso->semantic.root, &identity, 0, xdev, &rel);
	CHECK(xret == XRT_SUCCESS);
	return rel;
}

xrt_space_relation
locate_space_in_root(xrt_space_overseer *xso, xrt_space *space)
{
	xrt_pose identity = make_pose(0.0f, 0.0f, 0.0f);
	xrt_space_relation rel = {};
	xrt_result_t xret =
	    xrt_space_overseer_locate_space(xso, xso->semantic.root, &identity, 0, space, &identity, &rel);
	CHECK(xret == XRT_SUCCESS);
	return rel;
}

bool
rel_position_valid(const xrt_space_relation &rel)
{
	return (rel.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0;
}

} // namespace


TEST_CASE("add_tracking_origin")
{
	Fixture f;
	xrt_tracking_origin xto;
	init_origin(xto, XRT_TRACKING_TYPE_OTHER, make_pose(1.0f, 2.0f, 3.0f));

	xrt_pose offset = {};

	SECTION("unknown origin has no offset")
	{
		CHECK(xrt_space_overseer_get_tracking_origin_offset(f.xso, &xto, &offset) != XRT_SUCCESS);
	}

	SECTION("adding creates a space with the initial offset")
	{
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_get_tracking_origin_offset(f.xso, &xto, &offset) == XRT_SUCCESS);
		CHECK(offset.position.x == 1.0f);
		CHECK(offset.position.y == 2.0f);
		CHECK(offset.position.z == 3.0f);
	}

	SECTION("adding again is a no-op that keeps the offset")
	{
		xrt_pose changed = make_pose(4.0f, 5.0f, 6.0f);
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_set_tracking_origin_offset(f.xso, &xto, &changed) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_get_tracking_origin_offset(f.xso, &xto, &offset) == XRT_SUCCESS);
		CHECK(offset.position.x == 4.0f);
	}

	SECTION("origin without an ID is rejected")
	{
		xto.id.val = 0;
		CHECK(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_ERROR_INVALID_ARGUMENT);

		xrt_device xdev;
		init_device(xdev, &xto);
		CHECK(xrt_space_overseer_add_device(f.xso, &xdev) == XRT_ERROR_INVALID_ARGUMENT);
	}

	SECTION("attachable origin gets an attachable space")
	{
		xrt_tracking_origin attachable;
		init_origin(attachable, XRT_TRACKING_TYPE_ATTACHABLE, make_pose(0.0f, 0.0f, 0.0f));
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &attachable) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_get_tracking_origin_offset(f.xso, &attachable, &offset) ==
		      XRT_ERROR_UNSUPPORTED_SPACE_TYPE);

		xrt_device xdev;
		init_device(xdev, &attachable);
		REQUIRE(xrt_space_overseer_add_device(f.xso, &xdev) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_attach_device(f.xso, &xdev, f.xso->semantic.root) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_remove_device(f.xso, &xdev) == XRT_SUCCESS);

		// Not attachable once removed, even though its origin is still added.
		CHECK(xrt_space_overseer_attach_device(f.xso, &xdev, f.xso->semantic.root) ==
		      XRT_ERROR_DEVICE_NOT_ATTACHABLE);
	}

	SECTION("devices that were never added can't be attached")
	{
		xrt_tracking_origin attachable;
		init_origin(attachable, XRT_TRACKING_TYPE_ATTACHABLE, make_pose(0.0f, 0.0f, 0.0f));
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &attachable) == XRT_SUCCESS);

		xrt_device xdev;
		init_device(xdev, &attachable);
		CHECK(xrt_space_overseer_attach_device(f.xso, &xdev, f.xso->semantic.root) ==
		      XRT_ERROR_DEVICE_NOT_ATTACHABLE);
	}
}

TEST_CASE("devices_use_added_tracking_origin")
{
	Fixture f;
	xrt_tracking_origin xto;
	init_origin(xto, XRT_TRACKING_TYPE_OTHER, make_pose(1.0f, 0.0f, 0.0f));

	xrt_device a;
	xrt_device b;
	init_device(a, &xto);
	init_device(b, &xto);

	xrt_pose changed = make_pose(0.0f, 2.0f, 0.0f);

	REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
	REQUIRE(xrt_space_overseer_add_device(f.xso, &a) == XRT_SUCCESS);
	REQUIRE(xrt_space_overseer_set_tracking_origin_offset(f.xso, &xto, &changed) == XRT_SUCCESS);
	REQUIRE(xrt_space_overseer_add_device(f.xso, &b) == XRT_SUCCESS);

	xrt_space_relation rel_a = locate_device_in_root(f.xso, &a);
	xrt_space_relation rel_b = locate_device_in_root(f.xso, &b);
	CHECK(rel_position_valid(rel_a));
	CHECK(rel_position_valid(rel_b));
	CHECK(rel_a.pose.position.y == 2.0f);
	CHECK(rel_b.pose.position.y == 2.0f);

	CHECK(xrt_space_overseer_remove_device(f.xso, &a) == XRT_SUCCESS);
	CHECK(xrt_space_overseer_remove_device(f.xso, &b) == XRT_SUCCESS);
	CHECK(xrt_space_overseer_remove_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
}

TEST_CASE("add_device_requires_tracking_origin")
{
	Fixture f;
	xrt_tracking_origin xto;
	init_origin(xto, XRT_TRACKING_TYPE_OTHER, make_pose(1.0f, 0.0f, 0.0f));

	xrt_device xdev;
	init_device(xdev, &xto);

	xrt_pose offset = {};

	SECTION("never added")
	{
		CHECK(xrt_space_overseer_add_device(f.xso, &xdev) == XRT_ERROR_TRACKING_ORIGIN_NOT_ADDED);
	}

	SECTION("already removed")
	{
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_remove_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_add_device(f.xso, &xdev) == XRT_ERROR_TRACKING_ORIGIN_NOT_ADDED);
	}

	// Not added implicitly by the failed add.
	CHECK(xrt_space_overseer_get_tracking_origin_offset(f.xso, &xto, &offset) != XRT_SUCCESS);

	xrt_space_relation rel = locate_device_in_root(f.xso, &xdev);
	CHECK_FALSE(rel_position_valid(rel));
}

TEST_CASE("remove_tracking_origin")
{
	Fixture f;
	xrt_tracking_origin xto;
	init_origin(xto, XRT_TRACKING_TYPE_OTHER, make_pose(1.0f, 0.0f, 0.0f));

	xrt_pose offset = {};

	SECTION("removing an unknown origin is a no-op")
	{
		CHECK(xrt_space_overseer_remove_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
	}

	SECTION("removing twice is a no-op")
	{
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_remove_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_remove_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
	}

	SECTION("offset is gone after removal and re-adding starts over")
	{
		xrt_pose changed = make_pose(0.0f, 2.0f, 0.0f);
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_set_tracking_origin_offset(f.xso, &xto, &changed) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_remove_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_get_tracking_origin_offset(f.xso, &xto, &offset) != XRT_SUCCESS);
		CHECK(xrt_space_overseer_set_tracking_origin_offset(f.xso, &xto, &changed) != XRT_SUCCESS);

		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_get_tracking_origin_offset(f.xso, &xto, &offset) == XRT_SUCCESS);
		CHECK(offset.position.x == 1.0f);
		CHECK(offset.position.y == 0.0f);
	}

	SECTION("devices still added can no longer be located")
	{
		xrt_device xdev;
		init_device(xdev, &xto);

		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_add_device(f.xso, &xdev) == XRT_SUCCESS);

		xrt_space *before = nullptr;
		REQUIRE(xrt_space_overseer_create_pose_space(f.xso, &xdev, XRT_INPUT_GENERIC_TRACKER_POSE, &before) ==
		        XRT_SUCCESS);
		REQUIRE(rel_position_valid(locate_device_in_root(f.xso, &xdev)));
		REQUIRE(rel_position_valid(locate_space_in_root(f.xso, before)));

		REQUIRE(xrt_space_overseer_remove_tracking_origin(f.xso, &xto) == XRT_SUCCESS);

		xrt_space *after = nullptr;
		REQUIRE(xrt_space_overseer_create_pose_space(f.xso, &xdev, XRT_INPUT_GENERIC_TRACKER_POSE, &after) ==
		        XRT_SUCCESS);

		CHECK_FALSE(rel_position_valid(locate_device_in_root(f.xso, &xdev)));
		CHECK_FALSE(rel_position_valid(locate_space_in_root(f.xso, before)));
		CHECK_FALSE(rel_position_valid(locate_space_in_root(f.xso, after)));

		CHECK(xrt_space_overseer_remove_device(f.xso, &xdev) == XRT_SUCCESS);
		CHECK_FALSE(rel_position_valid(locate_device_in_root(f.xso, &xdev)));

		xrt_space_reference(&before, nullptr);
		xrt_space_reference(&after, nullptr);
	}

	SECTION("devices still added are not revived by re-adding the origin")
	{
		xrt_device old_dev;
		xrt_device new_dev;
		init_device(old_dev, &xto);
		init_device(new_dev, &xto);

		xrt_pose changed = make_pose(0.0f, 2.0f, 0.0f);
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_add_device(f.xso, &old_dev) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_set_tracking_origin_offset(f.xso, &xto, &changed) == XRT_SUCCESS);

		xrt_space *old_pose_space = nullptr;
		REQUIRE(xrt_space_overseer_create_pose_space(f.xso, &old_dev, XRT_INPUT_GENERIC_TRACKER_POSE,
		                                             &old_pose_space) == XRT_SUCCESS);

		REQUIRE(xrt_space_overseer_remove_tracking_origin(f.xso, &xto) == XRT_SUCCESS);

		// Re-added, starting over from the initial offset.
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &xto) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_add_device(f.xso, &new_dev) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_get_tracking_origin_offset(f.xso, &xto, &offset) == XRT_SUCCESS);
		CHECK(offset.position.x == 1.0f);
		CHECK(offset.position.y == 0.0f);

		CHECK_FALSE(rel_position_valid(locate_device_in_root(f.xso, &old_dev)));
		CHECK_FALSE(rel_position_valid(locate_space_in_root(f.xso, old_pose_space)));

		xrt_space_relation rel_new = locate_device_in_root(f.xso, &new_dev);
		CHECK(rel_position_valid(rel_new));
		CHECK(rel_new.pose.position.x == 1.0f);

		// Removing and adding the old device again moves it to the re-added origin.
		CHECK(xrt_space_overseer_remove_device(f.xso, &old_dev) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_add_device(f.xso, &old_dev) == XRT_SUCCESS);
		CHECK(rel_position_valid(locate_device_in_root(f.xso, &old_dev)));

		// Pose spaces from before stay dead, new ones follow the re-added origin.
		xrt_space *new_pose_space = nullptr;
		REQUIRE(xrt_space_overseer_create_pose_space(f.xso, &old_dev, XRT_INPUT_GENERIC_TRACKER_POSE,
		                                             &new_pose_space) == XRT_SUCCESS);
		CHECK_FALSE(rel_position_valid(locate_space_in_root(f.xso, old_pose_space)));
		CHECK(rel_position_valid(locate_space_in_root(f.xso, new_pose_space)));

		CHECK(xrt_space_overseer_remove_device(f.xso, &old_dev) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_remove_device(f.xso, &new_dev) == XRT_SUCCESS);

		xrt_space_reference(&old_pose_space, nullptr);
		xrt_space_reference(&new_pose_space, nullptr);
	}

	SECTION("devices of a removed attachable origin can't be attached")
	{
		xrt_tracking_origin attachable;
		init_origin(attachable, XRT_TRACKING_TYPE_ATTACHABLE, make_pose(0.0f, 0.0f, 0.0f));

		xrt_device xdev;
		init_device(xdev, &attachable);
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &attachable) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_add_device(f.xso, &xdev) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_remove_tracking_origin(f.xso, &attachable) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_attach_device(f.xso, &xdev, f.xso->semantic.root) != XRT_SUCCESS);
		CHECK_FALSE(rel_position_valid(locate_device_in_root(f.xso, &xdev)));

		// Still can't be attached after the origin is re-added, it stays on the removed space.
		REQUIRE(xrt_space_overseer_add_tracking_origin(f.xso, &attachable) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_attach_device(f.xso, &xdev, f.xso->semantic.root) ==
		      XRT_ERROR_DEVICE_NOT_ATTACHABLE);
		CHECK_FALSE(rel_position_valid(locate_device_in_root(f.xso, &xdev)));

		// Removing and adding the device again makes it attachable.
		CHECK(xrt_space_overseer_remove_device(f.xso, &xdev) == XRT_SUCCESS);
		REQUIRE(xrt_space_overseer_add_device(f.xso, &xdev) == XRT_SUCCESS);
		CHECK(xrt_space_overseer_attach_device(f.xso, &xdev, f.xso->semantic.root) == XRT_SUCCESS);
		CHECK(rel_position_valid(locate_device_in_root(f.xso, &xdev)));

		CHECK(xrt_space_overseer_remove_device(f.xso, &xdev) == XRT_SUCCESS);
	}
}
