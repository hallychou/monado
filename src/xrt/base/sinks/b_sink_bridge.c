// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Moves frames and timing events between two frame contexts.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup base
 */

#include "util/u_misc.h"

#include "os/os_threading.h"

#include "b_sink_bridge.h"

#include <assert.h>


struct b_sink_bridge_channel
{
	// Sending side.
	struct xrt_frame_sink frame_sink;
	struct t_timing_event_sink timing_sink;

	// Receiving side.
	struct xrt_frame_source frame_source;
	struct t_timing_event_source timing_source;

	struct xrt_frame_sink *frame_sinks[B_SINK_BRIDGE_MAX_SINKS];
	struct t_timing_event_sink *timing_sinks[B_SINK_BRIDGE_MAX_SINKS];
};

struct b_sink_bridge
{
	//! One for the send node, plus one while the receive node is in a context.
	struct xrt_reference ref;

	struct xrt_frame_node send_node;
	struct xrt_frame_node recv_node;

	/*!
	 * Held while pushing, sinks are not reference counted so this is what
	 * makes sure nothing is pushed to them after remove_sink has returned or
	 * the receiving context has been broken apart.
	 */
	struct os_mutex mutex;

	//! Cleared when the send node is broken apart.
	bool sending;

	//! Set on attach, cleared when the receive node is broken apart.
	bool receiving;

	//! Set on attach, cleared when the receive node is destroyed.
	bool recv_in_context;

	struct b_sink_bridge_channel channels[B_SINK_BRIDGE_MAX_CHANNELS];
};


/*
 *
 * Helpers.
 *
 */

static inline struct b_sink_bridge *
bridge_from_channel(struct b_sink_bridge_channel *ch, uint32_t index)
{
	// offsetof only accepts a plain member name in ISO C, so step back to channels[0] first.
	struct b_sink_bridge_channel *first = ch - index;

	return container_of(first, struct b_sink_bridge, channels);
}

static void
bridge_unref(struct b_sink_bridge *bsb)
{
	if (!xrt_reference_dec_and_is_zero(&bsb->ref)) {
		return;
	}

	os_mutex_destroy(&bsb->mutex);
	free(bsb);
}

static void
push_frame(struct b_sink_bridge *bsb, uint32_t index, struct xrt_frame *xf)
{
	struct b_sink_bridge_channel *ch = &bsb->channels[index];

	os_mutex_lock(&bsb->mutex);
	if (bsb->sending && bsb->receiving) {
		for (size_t i = 0; i < ARRAY_SIZE(ch->frame_sinks); i++) {
			if (ch->frame_sinks[i] != NULL) {
				xrt_sink_push_frame(ch->frame_sinks[i], xf);
			}
		}
	}
	os_mutex_unlock(&bsb->mutex);
}

static void
push_timing_event(struct b_sink_bridge *bsb, uint32_t index, const struct t_timing_event *event)
{
	struct b_sink_bridge_channel *ch = &bsb->channels[index];

	os_mutex_lock(&bsb->mutex);
	if (bsb->sending && bsb->receiving) {
		for (size_t i = 0; i < ARRAY_SIZE(ch->timing_sinks); i++) {
			if (ch->timing_sinks[i] != NULL) {
				t_timing_event_sink_push_timing_event(ch->timing_sinks[i], event);
			}
		}
	}
	os_mutex_unlock(&bsb->mutex);
}

static xrt_result_t
frame_source_add_sink(struct b_sink_bridge *bsb, uint32_t index, struct xrt_frame_sink *xs)
{
	struct b_sink_bridge_channel *ch = &bsb->channels[index];
	xrt_result_t xret = XRT_ERROR_ALLOCATION;

	os_mutex_lock(&bsb->mutex);
	if (!bsb->receiving) {
		xret = XRT_ERROR_INVALID_ARGUMENT;
	}
	for (size_t i = 0; bsb->receiving && i < ARRAY_SIZE(ch->frame_sinks); i++) {
		if (ch->frame_sinks[i] == NULL) {
			ch->frame_sinks[i] = xs;
			xret = XRT_SUCCESS;
			break;
		}
	}
	os_mutex_unlock(&bsb->mutex);

	return xret;
}

static void
frame_source_remove_sink(struct b_sink_bridge *bsb, uint32_t index, struct xrt_frame_sink *xs)
{
	struct b_sink_bridge_channel *ch = &bsb->channels[index];

	os_mutex_lock(&bsb->mutex);
	for (size_t i = 0; i < ARRAY_SIZE(ch->frame_sinks); i++) {
		if (ch->frame_sinks[i] == xs) {
			ch->frame_sinks[i] = NULL;
		}
	}
	os_mutex_unlock(&bsb->mutex);
}

static int
timing_source_add_sink(struct b_sink_bridge *bsb, uint32_t index, struct t_timing_event_sink *sink)
{
	struct b_sink_bridge_channel *ch = &bsb->channels[index];
	int ret = -1;

	os_mutex_lock(&bsb->mutex);
	for (size_t i = 0; bsb->receiving && i < ARRAY_SIZE(ch->timing_sinks); i++) {
		if (ch->timing_sinks[i] == NULL) {
			ch->timing_sinks[i] = sink;
			ret = 0;
			break;
		}
	}
	os_mutex_unlock(&bsb->mutex);

	return ret;
}

static void
timing_source_remove_sink(struct b_sink_bridge *bsb, uint32_t index, struct t_timing_event_sink *sink)
{
	struct b_sink_bridge_channel *ch = &bsb->channels[index];

	os_mutex_lock(&bsb->mutex);
	for (size_t i = 0; i < ARRAY_SIZE(ch->timing_sinks); i++) {
		if (ch->timing_sinks[i] == sink) {
			ch->timing_sinks[i] = NULL;
		}
	}
	os_mutex_unlock(&bsb->mutex);
}


/*
 *
 * Per channel functions, the channel index is baked in so no back pointer is needed.
 *
 */

#define BRIDGE_FROM(PTR, FIELD, IDX) bridge_from_channel(container_of(PTR, struct b_sink_bridge_channel, FIELD), IDX)

#define DEFINE_CHANNEL_FUNCS(IDX)                                                                                      \
	static void push_frame_##IDX(struct xrt_frame_sink *xs, struct xrt_frame *xf)                                  \
	{                                                                                                              \
		push_frame(BRIDGE_FROM(xs, frame_sink, IDX), IDX, xf);                                                 \
	}                                                                                                              \
	static void push_timing_event_##IDX(struct t_timing_event_sink *sink, const struct t_timing_event *event)      \
	{                                                                                                              \
		push_timing_event(BRIDGE_FROM(sink, timing_sink, IDX), IDX, event);                                    \
	}                                                                                                              \
	static xrt_result_t frame_add_sink_##IDX(struct xrt_frame_source *xfsrc, struct xrt_frame_sink *xs)            \
	{                                                                                                              \
		return frame_source_add_sink(BRIDGE_FROM(xfsrc, frame_source, IDX), IDX, xs);                          \
	}                                                                                                              \
	static void frame_remove_sink_##IDX(struct xrt_frame_source *xfsrc, struct xrt_frame_sink *xs)                 \
	{                                                                                                              \
		frame_source_remove_sink(BRIDGE_FROM(xfsrc, frame_source, IDX), IDX, xs);                              \
	}                                                                                                              \
	static int timing_add_sink_##IDX(struct t_timing_event_source *source, struct t_timing_event_sink *sink)       \
	{                                                                                                              \
		return timing_source_add_sink(BRIDGE_FROM(source, timing_source, IDX), IDX, sink);                     \
	}                                                                                                              \
	static void timing_remove_sink_##IDX(struct t_timing_event_source *source, struct t_timing_event_sink *sink)   \
	{                                                                                                              \
		timing_source_remove_sink(BRIDGE_FROM(source, timing_source, IDX), IDX, sink);                         \
	}

XRT_TRACKING_FOR_EACH_CAM(DEFINE_CHANNEL_FUNCS)
#undef DEFINE_CHANNEL_FUNCS
#undef BRIDGE_FROM

#define SETUP_CHANNEL(IDX)                                                                                             \
	do {                                                                                                           \
		struct b_sink_bridge_channel *ch = &bsb->channels[IDX];                                                \
		ch->frame_sink.push_frame = push_frame_##IDX;                                                          \
		ch->timing_sink.push_timing_event = push_timing_event_##IDX;                                           \
		ch->frame_source.add_sink = frame_add_sink_##IDX;                                                      \
		ch->frame_source.remove_sink = frame_remove_sink_##IDX;                                                \
		ch->timing_source.add_sink = timing_add_sink_##IDX;                                                    \
		ch->timing_source.remove_sink = timing_remove_sink_##IDX;                                              \
	} while (false);


/*
 *
 * Frame node functions.
 *
 */

static void
send_break_apart(struct xrt_frame_node *node)
{
	struct b_sink_bridge *bsb = container_of(node, struct b_sink_bridge, send_node);

	os_mutex_lock(&bsb->mutex);
	bsb->sending = false;
	os_mutex_unlock(&bsb->mutex);
}

static void
send_destroy(struct xrt_frame_node *node)
{
	bridge_unref(container_of(node, struct b_sink_bridge, send_node));
}

static void
recv_break_apart(struct xrt_frame_node *node)
{
	struct b_sink_bridge *bsb = container_of(node, struct b_sink_bridge, recv_node);

	os_mutex_lock(&bsb->mutex);
	bsb->receiving = false;
	for (size_t i = 0; i < ARRAY_SIZE(bsb->channels); i++) {
		U_ZERO_ARRAY(bsb->channels[i].frame_sinks);
		U_ZERO_ARRAY(bsb->channels[i].timing_sinks);
	}
	os_mutex_unlock(&bsb->mutex);
}

static void
recv_destroy(struct xrt_frame_node *node)
{
	struct b_sink_bridge *bsb = container_of(node, struct b_sink_bridge, recv_node);

	os_mutex_lock(&bsb->mutex);
	bsb->recv_in_context = false;
	os_mutex_unlock(&bsb->mutex);

	bridge_unref(bsb);
}


/*
 *
 * 'Exported' functions.
 *
 */

xrt_result_t
b_sink_bridge_create(struct xrt_frame_context *send_xfctx, struct b_sink_bridge **out_bsb)
{
	struct b_sink_bridge *bsb = U_TYPED_CALLOC(struct b_sink_bridge);
	if (bsb == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	if (os_mutex_init(&bsb->mutex) < 0) {
		free(bsb);
		return XRT_ERROR_SYNC_PRIMITIVE_CREATION_FAILED;
	}

	XRT_TRACKING_FOR_EACH_CAM(SETUP_CHANNEL)
#undef SETUP_CHANNEL

	bsb->ref.count = 1;
	bsb->sending = true;
	bsb->send_node.break_apart = send_break_apart;
	bsb->send_node.destroy = send_destroy;
	bsb->recv_node.break_apart = recv_break_apart;
	bsb->recv_node.destroy = recv_destroy;

	xrt_frame_context_add(send_xfctx, &bsb->send_node);

	*out_bsb = bsb;

	return XRT_SUCCESS;
}

xrt_result_t
b_sink_bridge_attach(struct b_sink_bridge *bsb, struct xrt_frame_context *recv_xfctx)
{
	os_mutex_lock(&bsb->mutex);
	if (bsb->recv_in_context) {
		os_mutex_unlock(&bsb->mutex);
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	xrt_reference_inc(&bsb->ref);
	bsb->receiving = true;
	bsb->recv_in_context = true;
	xrt_frame_context_add(recv_xfctx, &bsb->recv_node);
	os_mutex_unlock(&bsb->mutex);

	return XRT_SUCCESS;
}

struct xrt_frame_sink *
b_sink_bridge_get_frame_sink(struct b_sink_bridge *bsb, uint32_t index)
{
	assert(index < ARRAY_SIZE(bsb->channels));
	return &bsb->channels[index].frame_sink;
}

struct t_timing_event_sink *
b_sink_bridge_get_timing_sink(struct b_sink_bridge *bsb, uint32_t index)
{
	assert(index < ARRAY_SIZE(bsb->channels));
	return &bsb->channels[index].timing_sink;
}

struct xrt_frame_source *
b_sink_bridge_get_frame_source(struct b_sink_bridge *bsb, uint32_t index)
{
	assert(index < ARRAY_SIZE(bsb->channels));
	return &bsb->channels[index].frame_source;
}

struct t_timing_event_source *
b_sink_bridge_get_timing_source(struct b_sink_bridge *bsb, uint32_t index)
{
	assert(index < ARRAY_SIZE(bsb->channels));
	return &bsb->channels[index].timing_source;
}
