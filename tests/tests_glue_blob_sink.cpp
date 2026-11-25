// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Tests for glue/g_blob_sink.hpp
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 */

#include "glue/g_blob_sink.hpp"
#include "glue/g_frame_node.hpp"

#include "catch_amalgamated.hpp"

#include <stdexcept>


namespace {

//! Records every observation pushed to it.
class RecordingBlobSink : public xrt::util::BlobSinkBase<RecordingBlobSink>
{
public:
	int count = 0;
	bool destroyed = false;
	t_blob_observation *last_ptr = nullptr;
	uint64_t last_sequence_id = 0;
	uint32_t last_num_blobs = 0;

	void
	pushBlobs(t_blob_observation &observation)
	{
		count++;
		last_ptr = &observation;
		last_sequence_id = observation.sequence_id;
		last_num_blobs = observation.num_blobs;
	}

	static void
	destroyBlobSink(t_blob_sink *sink)
	{
		fromBlobSink(sink)->destroyed = true;
	}
};

//! Always throws, used to test the exception guards.
class ThrowingBlobSink : public xrt::util::BlobSinkBase<ThrowingBlobSink>
{
public:
	int count = 0;
	int destroy_count = 0;

	void
	pushBlobs(t_blob_observation &)
	{
		count++;
		throw std::runtime_error("test exception");
	}

	static void
	destroyBlobSink(t_blob_sink *sink)
	{
		fromBlobSink(sink)->destroy_count++;
		throw std::runtime_error("test exception");
	}
};

//! Sink that also has another glue base, to check pointer recovery with several bases.
class MultiBaseBlobSink : public xrt::util::FrameNodeBase<MultiBaseBlobSink>,
                          public xrt::util::BlobSinkBase<MultiBaseBlobSink>
{
public:
	int count = 0;

	void
	pushBlobs(t_blob_observation &)
	{
		count++;
	}

	static void
	destroyBlobSink(t_blob_sink *)
	{}

	void
	breakApart()
	{}

	static void
	destroyFrameNode(struct xrt_frame_node *)
	{}
};

t_blob_observation
makeObservation()
{
	t_blob_observation o = {};
	o.sequence_id = 42;
	o.timestamp_ns = 1000;
	o.num_blobs = 7;
	return o;
}

} // namespace


TEST_CASE("BlobSinkBase: C callbacks are installed", "[glue][blob_sink]")
{
	RecordingBlobSink sink;
	CHECK(sink.getBlobSink()->push_blobs != nullptr);
	CHECK(sink.getBlobSink()->destroy != nullptr);
}

TEST_CASE("BlobSinkBase: push through the C interface reaches derived class", "[glue][blob_sink]")
{
	RecordingBlobSink sink;
	t_blob_observation obs = makeObservation();

	t_blob_sink_push_blobs(sink.getBlobSink(), &obs);

	CHECK(sink.count == 1);
	CHECK(sink.last_ptr == &obs);
	CHECK(sink.last_sequence_id == 42);
	CHECK(sink.last_num_blobs == 7);

	t_blob_sink_push_blobs(sink.getBlobSink(), &obs);
	CHECK(sink.count == 2);
}

TEST_CASE("BlobSinkBase: destroy goes through the C interface", "[glue][blob_sink]")
{
	RecordingBlobSink sink;
	t_blob_sink *c = sink.getBlobSink();

	CHECK_FALSE(sink.destroyed);
	t_blob_sink_destroy(&c);

	CHECK(sink.destroyed);
	CHECK(c == nullptr);
}

TEST_CASE("BlobSinkBase: pointer recovery round trips", "[glue][blob_sink]")
{
	RecordingBlobSink sink;

	CHECK(RecordingBlobSink::fromBlobSink(sink.getBlobSink()) == &sink);

	const RecordingBlobSink &csink = sink;
	CHECK(RecordingBlobSink::fromBlobSink(csink.getBlobSink()) == &csink);
	CHECK(&sink.derived() == &sink);
	CHECK(&csink.derived() == &csink);
}

TEST_CASE("BlobSinkBase: separate sinks are independent", "[glue][blob_sink]")
{
	RecordingBlobSink a, b;
	t_blob_observation obs = makeObservation();

	t_blob_sink_push_blobs(a.getBlobSink(), &obs);

	CHECK(a.count == 1);
	CHECK(b.count == 0);
}

TEST_CASE("BlobSinkBase: exceptions are caught", "[glue][blob_sink]")
{
	ThrowingBlobSink sink;
	t_blob_observation obs = makeObservation();

	CHECK_NOTHROW(t_blob_sink_push_blobs(sink.getBlobSink(), &obs));
	CHECK(sink.count == 1);

	t_blob_sink *c = sink.getBlobSink();
	CHECK_NOTHROW(t_blob_sink_destroy(&c));
	CHECK(sink.destroy_count == 1);
}

TEST_CASE("BlobSinkBase: works alongside other glue bases", "[glue][blob_sink]")
{
	MultiBaseBlobSink sink;
	t_blob_observation obs = makeObservation();

	t_blob_sink_push_blobs(sink.getBlobSink(), &obs);
	CHECK(sink.count == 1);
	CHECK(MultiBaseBlobSink::fromBlobSink(sink.getBlobSink()) == &sink);
	CHECK(MultiBaseBlobSink::fromFrameNode(sink.getFrameNode()) == &sink);
}
