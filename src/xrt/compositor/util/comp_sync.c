// Copyright 2019-2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Independent @ref xrt_compositor_fence implementation.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_util
 */

#include "xrt/xrt_config_os.h"

#include "util/u_misc.h"
#include "util/u_handles.h"
#include "util/u_trace_marker.h"

#include "util/comp_sync.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef XRT_OS_WINDOWS
#include <unistd.h>
#include <poll.h>
#endif

// On Linux one can just poll() on a sync handle.
// It is also a workaround for 10ms waits on NVIDIA vkWaitForFences
#if !defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD) || defined(XRT_OS_ANDROID)
#define XRT_IMPORT_FENCE
#endif


/*
 *
 * Structs.
 *
 */

#if defined(XRT_IMPORT_FENCE)
/*!
 * A very simple implementation of a fence primitive.
 */
struct fence
{
	struct xrt_compositor_fence base;

	struct vk_bundle *vk;

	VkFence fence;
};
#else
/*!
 * A sync_fd implementation of a fence primitive.
 */
struct sync_fd_fence
{
	struct xrt_compositor_fence base;

	struct vk_bundle *vk;

	int fd;
};
#endif


/*
 *
 * Fence member functions.
 *
 */

#if defined(XRT_IMPORT_FENCE)
static xrt_result_t
fence_wait(struct xrt_compositor_fence *xcf, uint64_t timeout)
{
	COMP_TRACE_MARKER();

	struct fence *f = (struct fence *)xcf;
	struct vk_bundle *vk = f->vk;

	// Count no handle as signled fence.
	if (f->fence == VK_NULL_HANDLE) {
		return XRT_SUCCESS;
	}

	VkResult ret = vk->vkWaitForFences(vk->device, 1, &f->fence, VK_TRUE, timeout);
	if (ret == VK_TIMEOUT) {
		return XRT_TIMEOUT;
	}
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkWaitForFences: %s", vk_result_string(ret));
		return XRT_ERROR_VULKAN;
	}

	return XRT_SUCCESS;
}

static void
fence_destroy(struct xrt_compositor_fence *xcf)
{
	COMP_TRACE_MARKER();

	struct fence *f = (struct fence *)xcf;
	struct vk_bundle *vk = f->vk;

	if (f->fence != VK_NULL_HANDLE) {
		vk->vkDestroyFence(vk->device, f->fence, NULL);
		f->fence = VK_NULL_HANDLE;
	}

	free(f);
}

#else

static xrt_result_t
sync_fd_fence_wait(struct xrt_compositor_fence *xcf, uint64_t timeout)
{
	COMP_TRACE_MARKER();

	struct sync_fd_fence *f = (struct sync_fd_fence *)xcf;

	// Count no handle as signled fence.
	if (f->fd == -1) {
		return XRT_SUCCESS;
	}

	struct pollfd fds = {
	    .fd = f->fd,
	    .events = POLLIN,
	};

	int err = poll(&fds, 1, timeout / U_TIME_1MS_IN_NS);

	if (err == 0) {
		return XRT_TIMEOUT;
	}
	if (err < 0) {
		VK_ERROR(f->vk, "sync fd poll: %s", strerror(err));
	}

	return XRT_SUCCESS;
}

static void
sync_fd_fence_destroy(struct xrt_compositor_fence *xcf)
{
	COMP_TRACE_MARKER();

	struct sync_fd_fence *f = (struct sync_fd_fence *)xcf;

	if (f->fd >= 0) {
		close(f->fd);
	}
	free(f);
}
#endif


/*
 *
 * 'Exported' function.
 *
 */

xrt_result_t
comp_fence_import(struct vk_bundle *vk, xrt_graphics_sync_handle_t handle, struct xrt_compositor_fence **out_xcf)
{
	COMP_TRACE_MARKER();

#if defined(XRT_IMPORT_FENCE)
	VkFence fence = VK_NULL_HANDLE;

	VkResult ret = vk_create_fence_sync_from_native(vk, handle, &fence);
	if (ret != VK_SUCCESS) {
		return XRT_ERROR_VULKAN;
	}

	// Name for debugging.
	VK_NAME_FENCE(vk, fence, "Comp Sync");

	struct fence *f = U_TYPED_CALLOC(struct fence);
	f->base.wait = fence_wait;
	f->base.destroy = fence_destroy;
	f->fence = fence;
	f->vk = vk;
#else
	struct sync_fd_fence *f = U_TYPED_CALLOC(struct sync_fd_fence);
	f->base.wait = sync_fd_fence_wait;
	f->base.destroy = sync_fd_fence_destroy;
	f->fd = handle;
	f->vk = vk;
#endif

	*out_xcf = &f->base;

	return XRT_SUCCESS;
}
