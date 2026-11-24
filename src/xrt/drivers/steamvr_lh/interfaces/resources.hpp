// Copyright 2023, Shawn Wallace
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief OpenVR IVRResources interface header.
 * @author Shawn Wallace <yungwallace@live.com>
 * @ingroup drv_steamvr_lh
 */

#pragma once

#include "openvr_driver.h"
#include "util/u_logging.h"
#include <map>
#include <filesystem>

class Resources : public vr::IVRResources
{
	const u_logging_level log_level;
	const std::string steamvr_install;
	const std::map<std::string, std::filesystem::path> &driver_name_to_directory;

public:
	Resources(u_logging_level l, const std::string &steamvr_install, const std::map<std::string, std::filesystem::path> &driver_name_to_directory)
	    : log_level(l), steamvr_install(steamvr_install), driver_name_to_directory(driver_name_to_directory){};
	// ------------------------------------
	// Shared Resource Methods
	// ------------------------------------

	/** Loads the specified resource into the provided buffer if large enough.
	 * Returns the size in bytes of the buffer required to hold the specified resource. */
	uint32_t
	LoadSharedResource(const char *pchResourceName, char *pchBuffer, uint32_t unBufferLen) override;

	/** Provides the full path to the specified resource. Resource names can include named directories for
	 * drivers and other things, and this resolves all of those and returns the actual physical path.
	 * pchResourceTypeDirectory is the subdirectory of resources to look in. */
	uint32_t
	GetResourceFullPath(const char *pchResourceName,
	                    const char *pchResourceTypeDirectory,
	                    VR_OUT_STRING() char *pchPathBuffer,
	                    uint32_t unBufferLen) override;
};
