// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Helpers for @ref xrt_tracking_origin.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup aux_util
 */

#include "util/u_tracking_origin.h"

#include "xrt/xrt_device.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_pretty_print.h"
#include "util/u_tracking_origin_id.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>


static bool
is_missing_id(const struct xrt_device *xdev)
{
	return xdev != NULL && xdev->tracking_origin != NULL && xdev->tracking_origin->id.val == 0;
}


/*
 *
 * 'Exported' functions.
 *
 */

void
u_tracking_origin_init(struct xrt_tracking_origin *xto, enum xrt_tracking_type type, const char *fmt, ...)
{
	U_ZERO(xto);

	xto->id = u_tracking_origin_id_generate();
	xto->type = type;
	xto->initial_offset = (struct xrt_pose)XRT_POSE_IDENTITY;

	va_list args;
	va_start(args, fmt);
	vsnprintf(xto->name, sizeof(xto->name), fmt, args);
	va_end(args);
}

void
u_tracking_origin_check_xdev(const struct xrt_device *xdev)
{
	if (!is_missing_id(xdev)) {
		return;
	}

	U_LOG_E("Tracking origin '%s' of device '%s' has no ID!", xdev->tracking_origin->name, xdev->str);
	assert(false && "Tracking origin has no ID");
}

void
u_tracking_origin_check_xdevs(struct xrt_device *const *xdevs, size_t xdev_count)
{
	if (xdevs == NULL && xdev_count > 0) {
		U_LOG_E("Got a NULL xdevs array with a count of %zu!", xdev_count);
		assert(false && "xdevs is NULL but xdev_count is not zero");
		return;
	}

	bool all_ok = true;
	for (size_t i = 0; i < xdev_count; i++) {
		if (is_missing_id(xdevs[i])) {
			all_ok = false;
			break;
		}
	}

	if (all_ok) {
		return;
	}

	struct u_pp_sink_stack_only sink;
	u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);

	u_pp(dg, "Tracking origins without an ID (device -> tracking origin):");

	for (size_t i = 0; i < xdev_count; i++) {
		struct xrt_device *xdev = xdevs[i];
		if (!is_missing_id(xdev)) {
			continue;
		}

		u_pp(dg, "\n\t'%s' -> '%s'", xdev->str, xdev->tracking_origin->name);
	}

	U_LOG_E("%s", sink.buffer);
	assert(false && "Tracking origins without an ID");
}
