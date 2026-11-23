// Copyright 2023, Shawn Wallace
// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief SteamVR driver context implementation and entrypoint.
 * @author Shawn Wallace <yungwallace@live.com>
 * @ingroup drv_steamvr_lh
 */

#include "xrt/xrt_config_arch.h"
#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_system.h"

#include "interfaces/context.hpp"

#include "os/os_time.h"
#include "os/os_loader.h"

#include "util/u_var.h"
#include "util/u_device.h"
#include "util/u_time.h"
#include "util/u_json.hpp"

#include "math/m_api.h"

#ifdef XRT_BUILD_DRIVER_PIMAX_P2
#include "pimax_p2/pimax_p2.h"
#endif

#include "openvr_driver.h"

#include "vdf_parser.hpp"

#include "steamvr_lh_interface.h"
#include "device.hpp"

#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <cmath>
#include <unordered_map>
#include <string_view>
#include <filesystem>
#include <istream>
#include <algorithm>

using xrt::auxiliary::util::json::JSONNode;

namespace {

// based on the logic at
// <https://github.com/ValveSoftware/openvr/blob/ae46a8dd0172580648c8922658a100439115d3eb/src/vrcore/pathtools_public.h#L143-L153>.
#if defined(XRT_OS_WINDOWS)
#error "steamvr_lh does not yet support windows!"
#define OVR_PLAT_EXT ".dll"

#if defined(XRT_ARCH_X86_64)
#define OVR_PLAT_SUBDIR "win64"
#elif defined(XRT_ARCH_X86)
#define OVR_PLAT_SUBDIR "win32"
#else
#error "steamvr_lh: Unknown Windows Architecture"
#endif

#elif defined(XRT_OS_OSX)
#define OVR_PLAT_EXT ".dylib"
// contains all architectures in a universal binary
#define OVR_PLAT_SUBDIR "osx32"

#elif defined(XRT_OS_LINUX)
#define OVR_PLAT_EXT ".so"

#if defined(XRT_ARCH_X86)
#define OVR_PLAT_SUBDIR "linux32"
#elif defined(XRT_OS_ANDROID) && defined(XRT_ARCH_ARM)
#define OVR_PLAT_SUBDIR "androidarm32"
#elif defined(XRT_OS_ANDROID) && defined(XRT_ARCH_ARM64)
#define OVR_PLAT_SUBDIR "androidarm64"
#elif defined(XRT_ARCH_ARM64)
#define OVR_PLAT_SUBDIR "linuxarm64"
#elif defined(XRT_ARCH_X86_64)
#define OVR_PLAT_SUBDIR "linux64"
// risc-v 64-bit support is not official - added so it can build
#elif defined(XRT_ARCH_RISCV64)
#define OVR_PLAT_SUBDIR "linuxrv64"
#else
#error "steamvr_lh: Unknown Linux Architecture"
#endif

#else
#error "steamvr_lh: Unknown OS"
#endif

DEBUG_GET_ONCE_LOG_OPTION(lh_log, "LIGHTHOUSE_LOG", U_LOGGING_INFO)
DEBUG_GET_ONCE_NUM_OPTION(lh_discover_wait_ms, "LH_DISCOVER_WAIT_MS", 3000)
DEBUG_GET_ONCE_FLOAT_OPTION(lh_stick_deadzone, "LH_STICK_DEADZONE", 0)

static constexpr size_t MAX_CONTROLLERS = 16;


// ~/.steam/root is a symlink to where the Steam root is
const std::string STEAM_INSTALL_DIR = std::string(getenv("HOME")) + "/.steam/root";
constexpr auto STEAMVR_APPID = "250820";

// Parse libraryfolder.vdf to find where SteamVR is installed
std::string
find_steamvr_install()
{
	using namespace tyti;
	u_logging_level level = debug_get_log_option_lh_log();
	std::ifstream file(STEAM_INSTALL_DIR + "/steamapps/libraryfolders.vdf");
	if (!file.is_open()) {
		U_LOG_IFL_E(level, "Failed to open libraryfolders.vdf");
		return std::string();
	}

	vdf::basic_object<char> root;
	try {
		root = vdf::read(file);
	} catch (std::exception &ex) {
		U_LOG_IFL_E(level, "Failed to read libraryfolders.vdf: %s", ex.what());
		return std::string();
	}

	assert(root.name == "libraryfolders");
	for (auto &[_, child] : root.childs) {
		U_LOG_D("Found library folder %s", child->attribs["path"].c_str());
		std::shared_ptr<vdf::object> apps = child->childs["apps"];
		for (auto &[appid, _] : apps->attribs) {
			if (appid == STEAMVR_APPID) {
				std::string path = child->attribs["path"] + "/steamapps/common/SteamVR";
				if (std::filesystem::exists(path)) {
					return path;
				}
			}
		}
	}
	return std::string();
}

} // namespace

#define CTX_ERR(...) U_LOG_IFL_E(log_level, __VA_ARGS__)
#define CTX_WARN(...) U_LOG_IFL_W(log_level, __VA_ARGS__)
#define CTX_INFO(...) U_LOG_IFL_I(log_level, __VA_ARGS__)
#define CTX_TRACE(...) U_LOG_IFL_T(log_level, __VA_ARGS__)
#define CTX_DEBUG(...) U_LOG_IFL_D(log_level, __VA_ARGS__)

/**
 * Since only the devices will live after our get_devices function is called, we make our Context
 * a shared ptr that is owned by the devices that exist, so that it is also cleaned up by the
 * devices that exist when they are all destroyed.
 */
std::shared_ptr<Context>
Context::create(const std::string &driver_path,
                const std::string &driver_name,
                const std::string &user_settings_path,
				const std::string &chaperone_info_path,
				const std::string &steamvr_install,
				const std::map<std::string, std::filesystem::path> &driver_name_to_directory,
                vr::IServerTrackedDeviceProvider * provider,
                xrt_system_devices * xsysd)
{
	// xrt_tracking_origin initialization
	std::shared_ptr<Context> c =
	    std::make_shared<Context>(driver_path, driver_name, user_settings_path, chaperone_info_path, steamvr_install, driver_name_to_directory, xsysd, debug_get_log_option_lh_log());
	c->provider = provider;
	std::string tracking_name = "openvr " + driver_name + " driver tracking origin";
	std::strncpy(c->name, tracking_name.c_str(), XRT_TRACKING_NAME_LEN);
	c->type = XRT_TRACKING_TYPE_LIGHTHOUSE;
	c->initial_offset = XRT_POSE_IDENTITY;
	vr::EVRInitError err = provider->Init(c.get());
	if (err != vr::VRInitError_None) {
		U_LOG_IFL_E(c->log_level, "OpenVR driver initialization failed: error %u", err);
		return nullptr;
	}
	c->frame_thread = std::thread([ctx = c.get()] {
		while (ctx->frame_thread_run.load()) {
			using namespace std::chrono_literals;
			// SteamVR calls `RunFrame()` approximately every 10.1ms
			const std::chrono::time_point<std::chrono::steady_clock> next =
			    std::chrono::steady_clock::now() + 10ms;
			ctx->provider->RunFrame();
			ctx->frame_thread_event.try_acquire_until(next);
		}
	});
	return c;
}

Context::Context(const std::string &driver_path, const std::string &driver_name, const std::string &user_settings_path, const std::string &chaperone_info_path, const std::string &steamvr_install, const std::map<std::string, std::filesystem::path> &driver_name_to_directory, xrt_system_devices * xsysd, u_logging_level level)
    : settings(driver_path, user_settings_path, this), resources(level, steamvr_install, driver_name_to_directory), log_level(level),
      frame_thread_run(true), frame_thread(), driver_name(driver_name), chaperone_info_path(chaperone_info_path), xsysd(xsysd)
{}

Context::~Context()
{
	this->frame_thread_run.store(false);
	this->frame_thread_event.release();
	if (this->frame_thread.joinable())
		this->frame_thread.join();
	U_LOG_IFL_I(log_level, "Shutting down driver %s", driver_name.c_str());
	if (this->provider) {
		try {
			this->provider->Cleanup();
		} catch (const std::exception &e) {
			U_LOG_IFL_E(log_level, "Exception during Cleanup: %s", e.what());
		}
	}
}

/***** IVRDriverContext methods *****/

void *
Context::GetGenericInterface(const char *pchInterfaceVersion, vr::EVRInitError *peError)
{
	CTX_DEBUG("Driver %s Requested interface %s", this->driver_name.c_str(), pchInterfaceVersion);
#define MATCH_INTERFACE(version, interface)                                                                            \
	if (std::strcmp(pchInterfaceVersion, version) == 0) {                                                          \
		return interface;                                                                                      \
	}
#define MATCH_INTERFACE_THIS(interface) MATCH_INTERFACE(interface##_Version, static_cast<interface *>(this))

	// Known interfaces
	MATCH_INTERFACE_THIS(vr::IVRServerDriverHost);
	MATCH_INTERFACE_THIS(vr::IVRDriverInput);
	// This interface is not in a public header yet, but just passing IVRDriverInput_003 seems to work.
	MATCH_INTERFACE("IVRDriverInput_003", static_cast<vr::IVRDriverInput *>(this));
	MATCH_INTERFACE("IVRDriverInput_004", static_cast<vr::IVRDriverInput *>(this));
	MATCH_INTERFACE("IVRDriverInput_005", static_cast<vr::IVRDriverInput *>(this));
	MATCH_INTERFACE_THIS(vr::IVRProperties);
	MATCH_INTERFACE_THIS(vr::IVRDriverLog);
	MATCH_INTERFACE(vr::IVRSettings_Version, &settings);
	MATCH_INTERFACE(vr::IVRResources_Version, &resources);
	MATCH_INTERFACE(vr::IVRIOBuffer_Version, &iobuf);
	MATCH_INTERFACE(vr::IVRDriverManager_Version, &man);
	MATCH_INTERFACE(vr::IVRBlockQueue_Version, &blockqueue);
	MATCH_INTERFACE(vr::IVRPaths_Version, &paths);
	// This version of the interface is not in a public header.
	// Luckily it seems to be compatible with the previous version.
	MATCH_INTERFACE("IVRPaths_002", &paths);

	// Internal interfaces
	MATCH_INTERFACE("IVRServer_XXX", &server);
	MATCH_INTERFACE("IVRServerInternal_XXX", &server);
	MATCH_INTERFACE("IVRChaperoneServerDriver_XXX", &server);
	return nullptr;
}

vr::DriverHandle_t
Context::GetDriverHandle()
{
	return 1;
}


/***** IVRServerDriverHost methods *****/

bool
Context::setup_hmd(const char *serial, vr::ITrackedDeviceServerDriver *driver)
{
	auto *hmd = new HmdDevice(DeviceBuilder{this->shared_from_this(), driver, serial, this->chaperone_info_path});

	{
		// Publish the device before activating it: the driver writes its properties and creates its
		// input components from within Activate, and those callbacks have to be able to find it.
		std::lock_guard lk(this->devices_mut);
		this->hmd = hmd;
		devices[0] = hmd;
		registered_devices.insert(hmd);
		internal_devices.insert(hmd);
	}

#define VERIFY(expr, msg)                                                                                              \
	if (!(expr)) {                                                                                                 \
		CTX_ERR("Activating HMD failed: %s", msg);                                                             \
		std::lock_guard lk(this->devices_mut);                                                                 \
		if (this->hmd == hmd) {                                                                                \
			this->hmd = nullptr;                                                                           \
		}                                                                                                      \
		delete hmd;                                                                                            \
		return false;                                                                                          \
	}
	// Never call into the driver with `devices_mut` held: it takes its own locks here, and calls back into
	// us (WritePropertyBatch, TrackedDevicePoseUpdated, ...) from its own threads while holding them.
	vr::EVRInitError err = driver->Activate(0);
	VERIFY(err == vr::VRInitError_None, std::to_string(err).c_str());

	auto *display = static_cast<vr::IVRDisplayComponent *>(driver->GetComponent(vr::IVRDisplayComponent_Version));
	VERIFY(display, "IVRDisplayComponent is null");
#undef VERIFY

	auto hmd_parts = std::make_unique<HmdDevice::Parts>();
	hmd_parts->base.view_count = 2;
	for (size_t idx = 0; idx < 2; ++idx) {
		vr::EVREye eye = (idx == 0) ? vr::Eye_Left : vr::Eye_Right;
		xrt_view &view = hmd_parts->base.views[idx];

		display->GetEyeOutputViewport(eye, &view.viewport.x_pixels, &view.viewport.y_pixels,
		                              &view.viewport.w_pixels, &view.viewport.h_pixels);

		view.display.w_pixels = view.viewport.w_pixels;
		view.display.h_pixels = view.viewport.h_pixels;
		view.rot = u_device_rotation_ident;
	}

	hmd_parts->base.screens[0].w_pixels =
	    hmd_parts->base.views[0].display.w_pixels + hmd_parts->base.views[1].display.w_pixels;
	hmd_parts->base.screens[0].h_pixels = hmd_parts->base.views[0].display.h_pixels;
	// nominal frame interval will be set when lighthouse gives us the display frequency
	// see HmdDevice::handle_property_write

	hmd_parts->base.blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	hmd_parts->base.blend_mode_count = 1;

	auto &distortion = hmd_parts->base.distortion;
	distortion.models = XRT_DISTORTION_MODEL_COMPUTE;
	distortion.preferred = XRT_DISTORTION_MODEL_COMPUTE;
	for (size_t idx = 0; idx < 2; ++idx) {
		xrt_fov &fov = distortion.fov[idx];
		float tan_left, tan_right, tan_top, tan_bottom;
		display->GetProjectionRaw((vr::EVREye)idx, &tan_left, &tan_right, &tan_top, &tan_bottom);
		fov.angle_left = atanf(tan_left);
		fov.angle_right = atanf(tan_right);
		fov.angle_up = atanf(tan_bottom);
		fov.angle_down = atanf(tan_top);
	}

	u_var_add_root(hmd, "SteamVR HMD Device", true);
	u_var_add_f32(hmd, &hmd->lh_ipd, "Lighthouse IPD");

	u_var_add_f32(hmd, &distortion.fov[0].angle_up, "View 0 FovAngleUp");
	u_var_add_f32(hmd, &distortion.fov[0].angle_down, "View 0 FovAngleDown");
	u_var_add_f32(hmd, &distortion.fov[0].angle_left, "View 0 FovAngleLeft");
	u_var_add_f32(hmd, &distortion.fov[0].angle_right, "View 0 FovAngleRight");

	u_var_add_f32(hmd, &distortion.fov[1].angle_up, "View 1 FovAngleUp");
	u_var_add_f32(hmd, &distortion.fov[1].angle_down, "View 1 FovAngleDown");
	u_var_add_f32(hmd, &distortion.fov[1].angle_left, "View 1 FovAngleLeft");
	u_var_add_f32(hmd, &distortion.fov[1].angle_right, "View 1 FovAngleRight");

	hmd_parts->display = display;
	hmd->set_hmd_parts(std::move(hmd_parts));

	return true;
}

bool
Context::setup_controller(const char *serial, vr::ITrackedDeviceServerDriver *driver)
{
	size_t controller_idx = 0;
	size_t device_idx = 0;
	ControllerDevice *device = nullptr;

	{
		std::lock_guard lk(this->devices_mut);

		// Find the first available slot for a new controller
		for (; controller_idx < MAX_CONTROLLERS; ++controller_idx) {
			if (!controller[controller_idx])
				break;
		}

		// Check if we've exceeded the maximum number of controllers
		if (controller_idx == MAX_CONTROLLERS) {
			CTX_WARN("Attempted to activate more than %zu controllers - this is unsupported",
			         MAX_CONTROLLERS);
			return false;
		}

		// Create the new controller and claim its slot before activating it: the driver writes its
		// properties and creates its input components from within Activate, and those callbacks have to
		// be able to find it.
		get_devices(); // Ensure devices vector is up to date
		device_idx = devices.size();
		CTX_DEBUG("Creating controller device %s at index %zu (controller slot %zu)", serial, device_idx, controller_idx);
		device = new ControllerDevice(
		    device_idx, DeviceBuilder{this->shared_from_this(), driver, serial, this->chaperone_info_path});
		controller[controller_idx] = device;
		devices.push_back(device);
		registered_devices.insert(device);
		internal_devices.insert(device);
	}

	// Never call into the driver with `devices_mut` held: it takes its own locks here, and calls back into
	// us (WritePropertyBatch, TrackedDevicePoseUpdated, ...) from its own threads while holding them.
	vr::EVRInitError err = driver->Activate(device_idx);
	if (err != vr::VRInitError_None) {
		CTX_ERR("Activating controller failed: error %u", err);
		return false;
	}

	return true;
}

void
Context::wait_for_discover()
{
	std::unique_lock lk(this->devices_mut);

	this->discover_end_time =
	    std::chrono::steady_clock::now() + std::chrono::milliseconds(debug_get_num_option_lh_discover_wait_ms());

	while (true) {
		if (std::chrono::steady_clock::now() < this->discover_end_time) {
			this->discover_cv.wait_until(lk, this->discover_end_time);
			continue;
		}

		// The window is not over while a device we already know about is still being activated.
		if (this->devices_in_setup == 0)
			break;

		this->discover_cv.wait(lk, [this] { return this->devices_in_setup == 0; });
	}
}

void
Context::extend_discover()
{
	// Only ever push the deadline out - a device showing up must not cut `LH_DISCOVER_WAIT_MS` short.
	this->discover_end_time =
	    std::max(this->discover_end_time, std::chrono::steady_clock::now() + std::chrono::milliseconds(2000));
	this->discover_cv.notify_all();
}



// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool
Context::TrackedDeviceAdded(const char *pchDeviceSerialNumber,
                            vr::ETrackedDeviceClass eDeviceClass,
                            vr::ITrackedDeviceServerDriver *pDriver)
{
	{
		std::lock_guard lk(this->devices_mut);

		if (!this->in_setup) {
			// devices appearing after setup are not added to xrt_system_devices and would be leaked on
			// exit
			CTX_WARN("Cannot add device after setup; consider increasing LH_DISCOVER_WAIT_MS");
			return false;
		}

		this->extend_discover();

		// The setup below runs without `devices_mut` held, and publishes the device before the driver has
		// finished activating it - keep discovery from ending in the middle of that.
		this->devices_in_setup++;
	}

	bool added = false;

	CTX_INFO("New device added: %s", pchDeviceSerialNumber);
	switch (eDeviceClass) {
	case vr::TrackedDeviceClass_HMD: {
		CTX_INFO("Found lighthouse HMD: %s", pchDeviceSerialNumber);
		added = setup_hmd(pchDeviceSerialNumber, pDriver);
		break;
	}
	case vr::TrackedDeviceClass_Controller: {
		CTX_INFO("Found lighthouse controller: %s", pchDeviceSerialNumber);
		added = setup_controller(pchDeviceSerialNumber, pDriver);
		break;
	}
	case vr::TrackedDeviceClass_TrackingReference: {
		CTX_INFO("Found lighthouse base station: %s", pchDeviceSerialNumber);
		break;
	}
	case vr::TrackedDeviceClass_GenericTracker: {
		CTX_INFO("Found lighthouse tracker: %s", pchDeviceSerialNumber);
		added = setup_controller(pchDeviceSerialNumber, pDriver);
		break;
	}
	default: {
		CTX_WARN("Attempted to add unsupported device class: %u", eDeviceClass);
		break;
	}
	}

	{
		std::lock_guard lk(this->devices_mut);
		this->devices_in_setup--;
	}
	this->discover_cv.notify_all();

	return added;
}

void
Context::TrackedDevicePoseUpdated(uint32_t unWhichDevice, const vr::DriverPose_t &newPose, uint32_t unPoseStructSize)
{
	assert(sizeof(newPose) == unPoseStructSize);

	std::lock_guard lk(this->devices_mut);

	Device *device = static_cast<Device *>(get_devices()[unWhichDevice]);
	if (!device || this->internal_devices.count(device) == 0) {
		CTX_WARN("TrackedDevicePoseUpdated called for unknown device index %u", unWhichDevice);
		return;
	}

	device->update_pose(newPose);
}

void
Context::VsyncEvent(double vsyncTimeOffsetSeconds)
{}

void
Context::add_event_locked(vr::VREvent_t event)
{
	this->events.emplace_back(std::chrono::steady_clock::now(), event);
	this->frame_thread_event.try_acquire();
	this->frame_thread_event.release();
}

void
Context::VendorSpecificEvent(uint32_t unWhichDevice,
                             vr::EVREventType eventType,
                             const vr::VREvent_Data_t &eventData,
                             double eventTimeOffset)
{
	std::lock_guard lk(event_queue_mut);
	this->add_event_locked({
	    .eventType = static_cast<uint32_t>(eventType),
	    .trackedDeviceIndex = unWhichDevice,
	    .eventAgeSeconds = {},
	    .data = eventData,
	});
}

bool
Context::IsExiting()
{
	return false;
}

size_t
Context::add_haptic_event(vr::VREvent_HapticVibration_t event, const size_t old_event_handle)
{
	vr::VREvent_t e;
	e.eventType = vr::EVREventType::VREvent_Input_HapticVibration;
	e.trackedDeviceIndex = event.containerHandle - 1;
	vr::VREvent_Data_t d;
	d.hapticVibration = event;
	e.data = d;

	std::unique_lock lk(event_queue_mut);
	while (events.size() >= 120) { // avoid unbounded allocation if misbehaving apps send too many events at once
		event_popped.wait(lk);
	}
	const size_t old_event_index = old_event_handle - events_tail;
	if (old_event_index < size_t(events.size())) {
		vr::VREvent_t *const old_event = &events[old_event_index].inner;
		if (old_event->eventType == e.eventType &&
		    old_event->data.hapticVibration.containerHandle == e.data.hapticVibration.containerHandle &&
		    old_event->data.hapticVibration.componentHandle == e.data.hapticVibration.componentHandle) {
			old_event->eventType = vr::EVREventType::VREvent_None;
		}
	}
	this->add_event_locked(e);
	return events_tail + events.size() - 1;
}

bool
Context::PollNextEvent(vr::VREvent_t *pEvent, uint32_t uncbVREvent)
{
	std::lock_guard lk(event_queue_mut);
	while (!events.empty()) {
		assert(sizeof(vr::VREvent_t) == uncbVREvent);
		Event e = events.front();
		events.pop_front();
		++events_tail;
		if (e.inner.eventType == vr::EVREventType::VREvent_None) {
			continue;
		}
		event_popped.notify_all();
		*pEvent = e.inner;
		using float_sec = std::chrono::duration<float>;
		float_sec event_age = std::chrono::steady_clock::now() - e.insert_time;
		pEvent->eventAgeSeconds = event_age.count();
		return true;
	}
	return false;
}

void
Context::GetRawTrackedDevicePoses(float fPredictedSecondsFromNow,
                                  vr::TrackedDevicePose_t *pTrackedDevicePoseArray,
                                  uint32_t unTrackedDevicePoseArrayCount)
{

	const int64_t time = os_monotonic_get_ns();

	// set pose helper function
	auto set_pose = [&](int i, enum xrt_input_name name, xrt_device *device) {
		xrt_space_relation space = {};
		if(xrt_device_get_tracked_pose(device, name, time, &space) != XRT_SUCCESS) {
			U_LOG_IFL_W(log_level, "Failed to get pose for device %d %s (%s)", i, device->str, device->serial);
			pTrackedDevicePoseArray[i].eTrackingResult = vr::TrackingResult_Uninitialized;
		}
		if(space.relation_flags & (XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)) {
			xrt_matrix_3x3 rot = {};
			math_matrix_3x3_from_quat(&space.pose.orientation, &rot);
			pTrackedDevicePoseArray[i].mDeviceToAbsoluteTracking = {{ // openvr uses a right-handed system +y is up +x is to the right -z is forward cordinates
				{ rot.v[0],  rot.v[3], -rot.v[6],  space.pose.position.x},
				{ rot.v[1],  rot.v[4], -rot.v[7],  space.pose.position.y},
				{-rot.v[2], -rot.v[5],  rot.v[8], -space.pose.position.z},
			}};
			if(space.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) {
				pTrackedDevicePoseArray[i].eTrackingResult = vr::TrackingResult_Running_OK;
			} else {
				pTrackedDevicePoseArray[i].eTrackingResult = vr::TrackingResult_Fallback_RotationOnly;
			}
			pTrackedDevicePoseArray[i].bPoseIsValid = true;
		} else {
			pTrackedDevicePoseArray[i].eTrackingResult = vr::TrackingResult_Running_OutOfRange;
			pTrackedDevicePoseArray[i].bPoseIsValid = false;
		}
		if(space.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) {
			pTrackedDevicePoseArray[i].vVelocity = {space.linear_velocity.x, space.linear_velocity.y, space.linear_velocity.z};
		}
		if(space.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) {
			pTrackedDevicePoseArray[i].vAngularVelocity = {space.angular_velocity.x, space.angular_velocity.y, space.angular_velocity.z};
		}
		pTrackedDevicePoseArray[i].bDeviceIsConnected = true;

		// U_LOG_IFL_T(log_level, "Pose for device %d %s (%s): pos=(%.2f, %.2f, %.2f) rot=(%.2f, %.2f, %.2f, %.2f)", i, device->str, device->serial,
		//            space.pose.position.x, space.pose.position.y, space.pose.position.z,
		//            space.pose.orientation.x, space.pose.orientation.y, space.pose.orientation.z,
		//            space.pose.orientation.w);
	};

	std::vector<xrt_device *> devices = get_devices();

	for (size_t i = 0; i < devices.size() && i < unTrackedDevicePoseArrayCount; ++i) {
		if(devices[i] == nullptr) {
			pTrackedDevicePoseArray[i].eTrackingResult = vr::TrackingResult_Uninitialized;
			pTrackedDevicePoseArray[i].bPoseIsValid = false;
			pTrackedDevicePoseArray[i].bDeviceIsConnected = false;
			U_LOG_IFL_W(log_level, "Device at index %zu is null", i);
			continue;
		}
		if(devices[i]->device_type == XRT_DEVICE_TYPE_HMD) {
			// CTX_DEBUG("Setting pose for device %d %s (%s) as HMD", i, devices[i]->str, devices[i]->serial);
			set_pose(i, XRT_INPUT_GENERIC_HEAD_POSE, devices[i]);
		} else if(devices[i]->device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER ||
			devices[i]->device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER || 
			devices[i]->device_type == XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER) {
			// CTX_DEBUG("Setting pose for device %d %s (%s) as controller", i, devices[i]->str, devices[i]->serial);
			set_pose(i, XRT_INPUT_GENERIC_PALM_POSE, devices[i]);
		} else if(devices[i]->device_type == XRT_DEVICE_TYPE_UNKNOWN) {
			// CTX_DEBUG("Setting pose for device %d %s (%s) as local space pose", i, devices[i]->str, devices[i]->serial);
			set_pose(i, XRT_INPUT_GENERIC_LOCAL_SPACE_POSE, devices[i]);
		} else if(devices[i]->device_type == XRT_DEVICE_TYPE_GENERIC_TRACKER) {
			// CTX_DEBUG("Setting pose for device %d %s (%s) as generic tracker", i, devices[i]->str, devices[i]->serial);
			set_pose(i, XRT_INPUT_GENERIC_TRACKER_POSE, devices[i]);
		} else {
			// CTX_WARN("Device at index %zu has unknown device type %d, skipping pose update", i, devices[i]->device_type);
			continue;
		}
	}
}

void
Context::RequestRestart(const char *pchLocalizedReason,
                        const char *pchExecutableToStart,
                        const char *pchArguments,
                        const char *pchWorkingDirectory)
{}

uint32_t
Context::GetFrameTimings(vr::Compositor_FrameTiming *pTiming, uint32_t nFrames)
{
	return 0;
}

void
Context::SetDisplayEyeToHead(uint32_t unWhichDevice,
                             const vr::HmdMatrix34_t &eyeToHeadLeft,
                             const vr::HmdMatrix34_t &eyeToHeadRight)
{
	hmd->SetDisplayEyeToHead(unWhichDevice, eyeToHeadLeft, eyeToHeadRight);
}

void
Context::SetDisplayProjectionRaw(uint32_t unWhichDevice, const vr::HmdRect2_t &eyeLeft, const vr::HmdRect2_t &eyeRight)
{}

void
Context::SetRecommendedRenderTargetSize(uint32_t unWhichDevice, uint32_t nWidth, uint32_t nHeight)
{}

/***** IVRDriverInput methods *****/


vr::EVRInputError
Context::create_component_common(vr::PropertyContainerHandle_t container,
                                 const char *name,
                                 vr::VRInputComponentHandle_t *pHandle)
{
	*pHandle = vr::k_ulInvalidInputComponentHandle;

	std::lock_guard lk(this->devices_mut);
	Device *device = static_cast<Device *>(prop_container_to_device(container));
	if (!device || this->internal_devices.count(device) == 0) {
		return vr::VRInputError_InvalidHandle;
	}

	if (xrt_input *input = device->get_input_from_name(name); input) {
		CTX_DEBUG("creating component %s for %p", name, (void *)device);
		vr::VRInputComponentHandle_t handle = device->new_input_handle_locked();
		this->input.handle_to_input[handle] = input;
		*pHandle = handle;
	}

	return vr::VRInputError_None;
}

xrt_input *
Context::update_component_common_locked(vr::VRInputComponentHandle_t handle, double offset)
{
	xrt_input *input{nullptr};

	if (handle != vr::k_ulInvalidInputComponentHandle) {
		input = this->input.handle_to_input[handle];
		input->active = true;
		input->timestamp = os_monotonic_get_ns() + time_s_to_ns(offset);
	}

	return input;
}

vr::EVRInputError
Context::CreateBooleanComponent(vr::PropertyContainerHandle_t ulContainer,
                                const char *pchName,
                                vr::VRInputComponentHandle_t *pHandle)
{
	return create_component_common(ulContainer, pchName, pHandle);
}

vr::EVRInputError
Context::UpdateBooleanComponent(vr::VRInputComponentHandle_t ulComponent, bool bNewValue, double fTimeOffset)
{
	std::lock_guard lk(this->devices_mut);
	xrt_input *input = update_component_common_locked(ulComponent, fTimeOffset);
	if (input) {
		input->value.boolean = bNewValue;
	}
	return vr::VRInputError_None;
}

vr::EVRInputError
Context::CreateScalarComponent(vr::PropertyContainerHandle_t ulContainer,
                               const char *pchName,
                               vr::VRInputComponentHandle_t *pHandle,
                               vr::EVRScalarType eType,
                               vr::EVRScalarUnits eUnits)
{
	std::string_view name{pchName};
	// Lighthouse gives thumbsticks/trackpads as x/y components,
	// we need to combine them for Monado
	auto end = name.back();
	auto second_last = name.at(name.size() - 2);
	if (second_last == '/' && (end == 'x' || end == 'y')) {
		std::lock_guard lk(this->devices_mut);

		Device *device = static_cast<Device *>(prop_container_to_device(ulContainer));
		if (!device || this->internal_devices.count(device) == 0) {
			return vr::VRInputError_InvalidHandle;
		}

		bool x = end == 'x';
		name.remove_suffix(2);
		std::string n(name);
		xrt_input *input = device->get_input_from_name(n);
		if (!input) {
			return vr::VRInputError_None;
		}

		// Create the component mapping if it hasn't been created yet
		Vec2Components *components =
		    this->input.vec2_input_to_components.try_emplace(input, new Vec2Components).first->second.get();

		vr::VRInputComponentHandle_t new_handle = device->new_input_handle_locked();
		if (x)
			components->x = new_handle;
		else
			components->y = new_handle;

		this->input.handle_to_input[new_handle] = input;
		*pHandle = new_handle;
		return vr::VRInputError_None;
	}
	return create_component_common(ulContainer, pchName, pHandle);
}

static struct xrt_vec2
applyDeadzone(struct xrt_vec2 input)
{
	static const float deadzone = [] {
		float raw = debug_get_float_option_lh_stick_deadzone();
		// we apply deadzone to the input's absolute value; valid range is 0..1
		float clamped = CLAMP(raw, 0.0f, 0.99f);
		if (raw != clamped) {
			U_LOG_W("LH_STICK_DEADZONE value of %.2f falls outside of expected range 0..1 - clamp to %.2f",
			        raw, clamped);
		}
		return clamped;
	}();

	if (input.x * input.x + input.y * input.y <= deadzone * deadzone) {
		input.x = 0.0f;
		input.y = 0.0f;
	}

	return input;
}

vr::EVRInputError
Context::UpdateScalarComponent(vr::VRInputComponentHandle_t ulComponent, float fNewValue, double fTimeOffset)
{
	std::lock_guard lk(this->devices_mut);

	auto h = this->input.handle_to_input.find(ulComponent);
	if (h != this->input.handle_to_input.end() && h->second) {
		xrt_input *input = update_component_common_locked(ulComponent, fTimeOffset);
		if (XRT_GET_INPUT_TYPE(input->name) == XRT_INPUT_TYPE_VEC2_MINUS_ONE_TO_ONE) {
			std::unique_ptr<Vec2Components> &components = this->input.vec2_input_to_components.at(input);
			if (components->x == ulComponent) {
				input->value.vec2.x = fNewValue;
			} else if (components->y == ulComponent) {
				input->value.vec2.y = fNewValue;
			} else {
				CTX_WARN("Attempted to update component with handle %" PRIu64
				         " but it was neither the x nor y "
				         "component of its associated input",
				         ulComponent);
			}
			input->value.vec2 = applyDeadzone(input->value.vec2);

		} else {
			input->value.vec1.x = fNewValue;
		}
	}

	return vr::VRInputError_None;
}

vr::EVRInputError
Context::CreateHapticComponent(vr::PropertyContainerHandle_t ulContainer,
                               const char *pchName,
                               vr::VRInputComponentHandle_t *pHandle)
{
	*pHandle = vr::k_ulInvalidInputComponentHandle;

	std::lock_guard lk(this->devices_mut);
	Device *device = static_cast<Device *>(prop_container_to_device(ulContainer));
	if (!device || this->internal_devices.count(device) == 0) {
		return vr::VRInputError_InvalidHandle;
	}

	// Assuming HMDs won't have haptics.
	// Maybe a wrong assumption.
	if (device == hmd) {
		CTX_WARN("Didn't expect HMD with haptics.");
		return vr::VRInputError_InvalidHandle;
	}

	auto *controller = static_cast<ControllerDevice *>(device);
	vr::VRInputComponentHandle_t handle = controller->new_input_handle_locked();
	this->input.handle_to_input[handle] = nullptr;
	controller->set_haptic_handle(handle);
	*pHandle = handle;

	return vr::VRInputError_None;
}

vr::EVRInputError
Context::CreateSkeletonComponent(vr::PropertyContainerHandle_t ulContainer,
                                 const char *pchName,
                                 const char *pchSkeletonPath,
                                 const char *pchBasePosePath,
                                 vr::EVRSkeletalTrackingLevel eSkeletalTrackingLevel,
                                 const vr::VRBoneTransform_t *pGripLimitTransforms,
                                 uint32_t unGripLimitTransformCount,
                                 vr::VRInputComponentHandle_t *pHandle)
{
	std::string_view path(pchSkeletonPath); // should be /skeleton/hand/left or /skeleton/hand/right
	std::string_view skeleton_pfx("/skeleton/hand/");
	if (!path.starts_with(skeleton_pfx)) {
		CTX_ERR("Got invalid skeleton path: %s", std::string(path).c_str());
		return vr::VRInputError_InvalidSkeleton;
	}

	if (auto ret = create_component_common(ulContainer, pchSkeletonPath, pHandle); ret != vr::VRInputError_None) {
		return ret;
	}

	std::lock_guard lk(this->devices_mut);

	ControllerDevice *device = static_cast<ControllerDevice *>(prop_container_to_device(ulContainer));
	if (!device || this->internal_devices.count(device) == 0) {
		return vr::VRInputError_InvalidHandle;
	}
	path.remove_prefix(skeleton_pfx.size());
	xrt_hand hand;
	if (path == "left") {
		hand = XRT_HAND_LEFT;
	} else if (path == "right") {
		hand = XRT_HAND_RIGHT;
	} else {
		CTX_ERR("Got invalid skeleton path suffix: %s", std::string(path).c_str());
		return vr::VRInputError_InvalidSkeleton;
	}

	if (pGripLimitTransforms != nullptr && unGripLimitTransformCount != 0) {
		device->set_skeleton(std::span(pGripLimitTransforms, unGripLimitTransformCount), hand,
	                     eSkeletalTrackingLevel == vr::VRSkeletalTracking_Estimated, pchSkeletonPath);
	}
	this->input.skeleton_to_controller[*pHandle] = device;

	return vr::VRInputError_None;
}

vr::EVRInputError
Context::UpdateSkeletonComponent(vr::VRInputComponentHandle_t ulComponent,
                                 vr::EVRSkeletalMotionRange eMotionRange,
                                 const vr::VRBoneTransform_t *pTransforms,
                                 uint32_t unTransformCount)
{
	if (eMotionRange != vr::VRSkeletalMotionRange_WithoutController) {
		return vr::VRInputError_None;
	}

	std::lock_guard lk(this->devices_mut);
	if (!update_component_common_locked(ulComponent, 0)) {
		return vr::VRInputError_InvalidHandle;
	}

	auto *device = this->input.skeleton_to_controller[ulComponent];
	if (!device) {
		CTX_ERR("Got unknown component handle %" PRIu64, ulComponent);
		return vr::VRInputError_InvalidHandle;
	}

	device->update_skeleton_transforms(std::span(pTransforms, unTransformCount));

	return vr::VRInputError_None;
}

vr::EVRInputError
Context::CreatePoseComponent(vr::PropertyContainerHandle_t ulContainer,
                             const char *pchName,
                             vr::VRInputComponentHandle_t *pHandle)
{
	return vr::VRInputError_None;
}

vr::EVRInputError
Context::UpdatePoseComponent(vr::VRInputComponentHandle_t ulComponent,
                             const vr::HmdMatrix34_t *pMatPoseOffset,
                             double fTimeOffset)
{
	return vr::VRInputError_None;
}

vr::EVRInputError
Context::CreateEyeTrackingComponent(vr::PropertyContainerHandle_t ulContainer,
                                    const char *pchName,
                                    vr::VRInputComponentHandle_t *pHandle)
{
	return vr::VRInputError_None;
}

vr::EVRInputError
Context::UpdateEyeTrackingComponent(vr::VRInputComponentHandle_t ulComponent,
                                    const vr::VREyeTrackingData_t *pEyeTrackingData,
                                    double fTimeOffset)
{
	return vr::VRInputError_None;
}

/***** IVRProperties methods *****/

vr::ETrackedPropertyError
Context::ReadPropertyBatch(vr::PropertyContainerHandle_t ulContainerHandle,
                           vr::PropertyRead_t *pBatch,
                           uint32_t unBatchEntryCount)
{
	std::lock_guard lk(this->devices_mut);

	xrt_device *device = prop_container_to_device(ulContainerHandle);
	if (!device) {
		CTX_WARN("Invalid property container handle %u for device lookup", ulContainerHandle);
		return vr::TrackedProp_InvalidContainer;
	}
	if (!pBatch)
		return vr::TrackedProp_InvalidOperation; // not verified vs steamvr

	if(this->internal_devices.count(device) == 0) {
		return handle_external_read_properties(device, pBatch, unBatchEntryCount);
	}
	return static_cast<Device *>(device)->handle_read_properties(pBatch, unBatchEntryCount);
}

vr::ETrackedPropertyError
Context::WritePropertyBatch(vr::PropertyContainerHandle_t ulContainerHandle,
                            vr::PropertyWrite_t *pBatch,
                            uint32_t unBatchEntryCount)
{
	std::lock_guard lk(this->devices_mut);

	Device *device = static_cast<Device *>(prop_container_to_device(ulContainerHandle));
	if (!device || this->internal_devices.count(device) == 0) {
		return vr::TrackedProp_InvalidContainer;
	}
	if (!pBatch)
		return vr::TrackedProp_InvalidOperation; // not verified vs steamvr
	return device->handle_properties(pBatch, unBatchEntryCount);
}

const char *
Context::GetPropErrorNameFromEnum(vr::ETrackedPropertyError error)
{
	return nullptr;
}



std::vector<xrt_device *>
Context::get_devices()
{
	if(this->xsysd->static_roles.head != nullptr) {
		xrt_device *device = this->xsysd->static_roles.head;
		if(devices[0] != device) {
			if(devices[0] != nullptr) {
				registered_devices.erase(devices[0]);
				CTX_INFO("Removed head device %s (%s) from devices[0]", devices[0]->str, devices[0]->serial);
			}
			devices[0] = device;
			registered_devices.insert(device);
			CTX_INFO("Added head device %s (%s) to devices[0]", device->str, device->serial);
		}
	}

	for (uint32_t i = 0; i < this->xsysd->static_xdev_count; ++i) {
		if(this->xsysd->static_xdevs[i]->device_type == XRT_DEVICE_TYPE_HMD) continue;
		xrt_device *device = this->xsysd->static_xdevs[i];
		if(registered_devices.count(device)) continue;
		devices.push_back(device);
		registered_devices.insert(device);
		CTX_INFO("Added device %s (%s) to devices[%zu]", device->str, device->serial, devices.size() - 1);
	}

	return devices;
}

xrt_device *
Context::prop_container_to_device(vr::PropertyContainerHandle_t handle)
{
	std::vector<xrt_device *> devices = get_devices();

	if (handle != vr::k_ulInvalidPropertyContainer && handle <= MAX_CONTROLLERS && devices.size() >= handle) {
		return devices[handle - 1];
	}

	CTX_WARN("Invalid property container handle %u for device lookup", handle);

	return nullptr;
}

vr::ETrackedPropertyError
Context::handle_external_read_properties(xrt_device *device, vr::PropertyRead_t *batch, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i) {
		vr::ETrackedPropertyError err = handle_external_read_property(device, batch[i]);
		if (err != vr::ETrackedPropertyError::TrackedProp_Success) {
			return err;
		}
	}
	return vr::ETrackedPropertyError::TrackedProp_Success;
}

vr::ETrackedPropertyError
Context::handle_external_read_property(xrt_device *device, vr::PropertyRead_t &prop)
{
	// CTX_DEBUG("Device %s: Reading external property %d", device->str, prop.prop);

	if(external_properties.count(device) == 0) {
		external_properties.emplace(device, std::unordered_map<vr::ETrackedDeviceProperty, Property>());
	}
	std::unordered_map<vr::ETrackedDeviceProperty, Property> &device_properties = external_properties[device];

	switch (prop.prop) {
		case vr::ETrackedDeviceProperty::Prop_DeviceClass_Int32:
			if(device_properties.count(prop.prop) == 0) {
				vr::ETrackedDeviceClass value = vr::ETrackedDeviceClass::TrackedDeviceClass_Invalid;
				if(device->device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER) {
					value = vr::ETrackedDeviceClass::TrackedDeviceClass_Controller;
				} else if(device->device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER) {
					value = vr::ETrackedDeviceClass::TrackedDeviceClass_Controller;
				} else if(device->device_type == XRT_DEVICE_TYPE_GENERIC_TRACKER) {
					value = vr::ETrackedDeviceClass::TrackedDeviceClass_GenericTracker;
				} else if(device->device_type == XRT_DEVICE_TYPE_HMD) {
					value = vr::ETrackedDeviceClass::TrackedDeviceClass_HMD;
				}
				CTX_DEBUG("Device %s: External property Prop_DeviceClass_Int32 is %d", device->str, value);
				device_properties.emplace(prop.prop, Property(vr::k_unInt32PropertyTag, &value, (uint32_t)sizeof(int32_t)));
			}
			break;
		case vr::ETrackedDeviceProperty::Prop_SerialNumber_String:
			if(device_properties.count(prop.prop) == 0) {
				std::string serial = device->serial;
				CTX_DEBUG("Device %s: External property Prop_SerialNumber_String is %s", device->str, serial.c_str());
				device_properties.emplace(prop.prop, Property(vr::k_unStringPropertyTag, (void *)serial.c_str(), (uint32_t)(serial.size() + 1)));
			}
			break;
		case vr::ETrackedDeviceProperty::Prop_ControllerRoleHint_Int32:
			if(device_properties.count(prop.prop) == 0) {
				vr::ETrackedControllerRole value = vr::ETrackedControllerRole::TrackedControllerRole_Invalid;
				if (device->device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER) {
					value = vr::ETrackedControllerRole::TrackedControllerRole_LeftHand;
				} else if (device->device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER) {
					value = vr::ETrackedControllerRole::TrackedControllerRole_RightHand;
				}
				CTX_DEBUG("Device %s: External property Prop_ControllerRoleHint_Int32 is %d", device->str, value);
				device_properties.emplace(prop.prop, Property(vr::k_unInt32PropertyTag, &value, (uint32_t)sizeof(int32_t)));
			}
			break;
		case 10987: // VendorSpecific_Standable_987
			break;
		default:
			CTX_WARN("Unsupported property type %u for external device %s (%s)", prop.prop, device->str, device->serial);
			prop.eError = vr::ETrackedPropertyError::TrackedProp_UnknownProperty;
	}

	if(device_properties.count(prop.prop)) {
		Property property = device_properties.at(prop.prop);
		prop.unTag = property.tag;
		prop.unRequiredBufferSize = property.buffer.size();
		if (prop.pvBuffer == nullptr || prop.unBufferSize < property.buffer.size()) {
			prop.eError = vr::ETrackedPropertyError::TrackedProp_BufferTooSmall;
			return prop.eError;
		}
		std::memcpy(prop.pvBuffer, property.buffer.data(), property.buffer.size());
		prop.eError = vr::ETrackedPropertyError::TrackedProp_Success;
	} else {
		prop.eError = vr::ETrackedPropertyError::TrackedProp_UnknownProperty;
	}

	return prop.eError;
}

vr::PropertyContainerHandle_t
Context::TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t nDevice)
{
	std::vector<xrt_device *> devices = get_devices();
	if (nDevice <= MAX_CONTROLLERS && devices.size() > nDevice) {
		if(devices[nDevice] == nullptr) {
			CTX_WARN("Device at index %u is null", nDevice);
			return vr::k_ulInvalidPropertyContainer;
		}
		return nDevice + 1;
	}

	CTX_WARN("Invalid device index %u for property container lookup", nDevice);

	return vr::k_ulInvalidPropertyContainer;
}

void
Context::Log(const char *pchLogMessage)
{
	// check if the message has empty space or new line at the end and remove it
	std::string_view message(pchLogMessage);
	while (!message.empty() && (message.back() == ' ' || message.back() == '\n' || message.back() == '\r' || message.back() == '\t')) {
		message.remove_suffix(1);
	}
	std::string shortened_message(message);
	CTX_TRACE("[openvr %s]: %s", this->driver_name.c_str(), shortened_message.c_str());
}
// NOLINTEND(bugprone-easily-swappable-parameters)

extern "C" enum xrt_result
steamvr_lh_create_devices(struct xrt_prober *xp, struct xrt_system_devices *xsysd)
{
	u_logging_level level = debug_get_log_option_lh_log();

	if (xsysd == NULL) {
		U_LOG_IFL_E(level, "Invalid system pointer");
		return xrt_result::XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	std::filesystem::path steamvr{};
	if (getenv("STEAMVR_PATH") != nullptr) {
		steamvr = getenv("STEAMVR_PATH");
	} else {
		steamvr = find_steamvr_install();
	}

	std::vector<std::filesystem::path> driver_directories = {};

	// go through the steamvr drivers directory and find all the subdirectories that contain a driver manifest file

	std::filesystem::path steamvr_drivers_path = steamvr / "drivers";

	if (!std::filesystem::exists(steamvr_drivers_path)) {
		U_LOG_IFL_W(level, "Couldn't find SteamVR drivers folder (%s)", steamvr_drivers_path.c_str());
	} else {
		U_LOG_IFL_I(level, "Found SteamVR install: %s", steamvr.c_str());
		for (const auto &entry : std::filesystem::directory_iterator(steamvr_drivers_path)) {
			if (entry.is_directory()) {
				std::filesystem::path manifest_path = entry.path() / "driver.vrdrivermanifest";
				if (std::filesystem::exists(manifest_path)) {
					driver_directories.push_back(entry.path());
					U_LOG_IFL_I(level, "Found built-in driver directory: %s", entry.path().string().c_str());
				}
			}
		}
	}

	// go through the external_drivers in `~/.config/openvr/openvrpaths.vrpath` and find all directories that contain a driver manifest file
	// openvrpaths.vrpath is a JSON
	// {
	//   "jsonid" : "vrpathreg",
	//   "config": [
	//     "$HOME/.local/share/Steam/config",
	//     "/path/to/config2"
	//   ],
	//   "external_drivers": [
	//     "/path/to/driver1",
	//     "/path/to/driver2"
	//   ]
	// }

	std::vector<std::filesystem::path> config_directories = {};

	// load and parse the JSON file
	std::filesystem::path openvrpaths_path = std::filesystem::path(getenv("HOME")) / ".config/openvr/openvrpaths.vrpath";
	if (std::filesystem::exists(openvrpaths_path)) {
		auto openvrpaths_json = JSONNode::loadFromFile(openvrpaths_path);
		if (openvrpaths_json.hasKey("jsonid") && openvrpaths_json["jsonid"].isString() == true && openvrpaths_json["jsonid"].asString() == "vrpathreg") {
			U_LOG_IFL_I(level, "Found openvrpaths.vrpath: %s", openvrpaths_path.c_str());
			if(openvrpaths_json.hasKey("config") && openvrpaths_json["config"].isArray() == true) {
				for (const auto &config_path_json : openvrpaths_json["config"].asArray()) {
					if (config_path_json.isString() == false) {
						U_LOG_IFL_W(level, "Invalid config path in openvrpaths.vrpath: %s", config_path_json.toString().c_str());
						continue;
					}
					config_directories.push_back(config_path_json.asString());
				}
			}
			if(openvrpaths_json.hasKey("external_drivers") && openvrpaths_json["external_drivers"].isArray() == true) {
				for (const auto &driver_path_json : openvrpaths_json["external_drivers"].asArray()) {
					if (driver_path_json.isString() == false) {
						U_LOG_IFL_W(level, "Invalid external driver path in openvrpaths.vrpath: %s", driver_path_json.toString().c_str());
						continue;
					}
					std::filesystem::path driver_path = driver_path_json.asString();
					if (driver_path != "" && std::filesystem::exists(driver_path)) {
						std::filesystem::path manifest_path = driver_path / "driver.vrdrivermanifest";
						if (std::filesystem::exists(manifest_path)) {
							driver_directories.push_back(driver_path);
							U_LOG_IFL_I(level, "Found external driver directory: %s",
										driver_path.c_str());
						}
					}
				}
			}
		}
	}

	std::filesystem::path user_settings_path{};
	for (const auto &config_path : config_directories) {
		if (config_path != "" && std::filesystem::exists(config_path)) {
			if(std::filesystem::exists(config_path / "steamvr.vrsettings") == false) {
				continue;
			}
			user_settings_path = config_path / "steamvr.vrsettings";
		}
	}

	if (getenv("OPENVR_USER_SETTINGS") != nullptr) {
		user_settings_path = getenv("OPENVR_USER_SETTINGS");
		U_LOG_IFL_I(level, "Using SteamVR user settings from OPENVR_USER_SETTINGS: %s", user_settings_path.c_str());
	}

	if (!std::filesystem::exists(user_settings_path)) {
		U_LOG_IFL_W(level, "Couldn't find SteamVR user settings file (%s)", user_settings_path.c_str());
	} else {
		U_LOG_IFL_I(level, "Found SteamVR user settings: %s", user_settings_path.c_str());
	}

	JSONNode user_settings_json = JSONNode::loadFromFile(user_settings_path);

	// go through the driver directories and create a Context for each one, and add it to the drivers vector

	std::vector<std::shared_ptr<Context>> driver_contexts = {};

	// a map of driver name to driver directory
	std::map<std::string, std::filesystem::path> driver_name_to_directory = {};
	std::vector<std::string> enabled_drivers = {};

	for (const auto &driver_dir : driver_directories) {
		// parse the driver manifest file and get the driver name
		std::filesystem::path manifest_path = driver_dir / "driver.vrdrivermanifest";
		if (std::filesystem::exists(manifest_path)) {
			auto manifest_json = JSONNode::loadFromFile(manifest_path);
			if (manifest_json.hasKey("name") && manifest_json["name"].isString()) {
				std::string driver_name = manifest_json["name"].asString();
				driver_name_to_directory[driver_name] = driver_dir;

				if(manifest_json.hasKey("resourceOnly") && manifest_json["resourceOnly"].isBool() && manifest_json["resourceOnly"].asBool() == true) {
					U_LOG_IFL_I(level, "Skipping resource-only driver %s", driver_name.c_str());
					continue;
				}

				if(user_settings_json.hasKey("driver_" + driver_name) && user_settings_json["driver_" + driver_name].hasKey("enable")) {
					if(user_settings_json["driver_" + driver_name]["enable"].isBool() && user_settings_json["driver_" + driver_name]["enable"].asBool() == false) {
						U_LOG_IFL_I(level, "Skipping disabled driver %s in user settings", driver_name.c_str());
						continue;
					}
				}

				std::filesystem::path driver_path = driver_dir / "bin" / OVR_PLAT_SUBDIR / ("driver_" + driver_name + OVR_PLAT_EXT);
				if (!std::filesystem::exists(driver_path)) {
					U_LOG_IFL_I(level, "Skipping driver %s because missing driver library for %s", driver_name.c_str(), OVR_PLAT_SUBDIR);
					continue;
				}

				enabled_drivers.push_back(driver_name);
			}
		}
	}

	// Pimax P2 init needs to happen before lighthouse init, as it may disconnect the HMD lighthouse USB device if
	// PIMAX_REBOOT is enabled, which resets the USB hub in the headset.
	// The lighthouse driver does not reconnect to USB devices and will stop tracking devices that have been
	// disconnected.
#ifdef XRT_BUILD_DRIVER_PIMAX_P2
	pimax_p2_device *pdev = nullptr;
	pimax_p2_init(&pdev);
#endif

	for (const auto &driver_name : enabled_drivers) {
		auto driver_dir = driver_name_to_directory[driver_name];
		std::filesystem::path driver_path = driver_dir / "bin" / OVR_PLAT_SUBDIR / ("driver_" + driver_name + OVR_PLAT_EXT);
		U_LOG_IFL_I(level, "Loading driver %s: %s", driver_name.c_str(), driver_path.c_str());
		
		os_library_t driver_lib = os_library_open(driver_path.c_str());
		if (!driver_lib) {
			U_LOG_IFL_E(level, "Couldn't open driver lib: %s", os_library_error());
			continue;
		}

		void *sym = os_library_symbol(driver_lib, "HmdDriverFactory");
		if (!sym) {
			U_LOG_IFL_E(level, "Couldn't find HmdDriverFactory in driver lib: %s", os_library_error());
			continue;
		}

		using HmdDriverFactory_t = void *(*)(const char *, int *);
		auto factory = reinterpret_cast<HmdDriverFactory_t>(sym);

		vr::EVRInitError err = vr::VRInitError_None;

		vr::IServerTrackedDeviceProvider *driver = static_cast<vr::IServerTrackedDeviceProvider *>(
			factory(vr::IServerTrackedDeviceProvider_Version, (int *)&err));
		if (err != vr::VRInitError_None) {
			U_LOG_IFL_E(level, "Couldn't get tracked device driver: error %u", err);
			continue;
		}

		std::filesystem::path chaperone_info_file = "chaperone_info.vrchap";
		if(driver_name == "playstation_vr2") {
			chaperone_info_file = "playstation_vr2/chaperone_info.vrchap";
		}
		std::filesystem::path chaperone_info_path{};

		for (const auto &config_path : config_directories) {
			if (config_path != "" && std::filesystem::exists(config_path)) {
				if(std::filesystem::exists(config_path / chaperone_info_file) == false) {
					continue;
				}
				chaperone_info_path = config_path / chaperone_info_file;
			}
		}

		auto ctx = Context::create(driver_dir, driver_name, user_settings_path, chaperone_info_path, steamvr, driver_name_to_directory, driver, xsysd);

		if(!ctx) {
			U_LOG_IFL_E(level, "Couldn't create context for driver %s", driver_name.c_str());
			continue;
		}

		U_LOG_IFL_I(level, "Waiting for device discovery for driver %s...", driver_name.c_str());

		ctx->wait_for_discover();

		U_LOG_IFL_I(level, "Device search time complete for driver %s.", driver_name.c_str());

		driver_contexts.push_back(ctx);
	}

	if (driver_contexts.size() == 0) {
		U_LOG_IFL_W(level, "No drivers found!");
		return xrt_result::XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_IFL_I(level, "Loaded and initialized %zu drivers: ", driver_contexts.size());
	for (const auto &ctx : driver_contexts) {
		U_LOG_IFL_I(level, " - %s", ctx->driver_name.c_str());
	}

	for (auto &ctx : driver_contexts) {
		std::lock_guard lk(ctx->devices_mut);
		ctx->in_setup = false;

		// Include the HMD
		if (ctx->hmd) {
			if (ctx->hmd->variant == VIVE_VARIANT_PRO2 && !ctx->hmd->init_vive_pro_2(xp)) {
				U_LOG_IFL_W(level, "Found Vive Pro 2, but failed to initialize.");
			}
#ifdef XRT_BUILD_DRIVER_PIMAX_P2
			if (ctx->hmd->variant == VIVE_VARIANT_PIMAX_P2 && (pdev == nullptr || !ctx->hmd->init_pimax(pdev))) {
				U_LOG_IFL_W(level, "Found Pimax P2, but failed to initialize.");
			}
#else
			if (ctx->hmd->variant == VIVE_VARIANT_PIMAX_P2) {
				U_LOG_IFL_W(level, "Found Pimax P2, but the driver is not compiled in.");
			}
#endif
			xsysd->static_xdevs[xsysd->static_xdev_count++] = ctx->hmd;
		}

		// Include the controllers
		for (size_t i = 0; i < MAX_CONTROLLERS; i++) {
			if (ctx->controller[i]) {
				xsysd->static_xdevs[xsysd->static_xdev_count++] = ctx->controller[i];
			}
		}
	}

	if (xsysd->static_xdev_count == 0) {
		U_LOG_IFL_W(level, "No devices found!");
		return xrt_result::XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	return xrt_result::XRT_SUCCESS;
}
