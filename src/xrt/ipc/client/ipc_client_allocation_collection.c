// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  IPC client proxy for @ref xrt_allocation_collection.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup ipc_client
 */

#include "xrt/xrt_allocation_collection.h"
#include "xrt/xrt_compositor.h"

#include "util/u_misc.h"
#include "util/u_handles.h"

#include "client/ipc_client.h"
#include "client/ipc_client_allocation_collection.h"
#include "client/ipc_client_connection.h"

#include "ipc_client_generated.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>


struct ipc_client_allocation_collection
{
	struct xrt_allocation_collection base;

	struct ipc_connection *ipc_c;
	uint32_t id;

	/*! Cached from @ref ipc_call_allocation_collection_get_capabilities at create time. */
	struct xrt_swapchain_create_info cached_swapchain_info;
};


static bool
client_ac_supports_native_image(struct ipc_client_allocation_collection *icac)
{
	for (uint32_t i = 0; i < icac->base.supported_types_count; i++) {
		if (icac->base.supported_types[i].type == XRT_ALLOCATION_TYPE_NATIVE_IMAGE) {
			return true;
		}
	}
	return false;
}


static void
ipc_client_allocation_collection_destroy(struct xrt_allocation_collection *xac)
{
	struct ipc_client_allocation_collection *icac = (struct ipc_client_allocation_collection *)xac;
	xrt_result_t xret;

	xret = ipc_call_allocation_collection_destroy(icac->ipc_c, icac->id);

	// Can't return anything here, just continue.
	IPC_CHK_ONLY_PRINT(icac->ipc_c, xret, "ipc_call_allocation_collection_destroy");

	free(icac);
}

static xrt_result_t
ipc_client_allocation_collection_get_property(struct xrt_allocation_collection *xac,
                                              enum xrt_allocation_property prop,
                                              void *ptr)
{
	struct ipc_client_allocation_collection *icac = (struct ipc_client_allocation_collection *)xac;

	if (prop != XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	for (uint32_t i = 0; i < xac->supported_properties_count; i++) {
		if (xac->supported_properties[i].property == prop) {
			*(struct xrt_swapchain_create_info *)ptr = icac->cached_swapchain_info;
			return XRT_SUCCESS;
		}
	}

	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

static xrt_result_t
ipc_client_allocation_collection_get_all(struct xrt_allocation_collection *xac,
                                         enum xrt_allocation_type type,
                                         size_t element_size,
                                         void *ptr)
{
	struct ipc_client_allocation_collection *icac = (struct ipc_client_allocation_collection *)xac;
	struct ipc_allocation_collection_native_images_meta meta = XRT_STRUCT_INIT;
	xrt_graphics_buffer_handle_t handles[XRT_MAX_SWAPCHAIN_IMAGES]; // Initialized below.
	uint32_t image_count = 0;

	if (type != XRT_ALLOCATION_TYPE_NATIVE_IMAGE) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	if (!client_ac_supports_native_image(icac)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	if (element_size != sizeof(struct xrt_image_native) || ptr == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	// To ensure the handles are invalid for various error paths.
	for (uint32_t i = 0; i < XRT_MAX_SWAPCHAIN_IMAGES; i++) {
		handles[i] = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	}

	// Lock the connection to send the command and receive the response.
	ipc_client_connection_lock(icac->ipc_c);

	// To retrieve the image count and meta data.
	xrt_result_t xret = ipc_send_allocation_collection_get_native_images_locked(icac->ipc_c, icac->id);
	IPC_CHK_WITH_GOTO(icac->ipc_c, xret, "ipc_send_allocation_collection_get_native_images_locked", out_unlock);

	// To receive the image count and meta data.
	xret = ipc_receive_allocation_collection_get_native_images_locked( //
	    icac->ipc_c,                                                   //
	    &image_count,                                                  //
	    &meta);                                                        //
	IPC_CHK_WITH_GOTO(icac->ipc_c, xret, "ipc_receive_allocation_collection_get_native_images_locked", out_unlock);
	if (image_count == 0) {
		goto out_unlock;
	}

	/*
	 * Receive the handles. The client trusts the server: image_count is only
	 * range checked after the handles have arrived, and a misbehaving server
	 * can do much worse to the client than send a bad count.
	 */
	struct ipc_command_msg handle_msg = {0};
	xret = ipc_receive_handles_graphics_buffer( //
	    &icac->ipc_c->imc,                      //
	    &handle_msg,                            //
	    sizeof(handle_msg),                     //
	    handles,                                //
	    image_count);                           //
	IPC_CHK_WITH_GOTO(icac->ipc_c, xret, "ipc_receive_handles_graphics_buffer", out_unlock);
	if (handle_msg.cmd != IPC_ALLOCATION_COLLECTION_GET_NATIVE_IMAGES) {
		xret = XRT_ERROR_IPC_FAILURE;
		goto out_unlock;
	}

out_unlock:
	ipc_client_connection_unlock(icac->ipc_c);

	if (xret != XRT_SUCCESS) {
		goto unref_handles;
	}

	if (image_count > XRT_MAX_SWAPCHAIN_IMAGES || meta.image_count != image_count) {
		xret = XRT_ERROR_IPC_FAILURE;
		goto unref_handles;
	}

	icac->base.image_count = image_count;

	struct xrt_image_native *out = (struct xrt_image_native *)ptr;
	for (uint32_t i = 0; i < image_count; i++) {
		out[i].handle = handles[i];
		out[i].size = meta.sizes[i];
		out[i].use_dedicated_allocation = meta.use_dedicated_allocation[i];
		out[i].is_dxgi_handle = meta.is_dxgi_handle[i];
	}

	return xret;

unref_handles:
	// Unref the handles if the command is not as expected.
	for (uint32_t i = 0; i < image_count; i++) {
		u_graphics_buffer_unref(&handles[i]);
	}

	return xret;
}

xrt_result_t
ipc_client_allocation_collection_create(struct ipc_connection *ipc_c,
                                        uint32_t id,
                                        struct xrt_allocation_collection **out_xac)
{
	assert(ipc_c != NULL);
	assert(out_xac != NULL);

	bool supports_swapchain_info = false;
	bool supports_native_image = false;
	uint32_t image_count = 0;
	struct xrt_swapchain_create_info swapchain_info = XRT_STRUCT_INIT;
	xrt_result_t xret, xdret;

	struct ipc_client_allocation_collection *icac = U_TYPED_CALLOC(struct ipc_client_allocation_collection);
	if (icac == NULL) {
		xret = XRT_ERROR_ALLOCATION;
		goto err_release_id;
	}

	xret = ipc_call_allocation_collection_get_capabilities( //
	    ipc_c,                                              //
	    id,                                                 //
	    &supports_swapchain_info,                           //
	    &supports_native_image,                             //
	    &image_count,                                       //
	    &swapchain_info);                                   //
	IPC_CHK_WITH_GOTO(ipc_c, xret, "ipc_call_allocation_collection_get_capabilities", err_free);

	icac->base.reference.count = 1;
	icac->base.image_count = image_count;
	icac->cached_swapchain_info = swapchain_info;

	uint32_t pc = 0;
	if (supports_swapchain_info) {
		assert(pc < XRT_MAX_ALLOCATION_PROPERTY_COUNT);
		icac->base.supported_properties[pc++].property = XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO;
	}
	icac->base.supported_properties_count = pc;

	uint32_t tc = 0;
	if (supports_native_image) {
		assert(tc < XRT_MAX_ALLOCATION_TYPE_COUNT);
		icac->base.supported_types[tc++].type = XRT_ALLOCATION_TYPE_NATIVE_IMAGE;
	}
	icac->base.supported_types_count = tc;

	icac->base.get_property = ipc_client_allocation_collection_get_property;
	icac->base.get_all = ipc_client_allocation_collection_get_all;
	icac->base.destroy = ipc_client_allocation_collection_destroy;
	icac->ipc_c = ipc_c;
	icac->id = id;

	*out_xac = &icac->base;

	return XRT_SUCCESS;

err_free:
	free(icac);
err_release_id:
	xdret = ipc_call_allocation_collection_destroy(ipc_c, id);
	IPC_CHK_ONLY_PRINT(ipc_c, xdret, "ipc_call_allocation_collection_destroy");

	return xret;
}
