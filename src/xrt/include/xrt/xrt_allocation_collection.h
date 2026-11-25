// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Allocation collection.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup xrt
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_limits.h"


#ifdef __cplusplus
extern "C" {
#endif

struct xrt_swapchain_create_info;


/*!
 * @addtogroup xrt_iface
 * @{
 */

/*!
 * Allocation collection property, the properties that can be queried with the
 * @ref xrt_allocation_collection::get_property_raw function.
 *
 * Users do not call @ref xrt_allocation_collection::get_property_raw directly,
 * instead each property has a strongly typed helper function that is noted on
 * the property. Helpers for properties whose type needs big headers are placed
 * in their own `xrt_allocation_collection_<suffix>.h` header so users opt-in to
 * including them.
 */
enum xrt_allocation_property
{
	/*!
	 * Get with @ref xrt_allocation_collection_get_swapchain_info.
	 *
	 * Copies the @ref xrt_swapchain_create_info struct used to create this
	 * allocation collection into the provided pointer. Like this:
	 * ```c
	 * *(struct xrt_swapchain_create_info *)ptr = <the swapchain info>;
	 * ```
	 *
	 * The @p ptr parameter is cast to a pointer to
	 * @ref xrt_swapchain_create_info and the data is copied into it.
	 * The caller owns the copied data and is free to modify it as needed.
	 *
	 * @param ptr Pointer to a @ref xrt_swapchain_create_info to
	 *            receive the copy (will be cast from void*).
	 */
	XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO,

	/*!
	 * Get with @ref xrt_allocation_collection_get_vk_device.
	 *
	 * Returns the VkDevice the Vulkan allocations were created on.
	 *
	 * The @p ptr parameter is cast to a pointer to VkDevice and the handle
	 * is written to it. Like this:
	 * ```c
	 * *(VkDevice *)ptr = device;
	 * ```
	 *
	 * Lifetime note:
	 *
	 * The allocation collection does not own the VkDevice, the device
	 * outlives the collection.
	 *
	 * Callers:
	 * - MUST NOT call vkDestroyDevice on the handle.
	 *
	 * @param ptr Pointer to a VkDevice to receive the handle.
	 */
	XRT_ALLOCATION_PROPERTY_VK_DEVICE,

	/*!
	 * Returns the MTLDevice pointer.
	 *
	 * The @p ptr parameter is cast to a pointer to void pointer
	 * (void**) and the MTLDevice pointer is written to it. Like this:
	 * ```c
	 * *(void **)ptr = (__bridge void *)<the MTLDevice pointer>;
	 * ```
	 *
	 * Reference counting note:
	 *
	 * The allocation collection retains ownership of the MTLDevice object.
	 * The returned pointer is only valid as long as the allocation
	 * collection exists.
	 *
	 * Callers:
	 * - MUST NOT call [device release] on this pointer.
	 * - MUST ensure the allocation collection outlives their use of
	 *   the device.
	 * - If longer lifetime is needed, caller SHOULD cast back
	 *   to id<MTLDevice> and call [device retain], then
	 *   [device release] when done.
	 *
	 * @param ptr Pointer to a void* to receive the MTLDevice pointer
	 *            (will be cast from void* to void**).
	 */
	XRT_ALLOCATION_PROPERTY_METAL_DEVICE,
};

/*!
 * Allocation type, the types that the allocation collection can return from the
 * @ref xrt_allocation_collection::get_all_raw function. Each type has its own
 * element type and lifetime semantics, which the caller needs to know about.
 *
 * Like with @ref xrt_allocation_property each type has a strongly typed helper
 * function that does the type checking, users do not call
 * @ref xrt_allocation_collection::get_all_raw directly.
 */
enum xrt_allocation_type
{
	/*!
	 * Get with @ref xrt_allocation_collection_get_vk_images.
	 *
	 * VkImage handle.
	 *
	 * Lifetime note:
	 *
	 * VkImage handles do not have reference counting, the allocation collection
	 * owns the VkImage objects. The returned handles are only valid as long
	 * as the allocation collection exists.
	 *
	 * Callers:
	 * - MUST NOT call vkDestroyImage on these handles.
	 * - MUST ensure the allocation collection outlives their use of
	 *   the VkImage handles.
	 */
	XRT_ALLOCATION_TYPE_VULKAN_IMAGE,

	/*!
	 * MTLTexture handle (as void pointer).
	 *
	 * Reference counting note:
	 *
	 * The allocation collection retains ownership of the MTLTexture objects.
	 * The returned pointers are only valid as long as the allocation
	 * collection exists.
	 *
	 * Callers:
	 * - MUST NOT call [texture release] on these pointers.
	 * - MUST ensure the allocation collection outlives their use of
	 *   the textures.
	 * - If longer lifetime is needed, caller SHOULD cast back
	 *   to id<MTLTexture> and call [texture retain], then
	 *   [texture release] when done.
	 */
	XRT_ALLOCATION_TYPE_METAL_TEXTURE,

	/*!
	 * IOSurfaceRef handle.
	 *
	 * Reference counting note:
	 *
	 * The allocation collection retains ownership of the IOSurfaceRef objects.
	 * The returned pointers are only valid as long as the allocation
	 * collection exists.
	 *
	 * Callers:
	 * - MUST NOT call CFRelease on these pointers.
	 * - MUST ensure the allocation collection outlives their use of
	 *   the surfaces.
	 * - If longer lifetime is needed, caller SHOULD call
	 *   CFRetain, then CFRelease when done.
	 */
	XRT_ALLOCATION_TYPE_IOSURFACE,

	/*!
	 * Get with @ref xrt_allocation_collection_get_native_images.
	 *
	 * Native image handle (@ref xrt_image_native).
	 *
	 * Ownership note:
	 *
	 * Unlike other allocation types where the collection retains ownership,
	 * calling get_all with this type transfers ownership of the native
	 * handles to the caller. This is an all-or-nothing operation: if
	 * the call succeeds, the caller is responsible for closing/freeing
	 * all returned handles.
	 *
	 * The exact semantics for freeing depends on the underlying handle
	 * type (e.g., file descriptors on Linux, HANDLE on Windows, AHB on
	 * Android). In general, callers should use the appropriate platform
	 * API or @ref u_graphics_buffer_unref to release the handles.
	 *
	 * Callers:
	 * - MUST close/unref all handles if get_all succeeds.
	 * - SHOULD NOT call get_all with this type multiple times, as each
	 *   call may create new handles (implementation dependent).
	 */
	XRT_ALLOCATION_TYPE_NATIVE_IMAGE,
};

/*!
 * A collection of allocations, these can be images, buffers or general memory.
 *
 * This object is meant to be simple, just a container that more complex
 * objects like @ref xrt_swapchain are built on top of. Its main purpose is to
 * share common code between those objects, such as sending handles over IPC,
 * importing into Vulkan and OpenGL, and converting between allocation types.
 *
 * The properties of the collection are immutable after creation, unless
 * otherwise noted. The collection may be "virtual", like the IPC client one
 * which refers to allocations living in the service.
 *
 * Implementations can be fairly high level and may change the layout or even
 * the backing storage of the memory, what matters is that the objects returned
 * by @ref xrt_allocation_collection::get_all_raw keep referring to the same
 * allocations.
 *
 * This interface does not synchronize the contents of the allocations, that is
 * done by wrapper objects such as @ref xrt_swapchain.
 */
struct xrt_allocation_collection
{
	/*!
	 * Reference helper.
	 */
	struct xrt_reference reference;

	/*!
	 * Number of images in the collection, this is the total number of
	 * images in the collection, not the number of images of a given type.
	 * All images are guaranteed to be of the types supported by the
	 * @p supported_types array.
	 */
	uint32_t image_count;

	/*!
	 * Supported properties that can be queried from the @p get_property
	 * function. The @p supported_properties_count field is the number of
	 * supported properties.
	 */
	struct
	{
		enum xrt_allocation_property property;
	} supported_properties[XRT_MAX_ALLOCATION_PROPERTY_COUNT];

	/*!
	 * Number of supported properties.
	 */
	uint32_t supported_properties_count;

	/*!
	 * Supported allocation types that can be queried from the @p get_all
	 * function. Each types have different element size and semantics,
	 * which the caller needs to know about. The @p supported_types_count
	 * field is the number of supported types.
	 */
	struct
	{
		enum xrt_allocation_type type;
	} supported_types[XRT_MAX_ALLOCATION_TYPE_COUNT];

	/*!
	 * Number of supported allocation types.
	 */
	uint32_t supported_types_count;

	/*!
	 * Get a property from the allocation collection.
	 *
	 * This function is type-erased and internal, users MUST use the typed
	 * helper function noted on each @ref xrt_allocation_property.
	 *
	 * @param xac The allocation collection.
	 * @param prop The property to get.
	 * @param ptr Pointer to write the property value to (cast as needed per property).
	 */
	xrt_result_t (*get_property_raw)(struct xrt_allocation_collection *xac,
	                                 enum xrt_allocation_property prop,
	                                 void *ptr);

	/*!
	 * Get all images of a given type.
	 *
	 * This function is type-erased and internal, users MUST use the typed
	 * helper function noted on each @ref xrt_allocation_type.
	 *
	 * @param xac The allocation collection.
	 * @param type The type of the images to get.
	 * @param ptr The pointer to the images.
	 */
	xrt_result_t (*get_all_raw)(struct xrt_allocation_collection *xac, enum xrt_allocation_type type, void *ptr);

	/*!
	 * Destroy the allocation collection,
	 * called when the reference count reaches zero.
	 */
	void (*destroy)(struct xrt_allocation_collection *xac);
};

/*!
 * Typed helper to get the @ref XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO property.
 *
 * @param xac The allocation collection.
 * @param[out] out_info Receives a copy of the swapchain create info.
 *
 * @public @memberof xrt_allocation_collection
 */
static inline xrt_result_t
xrt_allocation_collection_get_swapchain_info(struct xrt_allocation_collection *xac,
                                             struct xrt_swapchain_create_info *out_info)
{
	return xac->get_property_raw(xac, XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO, out_info);
}

/*!
 * Update the reference counts on allocation collection(s).
 *
 * @param[in,out] dst Pointer to a object reference: if the object reference is
 *                non-null will decrement its counter. The reference that
 *                @p dst points to will be set to @p src.
 * @param[in] src New object for @p dst to refer to (may be null).
 *                If non-null, will have its refcount increased.
 * @ingroup xrt_iface
 * @relates xrt_allocation_collection
 */
static inline void
xrt_allocation_collection_reference(struct xrt_allocation_collection **dst, struct xrt_allocation_collection *src)
{
	struct xrt_allocation_collection *old_dst = *dst;

	if (old_dst == src) {
		return;
	}

	if (src) {
		xrt_reference_inc(&src->reference);
	}

	*dst = src;

	if (old_dst) {
		if (xrt_reference_dec_and_is_zero(&old_dst->reference)) {
			old_dst->destroy(old_dst);
		}
	}
}

/*!
 * @}
 */

#ifdef __cplusplus
}
#endif
