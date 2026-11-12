// Copyright 2026, UltraDev.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "xrt/xrt_config_drivers.h"

#include "target_builder_helpers.h"
#include "multi_wrapper/multi.h"

#ifdef XRT_MODULE_CONSTELLATION_TRACKING
#include "tracking/t_constellation.h"
#include "constellation/t_constellation_tracker.h"
#include "constellation/t_rift_blobwatch.h"
#endif

#ifdef XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE
#include "steamvr_lh/steamvr_lh_interface.h"
#include "vive/vive_common.h"
#endif
