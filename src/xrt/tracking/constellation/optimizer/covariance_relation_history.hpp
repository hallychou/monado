// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Covariance-aware relation history.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup tracking
 */

#pragma once

#include "math/m_relation_history.hpp"

#include "imu_preintegration.hpp"

namespace xrt::tracking::constellation::optimizer {

struct CovarianceRelationHistoryEntry : RelationHistoryEntry
{
public: // Fields
	PoseCovarianceMatrix covariance{};

private: // Fields
	const xrt_imu_noise_calibration *imu_calibration{};

	xrt_imu_sample raw_imu_sample{};
	xrt_imu_sample calibrated_imu_sample{};
	Eigen::Vector3d calibrated_accel_scale{};
	Eigen::Quaterniond Q_model_imu{};

private: // Methods
	explicit CovarianceRelationHistoryEntry(RelationHistoryEntry base)
	{
		this->relation = base.relation;
		this->timestamp = base.timestamp;
	}

public: // Methods
	CovarianceRelationHistoryEntry(const xrt_space_relation &relation,
	                               timepoint_ns timestamp,
	                               const PoseCovarianceMatrix &covariance,
	                               const xrt_imu_noise_calibration *imu_calibration,
	                               const xrt_imu_sample &raw_imu_sample,
	                               const xrt_imu_sample &calibrated_imu_sample,
	                               Eigen::Vector3d calibrated_accel_scale,
	                               Eigen::Quaterniond Q_model_imu)
	    : RelationHistoryEntry(relation, timestamp),      //
	      covariance(covariance),                         //
	      imu_calibration(imu_calibration),               //
	      raw_imu_sample(raw_imu_sample),                 //
	      calibrated_imu_sample(calibrated_imu_sample),   //
	      calibrated_accel_scale(calibrated_accel_scale), //
	      Q_model_imu(Q_model_imu)                        //
	{}

	CovarianceRelationHistoryEntry()
	    : covariance(PoseCovarianceMatrix::Zero()),        //
	      imu_calibration(nullptr),                        //
	      calibrated_accel_scale(Eigen::Vector3d::Ones()), //
	      Q_model_imu(Eigen::Quaterniond::Identity())      //
	{}

	CovarianceRelationHistoryEntry
	predict(time_duration_ns delta_ns) const
	{
		assert(this->imu_calibration != nullptr);

		double delta_s = time_ns_to_s(delta_ns);

		CovarianceRelationHistoryEntry final = *this;
		final.timestamp += delta_ns;
		m_predict_relation(&this->relation, delta_s, &final.relation);

		/*!
		 * @note Since we're propagating an *absolute* orientation covariance in world space, we use
		 *       `Q_world_begin` instead of `Q_begin_end`.
		 */

		//! @todo Integrate IMU bias noise propagation.

		auto Q_world_imu = map_quat(this->relation.pose.orientation).cast<double>() * this->Q_model_imu;

		// Bring covariance into IMU frame
		PoseCovarianceMatrix cov_imu =
		    rotateCovarianceRotBlock(this->covariance, this->Q_model_imu.conjugate());

		propagateCovariance(                                         //
		    Q_world_imu,                                             //
		    map_vec3_f64(this->raw_imu_sample.accel_m_s2),           //
		    map_vec3_f64(this->raw_imu_sample.gyro_rad_secs),        //
		    map_vec3_f64(this->calibrated_imu_sample.accel_m_s2),    //
		    map_vec3_f64(this->calibrated_imu_sample.gyro_rad_secs), //
		    this->calibrated_accel_scale,                            //
		    *this->imu_calibration,                                  //
		    delta_s,                                                 //
		    cov_imu);                                                //

		// Bring covariance back to model frame
		final.covariance = rotateCovarianceRotBlock(cov_imu, this->Q_model_imu);

		return final;
	}

	CovarianceRelationHistoryEntry
	interpolate(timepoint_ns to_ns, const CovarianceRelationHistoryEntry &successor) const
	{
		double t = (double)(to_ns - this->timestamp) / (double)(successor.timestamp - this->timestamp);

		auto final = CovarianceRelationHistoryEntry(this->RelationHistoryEntry::interpolate(to_ns, successor));

		auto R_a = map_quat(this->relation.pose.orientation).cast<double>();
		auto R_t = map_quat(final.relation.pose.orientation).cast<double>();
		auto R_b = map_quat(successor.relation.pose.orientation).cast<double>();

		// Rotate both covariances into the frame of the final covariance matrix before blending them
		final.covariance = ((1 - t) * rotateCovarianceRotBlock(this->covariance, R_t.conjugate() * R_a)) +
		                   (t * rotateCovarianceRotBlock(successor.covariance, R_t.conjugate() * R_b));

		return final;
	}

	void
	estimateMotion(CovarianceRelationHistoryEntry &in_out_end) const
	{
		this->RelationHistoryEntry::estimateMotion(in_out_end);
	}
};

using CovarianceRelationHistory = xrt::auxiliary::math::RelationHistoryBase<CovarianceRelationHistoryEntry>;

}; // namespace xrt::tracking::constellation::optimizer
