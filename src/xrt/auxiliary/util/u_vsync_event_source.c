// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Reusable single-producer, single-consumer base for xrt_vsync_source implementations.
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup aux_util
 */

#include "u_vsync_event_source.h"

static inline struct u_vsync_event_source *
u_vsync_event_source(struct xrt_vsync_source *xvs)
{
	return (struct u_vsync_event_source *)xvs;
}

/*!
 * Block until a new event is available, or until cancelled.
 *
 * Must be called from exactly one consumer thread.
 */
static xrt_result_t
u_vsync_event_source_wait(struct xrt_vsync_source *xvs, struct xrt_vsync_event *out_event)
{
	struct u_vsync_event_source *uvs = u_vsync_event_source(xvs);

	os_mutex_lock(&uvs->mutex);

	// Wait loop: handles spurious wakeups and checks cancellation.
	while (!uvs->has_event && !uvs->cancelled) {
		os_cond_wait(&uvs->cond, &uvs->mutex);
	}

	if (uvs->cancelled) {
		os_mutex_unlock(&uvs->mutex);
		return XRT_NO_MORE_VSYNC_EVENTS;
	}

	*out_event = uvs->event;
	uvs->has_event = false;

	os_mutex_unlock(&uvs->mutex);

	return XRT_SUCCESS;
}

/*!
 * Non-blocking, non-destructive read of the most recent event.
 *
 * Consumes nothing: repeated calls return the same event until the producer
 * pushes a new one, and an event not yet taken by wait() is still delivered
 * there afterwards unless cancelled. Returns XRT_VSYNC_EVENT_UNAVAILABLE until
 * the first event is pushed. Cancellation takes precedence and returns
 * XRT_NO_MORE_VSYNC_EVENTS even if a cached event exists.
 *
 * Safe to call from any thread.
 */
static xrt_result_t
u_vsync_event_source_peek_last(struct xrt_vsync_source *xvs, struct xrt_vsync_event *out_event)
{
	struct u_vsync_event_source *uvs = u_vsync_event_source(xvs);

	os_mutex_lock(&uvs->mutex);

	if (uvs->cancelled) {
		os_mutex_unlock(&uvs->mutex);
		return XRT_NO_MORE_VSYNC_EVENTS;
	}

	if (uvs->event.vsync_count == 0) {
		os_mutex_unlock(&uvs->mutex);
		return XRT_VSYNC_EVENT_UNAVAILABLE;
	}

	*out_event = uvs->event;

	os_mutex_unlock(&uvs->mutex);

	return XRT_SUCCESS;
}

void
u_vsync_event_source_push(struct u_vsync_event_source *uvs, const struct xrt_vsync_event *event)
{
	os_mutex_lock(&uvs->mutex);
	uvs->event = *event;
	uvs->has_event = true;
	os_cond_signal(&uvs->cond);
	os_mutex_unlock(&uvs->mutex);
}

void
u_vsync_event_source_cancel(struct u_vsync_event_source *uvs)
{
	os_mutex_lock(&uvs->mutex);
	uvs->cancelled = true;
	os_cond_signal(&uvs->cond);
	os_mutex_unlock(&uvs->mutex);
}

void
u_vsync_event_source_fini(struct u_vsync_event_source *uvs)
{
	assert(uvs->cancelled);
	os_cond_destroy(&uvs->cond);
	os_mutex_destroy(&uvs->mutex);
}

void
u_vsync_event_source_init(struct u_vsync_event_source *uvs, xrt_vsync_source_destroy_func_t destroy)
{
	uvs->base.wait_next = u_vsync_event_source_wait;
	uvs->base.peek_last = u_vsync_event_source_peek_last;
	uvs->base.destroy = destroy;
	os_mutex_init(&uvs->mutex);
	os_cond_init(&uvs->cond);
	uvs->has_event = false;
	uvs->cancelled = false;
	U_ZERO(&uvs->event);
}
