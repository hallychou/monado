// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan image collection implementing xrt_allocation_collection.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup aux_vk
 */

#include "xrt/xrt_config_vulkan.h"
#include "xrt/xrt_compositor.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_allocation_collection.h"

#include "vk/vk_helpers.h"
#include "vk/vk_image_allocator.h"
#include "vk/vk_image_collection.h"

#include <assert.h>


/*
 *
 * Structs.
 *
 */

/*!
 * Implementation of @ref xrt_allocation_collection that wraps a vk_image_collection.
 */
struct vk_xrt_image_collection
{
	//! Base interface.
	struct xrt_allocation_collection base;

	//! Vulkan bundle used to create images.
	struct vk_bundle *vk;

	//! The wrapped vk_image_collection.
	struct vk_image_collection vkic;

	//! Whether the memory can be exported as native images, false for imported memory.
	bool supports_native_images;

	//! Swapchain creation info used to create this collection.
	struct xrt_swapchain_create_info info;
};


/*
 *
 * Helpers.
 *
 */

static inline struct vk_xrt_image_collection *
vk_xrt_image_collection(struct xrt_allocation_collection *xac)
{
	return (struct vk_xrt_image_collection *)xac;
}


/*
 *
 * Internal functions.
 *
 */

/*!
 * Export new native handles for all images and move them to the caller, see
 * @ref XRT_ALLOCATION_TYPE_NATIVE_IMAGE for ownership.
 *
 * The handles are not cached in the collection, we don't want handles hanging
 * around as there is a limit on how many handles can be open at the same time.
 */
static xrt_result_t
move_natives(struct vk_xrt_image_collection *vxic, struct xrt_image_native *out_natives)
{
	assert(vxic->supports_native_images);
	assert(vxic->base.image_count <= XRT_MAX_SWAPCHAIN_IMAGES);
	assert(out_natives != NULL);

	xrt_graphics_buffer_handle_t handles[XRT_MAX_SWAPCHAIN_IMAGES];

	VkResult ret = vk_ic_get_handles(vxic->vk, &vxic->vkic, XRT_MAX_SWAPCHAIN_IMAGES, handles);
	if (ret != VK_SUCCESS) {
		U_LOG_E("Failed to get native handles for images: %d", ret);
		return XRT_ERROR_VULKAN;
	}

	for (uint32_t i = 0; i < vxic->base.image_count; i++) {
		U_ZERO(&out_natives[i]);
		out_natives[i].handle = handles[i];
		out_natives[i].size = vxic->vkic.images[i].size;
		out_natives[i].use_dedicated_allocation = vxic->vkic.images[i].use_dedicated_allocation;
	}

	return XRT_SUCCESS;
}


/*
 *
 * xrt_allocation_collection interface.
 *
 */

static xrt_result_t
vk_xic_get_property(struct xrt_allocation_collection *xac, enum xrt_allocation_property prop, void *ptr)
{
	struct vk_xrt_image_collection *vxic = vk_xrt_image_collection(xac);

	switch (prop) {
	case XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO:
		// See XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO for semantics.
		// Copy the swapchain info into the caller's struct.
		*(struct xrt_swapchain_create_info *)ptr = vxic->info;
		return XRT_SUCCESS;
	case XRT_ALLOCATION_PROPERTY_VK_DEVICE:
		// See XRT_ALLOCATION_PROPERTY_VK_DEVICE for lifetime semantics.
		*(VkDevice *)ptr = vxic->vk->device;
		return XRT_SUCCESS;
	default: U_LOG_E("Unknown property: %d", (int)prop); return XRT_ERROR_UNSUPPORTED_PROPERTY;
	}
}

static xrt_result_t
vk_xic_get_all(struct xrt_allocation_collection *xac, enum xrt_allocation_type type, void *ptr)
{
	struct vk_xrt_image_collection *vxic = vk_xrt_image_collection(xac);

	switch (type) {
	case XRT_ALLOCATION_TYPE_VULKAN_IMAGE: {
		VkImage *images = (VkImage *)ptr;
		// See XRT_ALLOCATION_TYPE_VULKAN_IMAGE for lifetime semantics.
		for (uint32_t i = 0; i < vxic->base.image_count; i++) {
			images[i] = vxic->vkic.images[i].handle;
		}
		return XRT_SUCCESS;
	}
	case XRT_ALLOCATION_TYPE_NATIVE_IMAGE: {
		if (!u_allocation_collection_supports_type(xac, XRT_ALLOCATION_TYPE_NATIVE_IMAGE)) {
			U_LOG_E("Native images not supported for imported memory");
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}

		return move_natives(vxic, (struct xrt_image_native *)ptr);
	}
	default: U_LOG_E("Unsupported allocation type: %d", (int)type); return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
}

static void
vk_xic_destroy(struct xrt_allocation_collection *xac)
{
	struct vk_xrt_image_collection *vxic = vk_xrt_image_collection(xac);

	// Exported native handles are owned by the callers of get_all, none to close here.
	vk_ic_destroy(vxic->vk, &vxic->vkic);
	free(vxic);
}


/*
 *
 * Helper function.
 *
 */

static void
init_base(struct vk_xrt_image_collection *vxic,
          struct vk_bundle *vk,
          const struct xrt_swapchain_create_info *xscci,
          uint32_t image_count,
          bool supports_native_images)
{
	vxic->base.reference.count = 1;
	vxic->base.image_count = image_count;
	uint32_t tc = 0;
	vxic->base.supported_types[tc++].type = XRT_ALLOCATION_TYPE_VULKAN_IMAGE;
	if (supports_native_images) {
		vxic->base.supported_types[tc++].type = XRT_ALLOCATION_TYPE_NATIVE_IMAGE;
	}
	vxic->base.supported_types_count = tc;
	vxic->base.supported_properties_count = 2;
	vxic->base.supported_properties[0].property = XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO;
	vxic->base.supported_properties[1].property = XRT_ALLOCATION_PROPERTY_VK_DEVICE;
	vxic->base.get_property_raw = vk_xic_get_property;
	vxic->base.get_all_raw = vk_xic_get_all;
	vxic->base.destroy = vk_xic_destroy;

	vxic->vk = vk;
	vxic->supports_native_images = supports_native_images;
	vxic->info = *xscci;
}


/*
 *
 * Exported functions.
 *
 */

xrt_result_t
vk_xac_allocate(struct vk_bundle *vk,
                const struct xrt_swapchain_create_info *xscci,
                uint32_t image_count,
                struct xrt_allocation_collection **out_xac)
{
	assert(vk != NULL);
	assert(xscci != NULL);
	assert(out_xac != NULL);
	assert(image_count > 0);
	assert(image_count <= XRT_MAX_SWAPCHAIN_IMAGES);

	// Allocate the image collection structure.
	struct vk_xrt_image_collection *vxic = U_TYPED_CALLOC(struct vk_xrt_image_collection);
	if (vxic == NULL) {
		U_LOG_E("Failed to allocate vk_xrt_image_collection");
		return XRT_ERROR_ALLOCATION;
	}

	// Allocate Vulkan images.
	VkResult ret = vk_ic_allocate(vk, xscci, image_count, &vxic->vkic);
	if (ret == VK_ERROR_FEATURE_NOT_PRESENT) {
		free(vxic);
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}
	if (ret == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		free(vxic);
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}
	if (ret != VK_SUCCESS) {
		free(vxic);
		return XRT_ERROR_VULKAN;
	}

	// Initialize base structure.
	init_base(vxic, vk, xscci, image_count, true);

	*out_xac = &vxic->base;

	return XRT_SUCCESS;
}

xrt_result_t
vk_xac_from_natives(struct vk_bundle *vk,
                    const struct xrt_swapchain_create_info *xscci,
                    struct xrt_image_native *native_images,
                    uint32_t image_count,
                    struct xrt_allocation_collection **out_xac)
{
	assert(vk != NULL);
	assert(xscci != NULL);
	assert(native_images != NULL);
	assert(out_xac != NULL);
	assert(image_count > 0);
	assert(image_count <= XRT_MAX_SWAPCHAIN_IMAGES);

	// Allocate the image collection structure.
	struct vk_xrt_image_collection *vxic = U_TYPED_CALLOC(struct vk_xrt_image_collection);
	if (vxic == NULL) {
		U_LOG_E("Failed to allocate vk_xrt_image_collection");
		return XRT_ERROR_ALLOCATION;
	}

	// Import native images into Vulkan.
	VkResult ret = vk_ic_from_natives(vk, xscci, native_images, image_count, &vxic->vkic);
	if (ret == VK_ERROR_FEATURE_NOT_PRESENT) {
		free(vxic);
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}
	if (ret == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		free(vxic);
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}
	if (ret != VK_SUCCESS) {
		free(vxic);
		return XRT_ERROR_VULKAN;
	}

	// Initialize base structure.
	init_base(vxic, vk, xscci, image_count, false);

	*out_xac = &vxic->base;

	return XRT_SUCCESS;
}

xrt_result_t
vk_xac_create(struct vk_bundle *vk, struct vk_image_collection *vkic, struct xrt_allocation_collection **out_xac)
{
	assert(vk != NULL);
	assert(vkic != NULL);
	assert(out_xac != NULL);
	assert(vkic->image_count > 0);
	assert(vkic->image_count <= XRT_MAX_SWAPCHAIN_IMAGES);

	// Allocate the image collection structure.
	struct vk_xrt_image_collection *vxic = U_TYPED_CALLOC(struct vk_xrt_image_collection);
	if (vxic == NULL) {
		U_LOG_E("Failed to allocate vk_xrt_image_collection");
		return XRT_ERROR_ALLOCATION;
	}

	// Initialize base structure.
	init_base(vxic, vk, &vkic->info, vkic->image_count, true);

	// Take ownership of the vk_image_collection contents.
	vxic->vkic = *vkic;

	// Clear the source so caller doesn't accidentally double-free.
	U_ZERO(vkic);

	*out_xac = &vxic->base;

	return XRT_SUCCESS;
}
