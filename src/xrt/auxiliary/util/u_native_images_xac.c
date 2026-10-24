// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Allocation collection over native images.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup aux_util
 */

#include "xrt/xrt_compositor.h"

#include "util/u_misc.h"
#include "util/u_handles.h"
#include "util/u_logging.h"
#include "util/u_native_images_xac.h"

#include <assert.h>
#include <stdlib.h>


struct u_native_images_xac
{
	struct xrt_allocation_collection base;

	struct xrt_swapchain_create_info info;

	struct xrt_image_native images[XRT_MAX_SWAPCHAIN_IMAGES];
};

static inline struct u_native_images_xac *
u_native_images_xac(struct xrt_allocation_collection *xac)
{
	return (struct u_native_images_xac *)xac;
}

static void
release_image(struct xrt_image_native *image)
{
	// DXGI handles can't be closed.
	if (!image->is_dxgi_handle) {
		u_graphics_buffer_unref(&image->handle);
	}
}

static xrt_result_t
native_xac_get_property(struct xrt_allocation_collection *xac, enum xrt_allocation_property prop, void *ptr)
{
	struct u_native_images_xac *nxac = u_native_images_xac(xac);

	if (prop != XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO) {
		return XRT_ERROR_UNSUPPORTED_PROPERTY;
	}

	*(struct xrt_swapchain_create_info *)ptr = nxac->info;

	return XRT_SUCCESS;
}

static xrt_result_t
native_xac_get_all(struct xrt_allocation_collection *xac, enum xrt_allocation_type type, size_t element_size, void *ptr)
{
	struct u_native_images_xac *nxac = u_native_images_xac(xac);

	if (type != XRT_ALLOCATION_TYPE_NATIVE_IMAGE) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	if (element_size != sizeof(struct xrt_image_native) || ptr == NULL) {
		U_LOG_E("Invalid element size %zu or NULL pointer", element_size);
		return XRT_ERROR_ALLOCATION;
	}

	struct xrt_image_native *out = (struct xrt_image_native *)ptr;
	for (uint32_t i = 0; i < xac->image_count; i++) {
		out[i] = nxac->images[i];

		if (nxac->images[i].is_dxgi_handle || !xrt_graphics_buffer_is_valid(nxac->images[i].handle)) {
			continue;
		}

		out[i].handle = u_graphics_buffer_ref(nxac->images[i].handle);
		if (!xrt_graphics_buffer_is_valid(out[i].handle)) {
			U_LOG_E("Failed to reference native image %u", i);
			for (uint32_t k = 0; k < i; k++) {
				release_image(&out[k]);
			}
			return XRT_ERROR_ALLOCATION;
		}
	}

	return XRT_SUCCESS;
}

static void
native_xac_destroy(struct xrt_allocation_collection *xac)
{
	struct u_native_images_xac *nxac = u_native_images_xac(xac);

	for (uint32_t i = 0; i < xac->image_count; i++) {
		release_image(&nxac->images[i]);
	}

	free(nxac);
}

xrt_result_t
u_native_images_xac_create(const struct xrt_swapchain_create_info *info,
                           struct xrt_image_native *native_images,
                           uint32_t image_count,
                           struct xrt_allocation_collection **out_xac)
{
	assert(info != NULL);
	assert(native_images != NULL);
	assert(out_xac != NULL);

	if (image_count == 0 || image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		U_LOG_E("Invalid image count %u", image_count);
		return XRT_ERROR_ALLOCATION;
	}

	struct u_native_images_xac *nxac = U_TYPED_CALLOC(struct u_native_images_xac);
	if (nxac == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	nxac->base.reference.count = 1;
	nxac->base.image_count = image_count;
	nxac->base.supported_properties[0].property = XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO;
	nxac->base.supported_properties_count = 1;
	nxac->base.supported_types[0].type = XRT_ALLOCATION_TYPE_NATIVE_IMAGE;
	nxac->base.supported_types_count = 1;
	nxac->base.get_property = native_xac_get_property;
	nxac->base.get_all = native_xac_get_all;
	nxac->base.destroy = native_xac_destroy;
	nxac->info = *info;

	for (uint32_t i = 0; i < image_count; i++) {
		nxac->images[i] = native_images[i];
		native_images[i].handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	}

	*out_xac = &nxac->base;

	return XRT_SUCCESS;
}
