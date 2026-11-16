// Copyright 2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Frame pacing tests.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 */

#include <util/u_pacing.h>

#include "catch_amalgamated.hpp"

#include "time_utils.hpp"

#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <queue>

namespace {

//! Test helper: begin a frame and predict it in one go.
void
u_pc_predict(u_pacing_compositor *upc,
             int64_t now_ns,
             int64_t *out_frame_id,
             int64_t *out_wake_up_time_ns,
             int64_t *out_desired_present_time_ns,
             int64_t *out_present_slop_ns,
             int64_t *out_predicted_display_time_ns,
             int64_t *out_predicted_display_period_ns,
             int64_t *out_min_display_period_ns)
{
	u_pc_allocate_frame(upc, out_frame_id);
	u_pc_predict_frame(upc, *out_frame_id, now_ns, out_wake_up_time_ns, out_desired_present_time_ns,
	                   out_present_slop_ns, out_predicted_display_time_ns, out_predicted_display_period_ns,
	                   out_min_display_period_ns);
}

} // namespace

using namespace std::chrono_literals;
using namespace std::chrono;

static constexpr unanoseconds frame_interval_ns(16ms);

namespace {

int64_t
getNextPresentAfterTimestampAndKnownPresent(int64_t timestamp_ns, int64_t known_present_ns)
{

	while (known_present_ns < timestamp_ns) {
		known_present_ns += frame_interval_ns.count();
	}
	return known_present_ns;
}
int64_t
getPresentBefore(int64_t timestamp_ns, int64_t known_present_ns)
{

	while (known_present_ns >= timestamp_ns && known_present_ns > frame_interval_ns.count()) {
		known_present_ns -= frame_interval_ns.count();
	}
	return known_present_ns;
}
int64_t
getNextPresentAfterTimestamp(int64_t timestamp_ns, int64_t known_present_ns)
{
	auto present_before_ns = getPresentBefore(timestamp_ns, known_present_ns);
	return getNextPresentAfterTimestampAndKnownPresent(timestamp_ns, present_before_ns);
}

struct CompositorPredictions
{
	int64_t frame_id{0};
	int64_t wake_up_time_ns{0};
	int64_t desired_present_time_ns{0};
	int64_t present_slop_ns{0};
	int64_t predicted_display_time_ns{0};
	int64_t predicted_display_period_ns{0};
	int64_t min_display_period_ns{0};
};
} // namespace

static void
basicPredictionConsistencyChecks(int64_t now_ns, CompositorPredictions const &predictions)
{
	INFO(predictions.frame_id);
	INFO(now_ns);
	CHECK(predictions.wake_up_time_ns >= now_ns);
	CHECK(predictions.desired_present_time_ns > now_ns);
	CHECK(predictions.desired_present_time_ns > predictions.wake_up_time_ns);
	CHECK(predictions.predicted_display_time_ns > now_ns);
	CHECK(predictions.predicted_display_time_ns > predictions.desired_present_time_ns);
	// display period predicted to be +- 2ms (arbitrary)
	CHECK(unanoseconds(predictions.predicted_display_period_ns) < (frame_interval_ns + unanoseconds(2ms)));
	CHECK(unanoseconds(predictions.predicted_display_period_ns) > (frame_interval_ns - unanoseconds(2ms)));
}

struct SimulatedDisplayTimingData
{
	SimulatedDisplayTimingData(int64_t id, int64_t desired_present_time, int64_t gpu_finish, int64_t now)
	    : frame_id(id), desired_present_time_ns(desired_present_time),
	      actual_present_time_ns(getNextPresentAfterTimestampAndKnownPresent(gpu_finish, desired_present_time)),
	      earliest_present_time_ns(getNextPresentAfterTimestamp(gpu_finish, desired_present_time)),
	      present_margin_ns(earliest_present_time_ns - gpu_finish), now_ns(now)
	{}

	int64_t frame_id;
	int64_t desired_present_time_ns;
	int64_t actual_present_time_ns;
	int64_t earliest_present_time_ns;
	int64_t present_margin_ns;
	int64_t now_ns;
	void
	call_u_pc_info(u_pacing_compositor *upc) const
	{
		std::cout << "frame_id:                 " << frame_id << '\n';
		std::cout << "desired_present_time_ns:  " << desired_present_time_ns << '\n';
		std::cout << "actual_present_time_ns:   " << actual_present_time_ns << '\n';
		std::cout << "earliest_present_time_ns: " << earliest_present_time_ns << '\n';
		std::cout << "present_margin_ns:        " << present_margin_ns << '\n';
		std::cout << "now_ns:                   " << now_ns << "\n" << '\n';
		u_pc_info(upc, frame_id, desired_present_time_ns, actual_present_time_ns, earliest_present_time_ns,
		          present_margin_ns, now_ns);
	}
};
static inline bool
operator>(SimulatedDisplayTimingData const &lhs, SimulatedDisplayTimingData const &rhs)
{
	return lhs.now_ns > rhs.now_ns;
}
using SimulatedDisplayTimingQueue = std::priority_queue<SimulatedDisplayTimingData,
                                                        std::vector<SimulatedDisplayTimingData>,
                                                        std::greater<SimulatedDisplayTimingData>>;

//! Process all simulated timing data in the queue that should be processed by now.
static void
processDisplayTimingQueue(SimulatedDisplayTimingQueue &display_timing_queue, int64_t now_ns, u_pacing_compositor *upc)
{
	while (!display_timing_queue.empty() && display_timing_queue.top().now_ns <= now_ns) {
		display_timing_queue.top().call_u_pc_info(upc);
		display_timing_queue.pop();
	}
}
//! Process all remaining simulated timing data in the queue and return the timestamp of the last one.
static int64_t
drainDisplayTimingQueue(SimulatedDisplayTimingQueue &display_timing_queue, int64_t now_ns, u_pacing_compositor *upc)
{
	while (!display_timing_queue.empty()) {
		now_ns = display_timing_queue.top().now_ns;
		display_timing_queue.top().call_u_pc_info(upc);
		display_timing_queue.pop();
	}
	return now_ns;
}

static void
doFrame(SimulatedDisplayTimingQueue &display_timing_queue,
        u_pacing_compositor *upc,
        MockClock &clock,
        int64_t wake_time_ns,
        int64_t desired_present_time_ns,
        int64_t frame_id,
        unanoseconds wake_delay,
        unanoseconds begin_delay,
        unanoseconds draw_delay,
        unanoseconds submit_delay,
        unanoseconds gpu_time_after_submit)
{
	REQUIRE(clock.now() <= wake_time_ns);
	// wake up (after delay)
	clock.advance_to(wake_time_ns);
	clock.advance(wake_delay);
	processDisplayTimingQueue(display_timing_queue, clock.now(), upc);
	u_pc_mark_point(upc, U_TIMING_POINT_WAKE_UP, frame_id, clock.now());

	// begin (after delay)
	clock.advance(begin_delay);
	processDisplayTimingQueue(display_timing_queue, clock.now(), upc);
	u_pc_mark_point(upc, U_TIMING_POINT_BEGIN, frame_id, clock.now());

	// spend cpu time drawing
	clock.advance(draw_delay);
	u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, clock.now());

	// spend cpu time before submit
	clock.advance(submit_delay);
	processDisplayTimingQueue(display_timing_queue, clock.now(), upc);
	u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_END, frame_id, clock.now());

	// spend gpu time before present
	clock.advance(gpu_time_after_submit);
	auto gpu_finish = clock.now();
	auto next_scanout_timepoint = getNextPresentAfterTimestampAndKnownPresent(gpu_finish, desired_present_time_ns);

	REQUIRE(next_scanout_timepoint >= gpu_finish);

	// our wisdom arrives after scanout
	MockClock infoClock;
	infoClock.advance_to(next_scanout_timepoint);
	infoClock.advance(1ms);
	display_timing_queue.push({frame_id, desired_present_time_ns, gpu_finish, infoClock.now()});
}

// u_pc is for the compositor, we should take way less than a frame to do our job.
static constexpr auto wakeDelay = microseconds(20);

static constexpr auto shortBeginDelay = microseconds(20);
static constexpr auto shortDrawDelay = 150us;
static constexpr auto shortSubmitDelay = 50us;
static constexpr auto shortGpuTime = 1ms;


static constexpr auto longBeginDelay = 1ms;
static constexpr auto longDrawDelay = 2ms;
static constexpr auto longGpuTime = 2ms;

TEST_CASE("u_pacing_compositor_display_timing")
{
	struct u_pc_display_timing_config config = u_pc_display_timing_get_default_config();
	u_pacing_compositor *upc = nullptr;
	MockClock clock;
	REQUIRE(XRT_SUCCESS == u_pc_display_timing_create(frame_interval_ns.count(), &config, &upc));
	REQUIRE(upc != nullptr);

	clock.advance(1ms);

	CompositorPredictions predictions;
	u_pc_predict(upc, clock.now(), &predictions.frame_id, &predictions.wake_up_time_ns,
	             &predictions.desired_present_time_ns, &predictions.present_slop_ns,
	             &predictions.predicted_display_time_ns, &predictions.predicted_display_period_ns,
	             &predictions.min_display_period_ns);
	basicPredictionConsistencyChecks(clock.now(), predictions);

	auto frame_id = predictions.frame_id;
	SimulatedDisplayTimingQueue queue;


	SECTION("faster than expected")
	{
		// We have a 16ms period
		// wake promptly
		clock.advance(wakeDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_WAKE_UP, frame_id, clock.now());

		// start promptly
		clock.advance(shortBeginDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_BEGIN, frame_id, clock.now());

		// spend cpu time drawing
		clock.advance(shortDrawDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, clock.now());

		// spend a little cpu time submitting the work to the GPU
		clock.advance(shortSubmitDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_END, frame_id, clock.now());

		// spend time in gpu rendering until present
		clock.advance(shortGpuTime);
		auto gpu_finish = clock.now();

		auto next_scanout_timepoint =
		    getNextPresentAfterTimestampAndKnownPresent(gpu_finish, predictions.desired_present_time_ns);

		// our wisdom arrives after scanout
		MockClock infoClock;
		infoClock.advance_to(next_scanout_timepoint);
		infoClock.advance(1ms);
		queue.push({frame_id, predictions.desired_present_time_ns, gpu_finish, infoClock.now()});

		// Do basically the same thing a few more frames.
		for (int i = 0; i < 20; ++i) {
			CompositorPredictions loopPred;
			u_pc_predict(upc, clock.now(), &loopPred.frame_id, &loopPred.wake_up_time_ns,
			             &loopPred.desired_present_time_ns, &loopPred.present_slop_ns,
			             &loopPred.predicted_display_time_ns, &loopPred.predicted_display_period_ns,
			             &loopPred.min_display_period_ns);
			CHECK(loopPred.frame_id > i);
			INFO("frame id" << loopPred.frame_id);
			INFO(clock.now());
			basicPredictionConsistencyChecks(clock.now(), loopPred);
			doFrame(queue, upc, clock, loopPred.wake_up_time_ns, loopPred.desired_present_time_ns,
			        loopPred.frame_id, wakeDelay, shortBeginDelay, shortDrawDelay, shortSubmitDelay,
			        shortGpuTime);
		}
		// we should now get a shorter time before present to wake up.
		CompositorPredictions newPred;
		u_pc_predict(upc, clock.now(), &newPred.frame_id, &newPred.wake_up_time_ns,
		             &newPred.desired_present_time_ns, &newPred.present_slop_ns,
		             &newPred.predicted_display_time_ns, &newPred.predicted_display_period_ns,
		             &newPred.min_display_period_ns);
		basicPredictionConsistencyChecks(clock.now(), newPred);
		CHECK(unanoseconds(newPred.desired_present_time_ns - newPred.wake_up_time_ns) <
		      unanoseconds(predictions.desired_present_time_ns - predictions.wake_up_time_ns));
		CHECK(unanoseconds(newPred.desired_present_time_ns - newPred.wake_up_time_ns) >
		      unanoseconds(shortDrawDelay + shortSubmitDelay + shortGpuTime));
	}

	SECTION("slower than desired")
	{
		// We have a 16ms period
		// wake promptly
		clock.advance(wakeDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_WAKE_UP, frame_id, clock.now());

		// waste time before beginframe
		clock.advance(longBeginDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_BEGIN, frame_id, clock.now());

		// spend cpu time drawing
		clock.advance(longDrawDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, clock.now());

		// spend a little cpu time submitting the work to the GPU
		clock.advance(shortSubmitDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_END, frame_id, clock.now());

		// spend time in gpu rendering until present
		clock.advance(longGpuTime);
		auto gpu_finish = clock.now();

		auto next_scanout_timepoint =
		    getNextPresentAfterTimestampAndKnownPresent(gpu_finish, predictions.desired_present_time_ns);

		REQUIRE(next_scanout_timepoint > gpu_finish);


		// our wisdom arrives after scanout
		MockClock infoClock;
		infoClock.advance_to(next_scanout_timepoint);
		infoClock.advance(1ms);
		queue.push({frame_id, predictions.desired_present_time_ns, gpu_finish, infoClock.now()});

		// Do basically the same thing a few more frames.
		for (int i = 0; i < 50; ++i) {
			CompositorPredictions loopPred;
			u_pc_predict(upc, clock.now(), &loopPred.frame_id, &loopPred.wake_up_time_ns,
			             &loopPred.desired_present_time_ns, &loopPred.present_slop_ns,
			             &loopPred.predicted_display_time_ns, &loopPred.predicted_display_period_ns,
			             &loopPred.min_display_period_ns);
			INFO(loopPred.frame_id);
			INFO(clock.now());
			basicPredictionConsistencyChecks(clock.now(), loopPred);
			doFrame(queue, upc, clock, loopPred.wake_up_time_ns, loopPred.desired_present_time_ns,
			        loopPred.frame_id, wakeDelay, longBeginDelay, longDrawDelay, shortSubmitDelay,
			        longGpuTime);
		}

		// we should now get a bigger time before present to wake up.
		CompositorPredictions newPred;
		u_pc_predict(upc, clock.now(), &newPred.frame_id, &newPred.wake_up_time_ns,
		             &newPred.desired_present_time_ns, &newPred.present_slop_ns,
		             &newPred.predicted_display_time_ns, &newPred.predicted_display_period_ns,
		             &newPred.min_display_period_ns);
		basicPredictionConsistencyChecks(clock.now(), newPred);
		CHECK(unanoseconds(newPred.desired_present_time_ns - newPred.wake_up_time_ns) >
		      unanoseconds(longBeginDelay + longDrawDelay + shortSubmitDelay + longGpuTime));
	}

	u_pc_destroy(&upc);
}

TEST_CASE("u_pacing_compositor_fake")
{
	MockClock clock;
	u_pacing_compositor *upc = nullptr;
	REQUIRE(XRT_SUCCESS == u_pc_fake_create(frame_interval_ns.count(), clock.now(), &upc));
	REQUIRE(upc != nullptr);

	clock.advance(1ms);

	SECTION("Standalone predictions")
	{
		CompositorPredictions predictions;
		u_pc_predict(upc, clock.now(), &predictions.frame_id, &predictions.wake_up_time_ns,
		             &predictions.desired_present_time_ns, &predictions.present_slop_ns,
		             &predictions.predicted_display_time_ns, &predictions.predicted_display_period_ns,
		             &predictions.min_display_period_ns);
		basicPredictionConsistencyChecks(clock.now(), predictions);
	}
	SECTION("Consistency in loop")
	{
		SimulatedDisplayTimingQueue queue;
		SECTION("Fast")
		{

			for (int i = 0; i < 10; ++i) {
				CompositorPredictions predictions;
				u_pc_predict(upc, clock.now(), &predictions.frame_id, &predictions.wake_up_time_ns,
				             &predictions.desired_present_time_ns, &predictions.present_slop_ns,
				             &predictions.predicted_display_time_ns,
				             &predictions.predicted_display_period_ns,
				             &predictions.min_display_period_ns);
				INFO(predictions.frame_id);
				INFO(clock.now());
				basicPredictionConsistencyChecks(clock.now(), predictions);
				doFrame(queue, upc, clock, predictions.wake_up_time_ns,
				        predictions.desired_present_time_ns, predictions.frame_id, wakeDelay,
				        shortBeginDelay, shortDrawDelay, shortSubmitDelay, shortGpuTime);
			}
			drainDisplayTimingQueue(queue, clock.now(), upc);
		}
		SECTION("Slow")
		{
			for (int i = 0; i < 10; ++i) {
				CompositorPredictions predictions;
				u_pc_predict(upc, clock.now(), &predictions.frame_id, &predictions.wake_up_time_ns,
				             &predictions.desired_present_time_ns, &predictions.present_slop_ns,
				             &predictions.predicted_display_time_ns,
				             &predictions.predicted_display_period_ns,
				             &predictions.min_display_period_ns);
				INFO(predictions.frame_id);
				INFO(clock.now());
				basicPredictionConsistencyChecks(clock.now(), predictions);
				doFrame(queue, upc, clock, predictions.wake_up_time_ns,
				        predictions.desired_present_time_ns, predictions.frame_id, wakeDelay,
				        longBeginDelay, longDrawDelay, shortSubmitDelay, longGpuTime);
			}
			drainDisplayTimingQueue(queue, clock.now(), upc);
		}
	}
	u_pc_destroy(&upc);
}

namespace {

enum class PacerKind
{
	Fake,
	DisplayTiming,
};

u_pacing_compositor *
createPacer(PacerKind kind, int64_t now_ns)
{
	u_pacing_compositor *upc = nullptr;
	switch (kind) {
	case PacerKind::Fake: REQUIRE(XRT_SUCCESS == u_pc_fake_create(frame_interval_ns.count(), now_ns, &upc)); break;
	case PacerKind::DisplayTiming: {
		u_pc_display_timing_config config = u_pc_display_timing_get_default_config();
		REQUIRE(XRT_SUCCESS == u_pc_display_timing_create(frame_interval_ns.count(), &config, &upc));
		break;
	}
	}
	REQUIRE(upc != nullptr);
	return upc;
}

//! Begin a frame, optionally update the present offset for it, then predict it.
CompositorPredictions
beginUpdateAndPredict(u_pacing_compositor *upc, int64_t now_ns, const int64_t *present_to_display_offset_ns)
{
	CompositorPredictions p;
	u_pc_allocate_frame(upc, &p.frame_id);
	if (present_to_display_offset_ns != nullptr) {
		u_pc_update_present_offset(upc, p.frame_id, *present_to_display_offset_ns);
	}
	u_pc_predict_frame(upc, p.frame_id, now_ns, &p.wake_up_time_ns, &p.desired_present_time_ns, &p.present_slop_ns,
	                   &p.predicted_display_time_ns, &p.predicted_display_period_ns, &p.min_display_period_ns);
	return p;
}

int64_t
displayMinusPresent(const CompositorPredictions &p)
{
	return p.predicted_display_time_ns - p.desired_present_time_ns;
}

} // namespace

TEST_CASE("u_pacing_compositor begin and predict are separate")
{
	const PacerKind kind = GENERATE(PacerKind::Fake, PacerKind::DisplayTiming);
	INFO((kind == PacerKind::Fake ? "fake" : "display timing"));

	MockClock clock;
	u_pacing_compositor *upc = createPacer(kind, clock.now());
	clock.advance(1ms);

	int64_t first_id = -1;
	int64_t second_id = -1;
	u_pc_allocate_frame(upc, &first_id);
	CHECK(first_id >= 0);

	// Beginning a frame must not predict, a second begin gives a new id.
	u_pc_allocate_frame(upc, &second_id);
	CHECK(second_id != first_id);

	// Frames can be predicted in order once begun.
	CompositorPredictions p;
	u_pc_predict_frame(upc, first_id, clock.now(), &p.wake_up_time_ns, &p.desired_present_time_ns,
	                   &p.present_slop_ns, &p.predicted_display_time_ns, &p.predicted_display_period_ns,
	                   &p.min_display_period_ns);
	p.frame_id = first_id;
	basicPredictionConsistencyChecks(clock.now(), p);

	u_pc_destroy(&upc);
}

TEST_CASE("u_pacing_compositor update present offset")
{
	const PacerKind kind = GENERATE(PacerKind::Fake, PacerKind::DisplayTiming);
	INFO((kind == PacerKind::Fake ? "fake" : "display timing"));

	MockClock clock;
	u_pacing_compositor *upc = createPacer(kind, clock.now());
	clock.advance(1ms);

	SECTION("Offset set between begin and predict is used")
	{
		const int64_t offset_ns = unanoseconds(7ms).count();
		CompositorPredictions p = beginUpdateAndPredict(upc, clock.now(), &offset_ns);
		CHECK(displayMinusPresent(p) == offset_ns);
	}
	SECTION("Small positive offset is used")
	{
		const int64_t offset_ns = unanoseconds(500us).count();
		CompositorPredictions p = beginUpdateAndPredict(upc, clock.now(), &offset_ns);
		CHECK(displayMinusPresent(p) == offset_ns);
	}
	SECTION("Zero offset is used")
	{
		const int64_t offset_ns = 0;
		CompositorPredictions p = beginUpdateAndPredict(upc, clock.now(), &offset_ns);
		CHECK(displayMinusPresent(p) == 0);
	}
	SECTION("Offset persists to later frames and can be changed")
	{
		int64_t offset_ns = unanoseconds(5ms).count();
		CompositorPredictions p = beginUpdateAndPredict(upc, clock.now(), &offset_ns);
		CHECK(displayMinusPresent(p) == offset_ns);

		// No update, the previous value stays.
		clock.advance(frame_interval_ns);
		p = beginUpdateAndPredict(upc, clock.now(), nullptr);
		CHECK(displayMinusPresent(p) == offset_ns);

		offset_ns = unanoseconds(8ms).count();
		clock.advance(frame_interval_ns);
		p = beginUpdateAndPredict(upc, clock.now(), &offset_ns);
		CHECK(displayMinusPresent(p) == offset_ns);
	}

	u_pc_destroy(&upc);
}

TEST_CASE("u_pacing_compositor default present to display offset")
{
	const PacerKind kind = GENERATE(PacerKind::Fake, PacerKind::DisplayTiming);
	INFO((kind == PacerKind::Fake ? "fake" : "display timing"));

	MockClock clock;
	u_pacing_compositor *upc = createPacer(kind, clock.now());
	clock.advance(1ms);

	CompositorPredictions p = beginUpdateAndPredict(upc, clock.now(), nullptr);
	CHECK(displayMinusPresent(p) == unanoseconds(4ms).count());

	u_pc_destroy(&upc);
}

TEST_CASE("u_pacing_compositor_display_timing keeps cadence without present info")
{
	// Regression test: when no frame has gotten present timing info yet (or a driver never provides any),
	// consecutive predictions must still be spaced exactly one frame period apart, a non-zero present to display
	// offset must not leak into the present time cadence.
	const int64_t offset_ns = unanoseconds(4ms).count();

	u_pc_display_timing_config config = u_pc_display_timing_get_default_config();
	config.present_to_display_offset_ns = offset_ns;

	MockClock clock;
	u_pacing_compositor *upc = nullptr;
	REQUIRE(XRT_SUCCESS == u_pc_display_timing_create(frame_interval_ns.count(), &config, &upc));
	REQUIRE(upc != nullptr);
	clock.advance(1ms);

	int64_t last_desired_present_time_ns = 0;
	for (int i = 0; i < 8; ++i) {
		CompositorPredictions p = beginUpdateAndPredict(upc, clock.now(), nullptr);
		INFO("frame " << p.frame_id);
		basicPredictionConsistencyChecks(clock.now(), p);
		CHECK(displayMinusPresent(p) == offset_ns);
		if (i > 0) {
			CHECK(p.desired_present_time_ns - last_desired_present_time_ns == frame_interval_ns.count());
		}
		last_desired_present_time_ns = p.desired_present_time_ns;

		// Sleep until wake up, render and submit, but never report any present timing info back.
		clock.advance_to(p.wake_up_time_ns);
		u_pc_mark_point(upc, U_TIMING_POINT_WAKE_UP, p.frame_id, clock.now());
		u_pc_mark_point(upc, U_TIMING_POINT_BEGIN, p.frame_id, clock.now());
		clock.advance(shortDrawDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_BEGIN, p.frame_id, clock.now());
		clock.advance(shortSubmitDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_END, p.frame_id, clock.now());
	}

	u_pc_destroy(&upc);
}
