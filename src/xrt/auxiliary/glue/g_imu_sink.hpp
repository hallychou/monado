// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for glue classes to wrap xrt IMU sink interfaces.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_tracking.h"

#include "g_catch_guard.hpp"
#include "g_traits.hpp"

#include <type_traits>


namespace xrt::util {

/*!
 * CRTP glue wrapper for @ref xrt_imu_sink. Relies on standard layout to
 * recover the derived object from the C struct, and has some requirements and
 * limitations because of that. See @ref cpp-glue-wrappers for the guide and
 * conventions for these wrappers.
 *
 * The derived class `T` must implement `void pushImu(xrt_imu_sample &sample)`.
 * Exceptions thrown from it are caught and logged. Unlike frame sinks, IMU
 * sinks are not part of a frame context graph, so the derived class manages
 * its own lifetime.
 */
template <class T> class ImuSinkBase
{
public: // Methods
	ImuSinkBase() noexcept
	{
		static_assert(std::is_standard_layout_v<ImuSinkBase>,
		              "glue base must be standard layout for pointer recovery");
		static_assert(is_non_virtual_base_v<ImuSinkBase, T>,
		              "glue base must be a non-virtual base of T for pointer recovery");

		mImuSink.push_imu = pushImuWrap;
	}

	~ImuSinkBase() noexcept = default;

	const T &
	derived() const noexcept
	{
		return static_cast<const T &>(*this);
	}

	T &
	derived() noexcept
	{
		return static_cast<T &>(*this);
	}

	static const T *
	fromImuSink(const xrt_imu_sink *sink) noexcept
	{
		return &(reinterpret_cast<const ImuSinkBase *>(sink)->derived());
	}

	static T *
	fromImuSink(xrt_imu_sink *sink) noexcept
	{
		return &(reinterpret_cast<ImuSinkBase *>(sink)->derived());
	}

	const xrt_imu_sink *
	getImuSink() const noexcept
	{
		return &mImuSink;
	}

	xrt_imu_sink *
	getImuSink() noexcept
	{
		return &mImuSink;
	}


private: // Members
	/*!
	 * Wrapped @ref xrt_imu_sink. Must be the first data member: a pointer to
	 * it is then interconvertible with a pointer to this standard-layout base,
	 * which lets the glue cast a C pointer back to the derived C++ class. See
	 * @ref cpp-glue-wrappers.
	 */
	xrt_imu_sink mImuSink = {};


private: // Functions
// Not `->derived()`, that is ambiguous when T has more than one glue base.
#define GET(sink) (*fromImuSink(sink))

	static void
	pushImuWrap(struct xrt_imu_sink *sink, struct xrt_imu_sample *sample) noexcept
	try {
		GET(sink).pushImu(*sample);
	}
	G_CATCH_GUARDS_VOID

#undef GET
};

} // namespace xrt::util
