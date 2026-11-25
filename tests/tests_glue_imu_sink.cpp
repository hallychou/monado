// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Tests for glue/g_imu_sink.hpp
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 */

#include "glue/g_frame_node.hpp"
#include "glue/g_imu_sink.hpp"

#include "catch_amalgamated.hpp"

#include <stdexcept>


namespace {

//! Records every sample pushed to it.
class RecordingImuSink : public xrt::util::ImuSinkBase<RecordingImuSink>
{
public:
	int count = 0;
	xrt_imu_sample *last_ptr = nullptr;
	xrt_imu_sample last = {};

	void
	pushImu(xrt_imu_sample &sample)
	{
		count++;
		last_ptr = &sample;
		last = sample;
	}
};

//! Always throws, used to test the exception guard.
class ThrowingImuSink : public xrt::util::ImuSinkBase<ThrowingImuSink>
{
public:
	int count = 0;

	void
	pushImu(xrt_imu_sample &)
	{
		count++;
		throw std::runtime_error("test exception");
	}
};

//! Sink that also has another glue base, to check pointer recovery with several bases.
class MultiBaseImuSink : public xrt::util::FrameNodeBase<MultiBaseImuSink>,
                         public xrt::util::ImuSinkBase<MultiBaseImuSink>
{
public:
	int count = 0;

	void
	pushImu(xrt_imu_sample &)
	{
		count++;
	}

	void
	breakApart()
	{}

	static void
	destroyFrameNode(struct xrt_frame_node *)
	{}
};

xrt_imu_sample
makeSample()
{
	xrt_imu_sample s = {};
	s.timestamp_ns = 12345;
	s.accel_m_s2 = {1.0, 2.0, 3.0};
	s.gyro_rad_secs = {0.1, 0.2, 0.3};
	return s;
}

} // namespace


TEST_CASE("ImuSinkBase: C callback is installed", "[glue][imu_sink]")
{
	RecordingImuSink sink;
	CHECK(sink.getImuSink()->push_imu != nullptr);
}

TEST_CASE("ImuSinkBase: push through the C interface reaches derived class", "[glue][imu_sink]")
{
	RecordingImuSink sink;
	xrt_imu_sample sample = makeSample();

	xrt_sink_push_imu(sink.getImuSink(), &sample);

	CHECK(sink.count == 1);
	CHECK(sink.last_ptr == &sample);
	CHECK(sink.last.timestamp_ns == 12345);
	CHECK(sink.last.accel_m_s2.y == 2.0);
	CHECK(sink.last.gyro_rad_secs.z == 0.3);

	xrt_sink_push_imu(sink.getImuSink(), &sample);
	CHECK(sink.count == 2);
}

TEST_CASE("ImuSinkBase: pointer recovery round trips", "[glue][imu_sink]")
{
	RecordingImuSink sink;
	xrt_imu_sink *c = sink.getImuSink();

	CHECK(RecordingImuSink::fromImuSink(c) == &sink);

	const RecordingImuSink &csink = sink;
	CHECK(RecordingImuSink::fromImuSink(csink.getImuSink()) == &csink);
	CHECK(&sink.derived() == &sink);
	CHECK(&csink.derived() == &csink);
}

TEST_CASE("ImuSinkBase: separate sinks are independent", "[glue][imu_sink]")
{
	RecordingImuSink a, b;
	xrt_imu_sample sample = makeSample();

	xrt_sink_push_imu(a.getImuSink(), &sample);

	CHECK(a.count == 1);
	CHECK(b.count == 0);
}

TEST_CASE("ImuSinkBase: exceptions are caught", "[glue][imu_sink]")
{
	ThrowingImuSink sink;
	xrt_imu_sample sample = makeSample();

	CHECK_NOTHROW(xrt_sink_push_imu(sink.getImuSink(), &sample));
	CHECK(sink.count == 1);
}

TEST_CASE("ImuSinkBase: works alongside other glue bases", "[glue][imu_sink]")
{
	MultiBaseImuSink sink;
	xrt_imu_sample sample = makeSample();

	xrt_sink_push_imu(sink.getImuSink(), &sample);
	CHECK(sink.count == 1);
	CHECK(MultiBaseImuSink::fromImuSink(sink.getImuSink()) == &sink);
	CHECK(MultiBaseImuSink::fromFrameNode(sink.getFrameNode()) == &sink);
}
