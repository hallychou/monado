// Copyright 2023, Shawn Wallace
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief OpenVR IVRSettings interface implementation.
 * @author Shawn Wallace <yungwallace@live.com>
 * @ingroup drv_steamvr_lh
 */

#include <openvr_driver.h>
#include <optional>
#include <cstring>
#include <map>
#include <string_view>

#include "settings.hpp"
#include "context.hpp"
#include "util/u_json.hpp"

using namespace std::string_view_literals;

// Default to 100% brightness.
DEBUG_GET_ONCE_FLOAT_OPTION(lh_default_brightness, "LH_DEFAULT_BRIGHTNESS", 1.0)
DEBUG_GET_ONCE_BOOL_OPTION(lh_standby_on_exit, "LH_STANDBY_ON_EXIT", false)

using xrt::auxiliary::util::json::JSONNode;

Settings::Settings(const std::string &driver_path, const std::string &user_settings_path, Context *context)
    : steamvr_settings(JSONNode::loadFromFile(user_settings_path)),
      driver_defaults(
          JSONNode::loadFromFile(driver_path + "/resources/settings/default.vrsettings")),
      context{context}
{
	analog_gain = debug_get_float_option_lh_default_brightness();
}

std::optional<JSONNode> Settings::get_settings_value(std::string section, std::string key)
{
	auto get_setting = [section, key](const JSONNode &root) -> std::optional<JSONNode> {
		if(!root.isValid() || !root.isObject())
			return std::nullopt;
		
		if (root.hasKey(section) == false)
			return std::nullopt;

		JSONNode section_node = root[section];

		if (!section_node.isObject() || section_node.hasKey(key) == false)
			return std::nullopt;

		return std::optional(section_node[key]);
	};

	std::optional value = get_setting(steamvr_settings);
	if (!value.has_value())
		value = get_setting(driver_defaults);

	return value;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
const char *
Settings::GetSettingsErrorNameFromEnum(vr::EVRSettingsError eError)
{
	return nullptr;
}

void
Settings::SetBool(const char *pchSection, const char *pchSettingsKey, bool bValue, vr::EVRSettingsError *peError)
{}

void
Settings::SetInt32(const char *pchSection, const char *pchSettingsKey, int32_t nValue, vr::EVRSettingsError *peError)
{}

void
Settings::SetFloat(const char *pchSection, const char *pchSettingsKey, float flValue, vr::EVRSettingsError *peError)
{
	if (pchSection == std::string_view{vr::k_pch_SteamVR_Section} && pchSettingsKey == "analogGain"sv) {
		analog_gain = flValue;
		context->add_vendor_event(vr::VREvent_SteamVRSectionSettingChanged);
	}
}

void
Settings::SetString(const char *pchSection,
                    const char *pchSettingsKey,
                    const char *pchValue,
                    vr::EVRSettingsError *peError)
{}

bool
Settings::GetBool(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError)
{
	if (peError)
		*peError = vr::VRSettingsError_None;
		
	U_LOG_IFL_D(context->log_level, "Getting bool setting: %s.%s", pchSection, pchSettingsKey);

	if (pchSection == std::string_view{vr::k_pch_Power_Section} && pchSettingsKey == "powerOffOnExit"sv)
		return debug_get_bool_option_lh_standby_on_exit();

	std::optional value = get_settings_value(pchSection, pchSettingsKey);

	if (!value.has_value() || !value->isBool()) {
		if (peError)
			*peError = vr::VRSettingsError_UnsetSettingHasNoDefault;
		return false;
	}

	return value.value().asBool();
}

int32_t
Settings::GetInt32(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError)
{
	if (peError)
		*peError = vr::VRSettingsError_None;
		
	U_LOG_IFL_D(context->log_level, "Getting int32 setting: %s.%s", pchSection, pchSettingsKey);

	std::optional value = get_settings_value(pchSection, pchSettingsKey);

	if (!value.has_value() || !value->isInt()) {
		if (peError)
			*peError = vr::VRSettingsError_UnsetSettingHasNoDefault;
		return 0;
	}

	return value.value().asInt();
}

float
Settings::GetFloat(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError)
{
	if (peError)
		*peError = vr::VRSettingsError_None;

	U_LOG_IFL_D(context->log_level, "Getting float setting: %s.%s", pchSection, pchSettingsKey);

	if (!strcmp(pchSection, vr::k_pch_SteamVR_Section)) {
		if (!strcmp(pchSettingsKey, "analogGain")) {
			return analog_gain;
		}
		if (!strcmp(pchSettingsKey, "ipd")) {
			// Inform the SteamVR driver we have 0 ipd (in case) it factors this into the eye matrix.
			return 0.f;
		}
	}

	std::optional value = get_settings_value(pchSection, pchSettingsKey);

	if (!value.has_value() || !value->isDouble()) {
		if (peError)
			*peError = vr::VRSettingsError_UnsetSettingHasNoDefault;
		return 0.f;
	}

	return value.value().asDouble();
}

// Driver requires a few string settings to initialize properly
void
Settings::GetString(const char *pchSection,
                    const char *pchSettingsKey,
                    char *pchValue,
                    uint32_t unValueLen,
                    vr::EVRSettingsError *peError)
{
	if (peError)
		*peError = vr::VRSettingsError_None;

	U_LOG_IFL_D(context->log_level, "Getting string setting: %s.%s", pchSection, pchSettingsKey);

	std::optional value = get_settings_value(pchSection, pchSettingsKey);

	if (!value.has_value() || !value->isString()) {
		if (peError)
			*peError = vr::VRSettingsError_UnsetSettingHasNoDefault;
		return;
	}
	
	auto string_value = value->asString();
	if (unValueLen > string_value.size()) {
		std::strncpy(pchValue, string_value.c_str(), string_value.size() + 1);
	}
}

void
Settings::RemoveSection(const char *pchSection, vr::EVRSettingsError *peError)
{}

void
Settings::RemoveKeyInSection(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError)
{}
// NOLINTEND(bugprone-easily-swappable-parameters)
