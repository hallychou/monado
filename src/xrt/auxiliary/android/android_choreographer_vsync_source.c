// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  AChoreographer based vsync source implementation
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup aux_android
 */
#include "xrt/xrt_vsync_source.h"

#include "os/os_threading.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_trace_marker.h"
#include "util/u_vsync_event_source.h"
#include "util/u_thread_priority.h"

#include "android/android_custom_surface.h"
#include "android/android_globals.h"

#include <errno.h>
#include <android/looper.h>
#include <android/choreographer.h>


struct android_choreographer_vsync_source
{
	struct u_vsync_event_source base;

	struct os_thread_helper looper_thread;

	struct
	{
		struct os_mutex mutex;
		struct os_cond cond;
		bool ready;
	} looper_signal;

	ALooper *looper;

	int64_t app_vsync_offset_ns;

	uint64_t vsync_count;

	int64_t last_timestamp_ns;
};

static inline struct android_choreographer_vsync_source *
android_choreographer_vsync_source(struct xrt_vsync_source *xvs)
{
	return (struct android_choreographer_vsync_source *)xvs;
}

static void
post_vsync_callback(struct android_choreographer_vsync_source *avs, AChoreographer *choreographer);

static inline void
set_and_signal_ready(struct android_choreographer_vsync_source *avs, ALooper *looper)
{
	os_mutex_lock(&avs->looper_signal.mutex);
	avs->looper = looper;
	avs->looper_signal.ready = true;
	os_cond_signal(&avs->looper_signal.cond);
	os_mutex_unlock(&avs->looper_signal.mutex);
}

static inline int
wait_for_looper_ready(struct android_choreographer_vsync_source *avs, int64_t timeout_ns)
{
	struct timespec timeout_ts = {0};
	if (os_semaphore_get_realtime_clock(&timeout_ts, timeout_ns) < 0) {
		return ETIMEDOUT;
	}

	int wait_ret = 0;
	os_mutex_lock(&avs->looper_signal.mutex);
	while (!avs->looper_signal.ready) {
		wait_ret = pthread_cond_timedwait(&avs->looper_signal.cond.cond,   //
		                                  &avs->looper_signal.mutex.mutex, //
		                                  &timeout_ts);
		if (wait_ret == ETIMEDOUT) {
			break;
		}
	}
	os_mutex_unlock(&avs->looper_signal.mutex);

	return wait_ret;
}

static inline void
wake_looper_thread(struct android_choreographer_vsync_source *avs)
{
	ALooper *looper = NULL;
	os_mutex_lock(&avs->looper_signal.mutex);
	looper = avs->looper;
	os_mutex_unlock(&avs->looper_signal.mutex);

	if (looper != NULL) {
		ALooper_wake(looper);
	}
}

static void
push_vsync_event(struct android_choreographer_vsync_source *avs, int64_t timestamp_ns)
{
	if (avs->last_timestamp_ns == 0) {
		avs->last_timestamp_ns = timestamp_ns;
		return;
	}

	const int64_t period_ns = timestamp_ns - avs->last_timestamp_ns;
	assert(period_ns > 0);
	avs->last_timestamp_ns = timestamp_ns;

	const int64_t hw_vsync_ns = timestamp_ns - avs->app_vsync_offset_ns;

	struct xrt_vsync_event event = {
	    .timestamp_ns = hw_vsync_ns,
	    .period_ns = period_ns,
	    .vsync_count = ++avs->vsync_count,
	};
	u_vsync_event_source_push(&avs->base, &event);
}

static inline void
choreographer_on_vsync(
#if (__ANDROID_API__ >= 33)
    const AChoreographerFrameCallbackData *data,
#elif (__ANDROID_API__ >= 29)
    int64_t frame_time_ns,
#else //! (__ANDROID_API__ < 29)
    long frame_time_ns,
#endif
    void *ptr)
{
	struct android_choreographer_vsync_source *avs = (struct android_choreographer_vsync_source *)ptr;

#if (__ANDROID_API__ >= 33)
	const int64_t frame_time_ns = AChoreographerFrameCallbackData_getFrameTimeNanos(data);
	push_vsync_event(avs, frame_time_ns);

#elif (__ANDROID_API__ >= 29)
	push_vsync_event(avs, frame_time_ns);
#else //! (__ANDROID_API__ < 29)
	push_vsync_event(avs, (int64_t)frame_time_ns);
#endif

	post_vsync_callback(avs, AChoreographer_getInstance());
}

static void
post_vsync_callback(struct android_choreographer_vsync_source *avs, AChoreographer *choreographer)
{
#if (__ANDROID_API__ >= 33)
	AChoreographer_postVsyncCallback(choreographer, choreographer_on_vsync, avs);
#elif (__ANDROID_API__ >= 29)
	AChoreographer_postFrameCallback64(choreographer, choreographer_on_vsync, avs);
#else //! (__ANDROID_API__ < 29)
	AChoreographer_postFrameCallback(choreographer, choreographer_on_vsync, avs);
#endif
}

static void *
vsync_looper_thread(void *ctx)
{
	struct android_choreographer_vsync_source *avs = (struct android_choreographer_vsync_source *)ctx;

	const char *thread_name = "android_choreographer_vsync_source";

	U_TRACE_SET_THREAD_NAME(thread_name);
	os_thread_name_self(thread_name);

	u_try_to_set_realtime_priority_on_thread(U_LOGGING_INFO, thread_name);

	XRT_MAYBE_UNUSED ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);

	// Get the choreographer instance bound to this thread's looper.
	AChoreographer *choreographer = AChoreographer_getInstance();

	// Post the first callback to start the continuous chain.
	post_vsync_callback(avs, choreographer);

	set_and_signal_ready(avs, looper);

	while (os_thread_helper_is_running(&avs->looper_thread)) {
		const int result = ALooper_pollOnce(-1, NULL, NULL, NULL);
		if (result == ALOOPER_POLL_ERROR) {
			U_LOG_E("ALooper_pollOnce returned error");
			break;
		}
	}

	return NULL;
}

static void
destroy(struct xrt_vsync_source *xvs)
{
	struct android_choreographer_vsync_source *avs = android_choreographer_vsync_source(xvs);
	if (avs == NULL) {
		return;
	}

	u_vsync_event_source_cancel(&avs->base);

	wake_looper_thread(avs);

	os_thread_helper_destroy(&avs->looper_thread);

	u_vsync_event_source_fini(&avs->base);

	os_cond_destroy(&avs->looper_signal.cond);
	os_mutex_destroy(&avs->looper_signal.mutex);

	free(avs);
}

struct xrt_vsync_source *
android_choreographer_vsync_source_create(void)
{
	struct android_choreographer_vsync_source *avs = U_TYPED_CALLOC(struct android_choreographer_vsync_source);

	u_vsync_event_source_init(&avs->base, destroy);
	os_mutex_init(&avs->looper_signal.mutex);
	os_cond_init(&avs->looper_signal.cond);

	const int32_t display_id = 0;

	avs->app_vsync_offset_ns = android_custom_surface_get_app_vsync_offset_ns(
	    android_globals_get_vm(), android_globals_get_context(), display_id);

	if (os_thread_helper_init(&avs->looper_thread) != 0 || //
	    os_thread_helper_start(&avs->looper_thread, vsync_looper_thread, avs) != 0) {
		U_LOG_E("Failed to start vsync looper thread");
		destroy(&avs->base.base);
		return NULL;
	}

	const int64_t wait_time_ms = 5000;
	if (wait_for_looper_ready(avs, (int64_t)(wait_time_ms * 1e+6)) == ETIMEDOUT) {
		U_LOG_E("waiting for vsync looper thread init timedout");
		destroy(&avs->base.base);
		return NULL;
	}

	return &avs->base.base;
}
