// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header defining an xrt vsync source.
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Vsync event information output by a vsync source.
 */
struct xrt_vsync_event
{
	/*!
	 * Monotonic timestamp of the most recent vsync (past event).
	 * The next vsync is expected at `timestamp_ns + period_ns`.
	 */
	int64_t timestamp_ns;

	//! Interval between the two most recent vsyncs, tracks the display period.
	int64_t period_ns;

	/*!
	 * Total number of vsyncs that have occurred.
	 *
	 * let diff_vsyncs = vsync_count - last_vsync_count
	 *
	 * if (diff_vsyncs > 1) then diff_vsyncs missed vsync(s)
	 */
	uint64_t vsync_count;
};

/*!
 * @brief Vsync source interface.
 */
struct xrt_vsync_source
{
	/*!
	 * @brief Block until the next vsync event.
	 *
	 * @param[in] xvs          The vsync source.
	 * @param[out] out_event   The vsync event result.
	 *
	 * @return
	 * - @p XRT_SUCCESS when a new event was produced.
	 * - @p XRT_NO_MORE_VSYNC_EVENTS when the source is being torn down.
	 */
	xrt_result_t (*wait_next)(struct xrt_vsync_source *xvs, struct xrt_vsync_event *out_event);

	/*!
	 * @brief Non-blocking, non-destructive read of the most recent vsync event.
	 *
	 * Samples the latest event produced by the source; nothing is consumed.
	 * This never affects @ref wait_next: an undelivered event is still handed to
	 * @ref wait_next afterwards unless cancelled, and events already delivered
	 * through @ref wait_next keep being reported here until cancellation.
	 *
	 * The same event is returned on every call until the source produces a new
	 * one, so compare @ref xrt_vsync_event::vsync_count against the previously
	 * seen value to tell a new event apart from a repeat of the last one. Not
	 * every observed vsync produces an event, an implementation may use one to
	 * establish the display period instead.
	 *
	 * @param[in] xvs          The vsync source.
	 * @param[out] out_event   The latest vsync event result, written only on @p XRT_SUCCESS.
	 *
	 * @return
	 * - @p XRT_SUCCESS when the source has produced at least one event and has not been cancelled.
	 * - @p XRT_VSYNC_EVENT_UNAVAILABLE when no event has been produced yet and the source is not cancelled.
	 * - @p XRT_NO_MORE_VSYNC_EVENTS when the source has been cancelled, even if a cached event exists.
	 */
	xrt_result_t (*peek_last)(struct xrt_vsync_source *xvs, struct xrt_vsync_event *out_event);

	void (*destroy)(struct xrt_vsync_source *xvs);
};

/*!
 * Helper function for @ref xrt_vsync_source::wait_next.
 *
 * @copydoc xrt_vsync_source::wait_next
 *
 * @public @memberof xrt_vsync_source
 */
XRT_NONNULL_ALL static inline xrt_result_t
xrt_vsync_source_wait_next(struct xrt_vsync_source *xvs, struct xrt_vsync_event *out_event)
{
	return xvs->wait_next(xvs, out_event);
}

/*!
 * Helper function for @ref xrt_vsync_source::peek_last.
 *
 * @copydoc xrt_vsync_source::peek_last
 *
 * @public @memberof xrt_vsync_source
 */
XRT_NONNULL_ALL static inline xrt_result_t
xrt_vsync_source_peek_last(struct xrt_vsync_source *xvs, struct xrt_vsync_event *out_event)
{
	return xvs->peek_last(xvs, out_event);
}

/*!
 * Helper function for @ref xrt_vsync_source::destroy.
 *
 * Handles nulls, sets your pointer to null.
 *
 * @public @memberof xrt_vsync_source
 */
XRT_NONNULL_ALL static inline void
xrt_vsync_source_destroy(struct xrt_vsync_source **xvs_ptr)
{
	struct xrt_vsync_source *xvs = *xvs_ptr;
	if (xvs == NULL) {
		return;
	}

	xvs->destroy(xvs);
	*xvs_ptr = NULL;
}

#ifdef __cplusplus
} // extern "C"
#endif
