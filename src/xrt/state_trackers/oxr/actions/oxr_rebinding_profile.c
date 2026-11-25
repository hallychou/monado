// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Runtime OpenXR-level rebinding profile data.
 * @ingroup oxr_main
 */

#include "oxr_rebinding_profile.h"


bool
oxr_rebinding_profile_find_target(const struct oxr_rebinding_profile *profile,
                                  XrPath source_path,
                                  const XrPath **out_target_paths,
                                  size_t *out_target_count)
{
	if (profile == NULL) {
		*out_target_paths = NULL;
		*out_target_count = 0;
		return false;
	}

	for (size_t i = 0; i < profile->binding_count; i++) {
		if (profile->bindings[i].source_path == source_path) {
			*out_target_paths = profile->bindings[i].target_paths;
			*out_target_count = profile->bindings[i].target_count;
			return true;
		}
	}

	*out_target_paths = NULL;
	*out_target_count = 0;
	return false;
}
