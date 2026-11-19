// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Internal header for sensor fusion usage of rerun logging.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup tracking
 */

#pragma once

#include "constellation/constellation_rerun_common.hpp"
#include "sensor_fusion.hpp"

#include <rerun.hpp>


namespace xrt::tracking::constellation::optimizer::sensor_fusion {

//! Whether to log older keyframes or just the newest.
constexpr bool kLogOldKeyframes = false;

const std::string_view kRerunRecordingId = "constellation_sensor_fusion";

struct RerunContext : public RerunStream
{
public: // Methods
	RerunContext() : RerunStream(kRerunRecordingId) {}

	/*!
	 * Logs one sliding-window solve to rerun.
	 *
	 * Everything is logged against two timelines: a `solves` sequence, which is the only one that advances
	 * monotonically, and a `fusion_time` timestamp taken from the newest keyframe in the window.
	 *
	 * @param snapshot The window and its solved state, as captured around @ref SensorFusion::runFusionLocked.
	 */
	void
	logSolve(const SolveSnapshot &snapshot);

	/*!
	 * Logs one dead reckoned IMU relation history entry to rerun.
	 *
	 * Logged only against its own `imu_time` timestamp timeline, and under its own entity, so it never moves the
	 * solved device on the solve timelines.
	 *
	 * @param entry The entry as it was pushed to the IMU relation history.
	 */
	void
	logImuRelation(const CovarianceRelationHistoryEntry &entry);

private: // Methods
	void
	logStaticScene();

	/*!
	 * Logs a 3 sigma position covariance ellipsoid at @p entity, which must be a child of an entity carrying the
	 * @p Tcv_world_device transform.
	 */
	void
	logPositionCovariance(const std::string &entity,
	                      const xrt_pose &Tcv_world_device,
	                      const PoseCovarianceMatrix &covariance);

	void
	logSolveSummary(const SolveSnapshot &snapshot);

	void
	logSolverMetrics(const SolveSnapshot &snapshot);

	void
	logCameras(const SolveSnapshot &snapshot);

	void
	logDevice(const SolveSnapshot &snapshot);

	void
	logDeviceWindow(const SolveSnapshot &snapshot);

	void
	logDeviceMetrics(const SolveSnapshot &snapshot);

	void
	logObservations(const SolveSnapshot &snapshot);
};

}; // namespace xrt::tracking::constellation::optimizer::sensor_fusion
