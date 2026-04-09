// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Reusable single-producer, single-consumer base for xrt_vsync_source implementations.
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_vsync_source.h"

#include "os/os_threading.h"

#include "u_misc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * A basic single-producer, single-consumer base for @ref xrt_vsync_source
 * implementations.
 *
 * Implements @ref xrt_vsync_source::wait_next and @ref xrt_vsync_source::peek_last,
 * the derived implementation supplies the destroy function and pushes events with
 * @ref u_vsync_event_source_push.
 *
 * Deriving from it is a convenience and not required for xrt_vsync_source
 * implementations, but they may choose to do so in cases where vsync signals come
 * from a different thread.
 *
 * Holds only the most recent event, not a queue of them.
 *
 * @ingroup aux_util
 */
struct u_vsync_event_source
{
	struct xrt_vsync_source base;

	//! most recent event from the producer.
	struct xrt_vsync_event event;

	struct os_mutex mutex;
	struct os_cond cond;

	//! set when a new event has been pushed but not yet consumed.
	bool has_event;

	/*!
	 * Set by cancel operation to break a waiting operation, and all future operations.
	 * Once set will never be cleared for the lifetime of this vsync object.
	 */
	bool cancelled;
};

typedef void (*xrt_vsync_source_destroy_func_t)(struct xrt_vsync_source *xvs);

void
u_vsync_event_source_init(struct u_vsync_event_source *uvs, xrt_vsync_source_destroy_func_t destroy);

void
u_vsync_event_source_fini(struct u_vsync_event_source *uvs);

/*!
 * Cancel the source. Wakes any thread blocked in wait(), which will
 * return XRT_NO_MORE_VSYNC_EVENTS. Idempotent — safe to call multiple times.
 *
 * Safe to call from any thread.
 */
void
u_vsync_event_source_cancel(struct u_vsync_event_source *uvs);

/*!
 * Push a new vsync event. Called by the backend's producer thread
 * when a hardware vsync fires. Wakes the consumer if it's blocked
 * in wait().
 *
 * The source holds only the latest event: pushing over an event that wait() has
 * not consumed yet overwrites it, and the skipped vsync shows up as a gap in
 * @ref xrt_vsync_event::vsync_count rather than as a queued event.
 *
 * @p event->vsync_count must be non-zero, peek_last() reads zero as
 * "nothing observed yet".
 *
 * Safe to call from any thread.
 */
void
u_vsync_event_source_push(struct u_vsync_event_source *uvs, const struct xrt_vsync_event *event);

#ifdef __cplusplus
} // extern "C"
#endif
