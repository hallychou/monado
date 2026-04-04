// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  IPC server handlers for @ref xrt_allocation_collection.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup ipc_server
 */

#include "xrt/xrt_allocation_collection.h"
#include "xrt/xrt_compositor.h"

#include "shared/ipc_protocol.h"
#include "server/ipc_server.h"
#include "server/ipc_server_objects.h"

#include "ipc_server_generated.h"

#include "util/u_handles.h"
#include "util/u_misc.h"

#include <assert.h>

/*
 *
 * Helper functions.
 *
 */

/*!
 * Get an allocation collection by ID, must only be called from the per client
 * thread as this function accesses the client state's memory. The macro will
 * insert a return statement on an error.
 *
 * @param ICS The client state instance.
 * @param ID The allocation collection ID.
 * @param XAC Will be filled with the allocation collection object on success.
 * @ingroup ipc_server
 */
#define GET_XAC_OR_RETURN(ICS, ID, XAC)                                                                                \
	do {                                                                                                           \
		xrt_result_t xret = ipc_server_objects_get_xac_and_validate((ICS), ID, &(XAC));                        \
		IPC_CHK_AND_RET((ICS)->server, xret, "ipc_server_objects_get_xac_and_validate");                       \
	} while (0)

static bool
xac_supports_property(struct xrt_allocation_collection *xac, enum xrt_allocation_property prop)
{
	for (uint32_t i = 0; i < xac->supported_properties_count; i++) {
		if (xac->supported_properties[i].property == prop) {
			return true;
		}
	}
	return false;
}

static bool
xac_supports_type(struct xrt_allocation_collection *xac, enum xrt_allocation_type type)
{
	for (uint32_t i = 0; i < xac->supported_types_count; i++) {
		if (xac->supported_types[i].type == type) {
			return true;
		}
	}
	return false;
}


/*
 *
 * Handler functions for allocation collection.
 *
 */

xrt_result_t
ipc_handle_allocation_collection_get_capabilities(volatile struct ipc_client_state *ics,
                                                  uint32_t id,
                                                  bool *out_supports_swapchain_info,
                                                  bool *out_supports_native_image,
                                                  uint32_t *out_image_count,
                                                  struct xrt_swapchain_create_info *out_swapchain_info)
{
	struct xrt_allocation_collection *xac = NULL;
	GET_XAC_OR_RETURN(ics, id, xac);

	*out_supports_swapchain_info = xac_supports_property(xac, XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO);
	*out_supports_native_image = xac_supports_type(xac, XRT_ALLOCATION_TYPE_NATIVE_IMAGE);
	*out_image_count = xac->image_count;

	if (*out_supports_swapchain_info) {
		xrt_result_t xret = xrt_allocation_collection_get_property( //
		    xac,                                                    //
		    XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO,                 //
		    out_swapchain_info);                                    //
		if (xret != XRT_SUCCESS) {
			U_ZERO(out_swapchain_info);
		}
	} else {
		U_ZERO(out_swapchain_info);
	}

	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_allocation_collection_get_swapchain_info(volatile struct ipc_client_state *ics,
                                                    uint32_t id,
                                                    struct xrt_swapchain_create_info *out_info)
{
	struct xrt_allocation_collection *xac = NULL;
	GET_XAC_OR_RETURN(ics, id, xac);

	if (!xac_supports_property(xac, XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO)) {
		U_ZERO(out_info);
		return XRT_SUCCESS;
	}

	return xrt_allocation_collection_get_property(xac, XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO, out_info);
}

xrt_result_t
ipc_handle_allocation_collection_get_native_images(volatile struct ipc_client_state *ics, uint32_t id)
{
	struct ipc_message_channel *imc = (struct ipc_message_channel *)&ics->imc;
	struct ipc_allocation_collection_get_native_images_reply reply = XRT_STRUCT_INIT;

	struct xrt_allocation_collection *xac = NULL;
	xrt_result_t xret = ipc_server_objects_get_xac_and_validate(ics, id, &xac);
	if (xret != XRT_SUCCESS) {
		reply.result = xret;
		xret = ipc_send(imc, &reply, sizeof(reply));
		IPC_CHK_ALWAYS_RET(ics->server, xret, "ipc_send");
	}

	if (!xac_supports_type(xac, XRT_ALLOCATION_TYPE_NATIVE_IMAGE)) {
		reply.result = XRT_ERROR_FEATURE_NOT_SUPPORTED;
		xret = ipc_send(imc, &reply, sizeof(reply));
		IPC_CHK_ALWAYS_RET(ics->server, xret, "ipc_send");
	}

	const uint32_t count = xac->image_count;
	if (count == 0 || count > XRT_MAX_SWAPCHAIN_IMAGES) {
		reply.result = XRT_ERROR_IPC_FAILURE;
		xret = ipc_send(imc, &reply, sizeof(reply));
		IPC_CHK_ALWAYS_RET(ics->server, xret, "ipc_send");
	}

	struct xrt_image_native images[XRT_MAX_SWAPCHAIN_IMAGES] = XRT_STRUCT_INIT;

	reply.result = xrt_allocation_collection_get_all( //
	    xac,                                          //
	    XRT_ALLOCATION_TYPE_NATIVE_IMAGE,             //
	    sizeof(struct xrt_image_native),              //
	    images);                                      //
	IPC_CHK_ONLY_PRINT(ics->server, reply.result, "xrt_allocation_collection_get_all");
	if (reply.result != XRT_SUCCESS) {
		xret = ipc_send(imc, &reply, sizeof(reply));
		IPC_CHK_ALWAYS_RET(ics->server, xret, "ipc_send");
	}

	reply.image_count = count;
	reply.native_meta.image_count = count;

	for (uint32_t i = 0; i < count; i++) {
		reply.native_meta.sizes[i] = images[i].size;
		reply.native_meta.use_dedicated_allocation[i] = images[i].use_dedicated_allocation;
		reply.native_meta.is_dxgi_handle[i] = images[i].is_dxgi_handle;
	}

	xret = ipc_send(imc, &reply, sizeof(reply));
	IPC_CHK_WITH_GOTO(ics->server, xret, "ipc_send", out);

	xrt_graphics_buffer_handle_t handle_row[XRT_MAX_SWAPCHAIN_IMAGES];
	for (uint32_t i = 0; i < count; i++) {
		handle_row[i] = images[i].handle;
	}

	struct ipc_command_msg filler = {
	    .cmd = IPC_ALLOCATION_COLLECTION_GET_NATIVE_IMAGES,
	};

	xret = ipc_send_handles_graphics_buffer(imc, &filler, sizeof(filler), handle_row, count);
	IPC_CHK_ONLY_PRINT(ics->server, xret, "ipc_send_handles_graphics_buffer");

out:
	for (uint32_t i = 0; i < count; i++) {
		u_graphics_buffer_unref(&images[i].handle);
	}

	return xret;
}

xrt_result_t
ipc_handle_allocation_collection_destroy(volatile struct ipc_client_state *ics, uint32_t id)
{
	return ipc_server_objects_destroy_xac(ics, id);
}
