// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  IPC client proxy for @ref xrt_allocation_collection.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup ipc_client
 */

#pragma once

#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_connection;
struct xrt_allocation_collection;

/*!
 * Create a client-side allocation collection proxy for a server-registered id.
 *
 * Takes ownership of @p id, if creation fails the server-side collection is
 * released.
 *
 * Supports @ref XRT_ALLOCATION_PROPERTY_SWAPCHAIN_INFO and
 * @ref XRT_ALLOCATION_TYPE_NATIVE_IMAGE only.
 *
 * @ingroup ipc_client
 */
xrt_result_t
ipc_client_allocation_collection_create(struct ipc_connection *ipc_c,
                                        uint32_t id,
                                        struct xrt_allocation_collection **out_xac);

#ifdef __cplusplus
}
#endif
