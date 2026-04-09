// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simple single producer-consumer sink for u_vsync_source backends.
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_vsync_source.h"

#include "os/os_threading.h"

#include "u_misc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct u_vsync_event_sink
{
	//! most recent event from the producer.
	struct xrt_vsync_event event;

	struct os_mutex mutex;
	struct os_cond cond;

	//! set when a new event has been pushed but not yet consumed.
	bool has_event;

	//! set by cancel operation to break a waiting operation.
	bool cancelled;
};

static inline void
u_vsync_event_sink_init(struct u_vsync_event_sink *sink)
{
	os_mutex_init(&sink->mutex);
	os_cond_init(&sink->cond);
	sink->has_event = false;
	sink->cancelled = false;
	U_ZERO(&sink->event);
}

/*!
 * Cancel the sink. Wakes any thread blocked in wait(), which will
 * return XRT_OPERATION_CANCELLED. Idempotent — safe to call multiple times.
 *
 * Safe to call from any thread.
 */
static inline void
u_vsync_event_sink_cancel(struct u_vsync_event_sink *sink)
{
	os_mutex_lock(&sink->mutex);
	sink->cancelled = true;
	os_cond_signal(&sink->cond);
	os_mutex_unlock(&sink->mutex);
}

static inline void
u_vsync_event_sink_fini(struct u_vsync_event_sink *sink)
{
	assert(sink->cancelled);
	os_cond_destroy(&sink->cond);
	os_mutex_destroy(&sink->mutex);
}

/*!
 * Push a new vsync event. Called by the backend's producer thread
 * when a hardware vsync fires. Wakes the consumer if it's blocked
 * in wait().
 *
 * Safe to call from any thread.
 */
static inline void
u_vsync_event_sink_push(struct u_vsync_event_sink *sink, const struct xrt_vsync_event *event)
{
	os_mutex_lock(&sink->mutex);
	sink->event = *event;
	sink->has_event = true;
	os_cond_signal(&sink->cond);
	os_mutex_unlock(&sink->mutex);
}

/*!
 * Block until a new event is available, or until cancelled.
 *
 * Must be called from exactly one consumer thread.
 */
static inline xrt_result_t
u_vsync_event_sink_wait(struct u_vsync_event_sink *sink, struct xrt_vsync_event *out_event)
{
	os_mutex_lock(&sink->mutex);

	// Wait loop: handles spurious wakeups and checks cancellation.
	while (!sink->has_event && !sink->cancelled) {
		os_cond_wait(&sink->cond, &sink->mutex);
	}

	if (sink->cancelled) {
		os_mutex_unlock(&sink->mutex);
		return XRT_OPERATION_CANCELLED;
	}

	*out_event = sink->event;
	sink->has_event = false;

	os_mutex_unlock(&sink->mutex);

	return XRT_SUCCESS;
}

static inline xrt_result_t
u_vsync_event_sink_peek_last(struct u_vsync_event_sink *sink, struct xrt_vsync_event *out_event)
{
	os_mutex_lock(&sink->mutex);

	if (sink->event.vsync_count == 0) {
		os_mutex_unlock(&sink->mutex);
		return XRT_TIMEOUT;
	}

	*out_event = sink->event;

	os_mutex_unlock(&sink->mutex);

	return XRT_SUCCESS;
}

#ifdef __cplusplus
} // extern "C"
#endif
