// Copyright 2022-2023, Collabora, Ltd.
// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Helpers for probing and querying @ref xrt_system_devices.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_util
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_system_helpers.h"

#include <assert.h>
#include <string.h>


/*
 *
 * Env variable options.
 *
 */

DEBUG_GET_ONCE_OPTION(ht_left_unobstructed, "XRT_DEVICE_HAND_TRACKER_LEFT_UNOBSTRUCTED_SERIAL", NULL)
DEBUG_GET_ONCE_OPTION(ht_right_unobstructed, "XRT_DEVICE_HAND_TRACKER_RIGHT_UNOBSTRUCTED_SERIAL", NULL)
DEBUG_GET_ONCE_OPTION(ht_left_conforming, "XRT_DEVICE_HAND_TRACKER_LEFT_CONFORMING_SERIAL", NULL)
DEBUG_GET_ONCE_OPTION(ht_right_conforming, "XRT_DEVICE_HAND_TRACKER_RIGHT_CONFORMING_SERIAL", NULL)


/*
 *
 * Helper functions.
 *
 */

static bool
xdev_has_input(const struct xrt_device *xdev, enum xrt_input_name name)
{
	if (xdev->inputs == NULL) {
		return false;
	}

	for (uint32_t i = 0; i < xdev->input_count; i++) {
		if (xdev->inputs[i].name == name) {
			return true;
		}
	}

	return false;
}


/*
 *
 * Exported functions.
 *
 */

xrt_result_t
u_system_devices_create_from_prober(struct xrt_instance *xinst,
                                    struct xrt_session_event_sink *broadcast,
                                    struct xrt_system_devices **out_xsysd,
                                    struct xrt_space_overseer **out_xso)
{
	xrt_result_t xret;

	assert(out_xsysd != NULL);
	assert(*out_xsysd == NULL);

	/*
	 * Create the devices.
	 */

	struct xrt_prober *xp = NULL;
	xret = xrt_instance_get_prober(xinst, &xp);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	xret = xrt_prober_probe(xp);
	if (xret < 0) {
		return xret;
	}

	return xrt_prober_create_system(xp, broadcast, out_xsysd, out_xso);
}

struct xrt_device *
u_system_devices_get_ht_device(struct xrt_system_devices *xsysd, enum xrt_input_name name)
{
	const char *ht_serial = NULL;
	switch (name) {
	case XRT_INPUT_HT_UNOBSTRUCTED_LEFT: ht_serial = debug_get_option_ht_left_unobstructed(); break;
	case XRT_INPUT_HT_UNOBSTRUCTED_RIGHT: ht_serial = debug_get_option_ht_right_unobstructed(); break;
	case XRT_INPUT_HT_CONFORMING_LEFT: ht_serial = debug_get_option_ht_left_conforming(); break;
	case XRT_INPUT_HT_CONFORMING_RIGHT: ht_serial = debug_get_option_ht_right_conforming(); break;
	default: break;
	}

	for (uint32_t i = 0; i < xsysd->static_xdev_count; i++) {
		struct xrt_device *xdev = xsysd->static_xdevs[i];

		if (xdev == NULL || !xdev->supported.hand_tracking ||
		    (ht_serial != NULL && (strncmp(xdev->serial, ht_serial, XRT_DEVICE_NAME_LEN) != 0))) {
			continue;
		}

		for (uint32_t j = 0; j < xdev->input_count; j++) {
			struct xrt_input *input = &xdev->inputs[j];

			if (input->name == name) {
				return xdev;
			}
		}
	}

	return NULL;
}

void
u_system_devices_supported_from_xdevs(struct xrt_device *const *xdevs,
                                      uint32_t xdev_count,
                                      struct xrt_system_devices_supported *out_supported)
{
	assert(xdevs != NULL || xdev_count == 0);
	assert(out_supported != NULL);

	struct xrt_system_devices_supported supported = {0};

	for (uint32_t i = 0; i < xdev_count; i++) {
		const struct xrt_device *xdev = xdevs[i];
		if (xdev == NULL) {
			continue;
		}

		const struct xrt_device_supported *s = &xdev->supported;

		supported.orientation_tracking |= s->orientation_tracking;
		supported.position_tracking |= s->position_tracking;
		supported.hand_tracking |= s->hand_tracking;
		supported.eye_gaze |= s->eye_gaze;
		supported.presence |= s->presence;
		supported.force_feedback |= s->force_feedback;
		supported.face_tracking |= s->face_tracking;
		if (s->body_tracking) {
			supported.body_tracking_fb |= xdev_has_input(xdev, XRT_INPUT_FB_BODY_TRACKING);
			supported.full_body_tracking_meta |= xdev_has_input(xdev, XRT_INPUT_META_FULL_BODY_TRACKING);
			supported.body_tracking_bd |= xdev_has_input(xdev, XRT_INPUT_BD_BODY_TRACKING) ||
			                              xdev_has_input(xdev, XRT_INPUT_BD_BODY_TRACKING_WITHOUT_ARM);
		}
		supported.body_tracking_calibration |= s->body_tracking_calibration;
		supported.body_tracking_fidelity |= s->body_tracking_fidelity;
		supported.planes |= s->planes;
		supported.plane_capability_flags |= s->plane_capability_flags;
	}

	*out_supported = supported;
}

void
u_system_devices_supported_from_static_roles(const struct xrt_system_devices *xsysd,
                                             struct xrt_system_devices_supported *out_supported)
{
	assert(xsysd != NULL);
	assert(out_supported != NULL);

	struct xrt_device *const ht_xdevs[] = {
	    xsysd->static_roles.hand_tracking.unobstructed.left,
	    xsysd->static_roles.hand_tracking.unobstructed.right,
	    xsysd->static_roles.hand_tracking.conforming.left,
	    xsysd->static_roles.hand_tracking.conforming.right,
	};

	struct xrt_system_devices_supported head = {0};
	struct xrt_system_devices_supported eyes = {0};
	struct xrt_system_devices_supported face = {0};
	struct xrt_system_devices_supported body = {0};
	struct xrt_system_devices_supported ht = {0};

	u_system_devices_supported_from_xdevs(&xsysd->static_roles.head, 1, &head);
	u_system_devices_supported_from_xdevs(&xsysd->static_roles.eyes, 1, &eyes);
	u_system_devices_supported_from_xdevs(&xsysd->static_roles.face, 1, &face);
	u_system_devices_supported_from_xdevs(&xsysd->static_roles.body, 1, &body);
	u_system_devices_supported_from_xdevs(ht_xdevs, ARRAY_SIZE(ht_xdevs), &ht);

	*out_supported = (struct xrt_system_devices_supported){
	    .orientation_tracking = head.orientation_tracking,
	    .position_tracking = head.position_tracking,
	    .hand_tracking = ht.hand_tracking,
	    .eye_gaze = eyes.eye_gaze,
	    .presence = head.presence,
	    .force_feedback = ht.force_feedback,
	    .face_tracking = face.face_tracking,
	    .body_tracking_fb = body.body_tracking_fb,
	    .full_body_tracking_meta = body.full_body_tracking_meta,
	    .body_tracking_bd = body.body_tracking_bd,
	    .body_tracking_calibration = body.body_tracking_calibration,
	    .body_tracking_fidelity = body.body_tracking_fidelity,
	    .planes = head.planes,
	    .plane_capability_flags = head.plane_capability_flags,
	};
}
