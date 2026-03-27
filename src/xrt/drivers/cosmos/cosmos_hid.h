// Copyright 2026, LunaticCat.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the Cosmos HID interface.
 * @author LunaticCat <lunareredwood@gmail.com>
 * @ingroup drv_cosmos
 */

#pragma once


#include <assert.h>
#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif


#define COSMOS_VID 0x0bb4
#define COSMOS_PID 0x0313

#define VIVE_COSMOS_DEFAULT_REFRESH_RATE (90.0f)


static inline void
cosmos_resolution_get_extents(int *out_w, int *out_h)
{
	*out_w = 2880;
	*out_h = 1700;
}

struct cosmos_hid;

int
cosmos_hid_open(struct os_hid_device *hid_dev, struct cosmos_hid **out_hid);

struct cosmos_config *
cosmos_get_config(struct cosmos_hid *cosmos);

void
cosmos_hid_destroy(struct cosmos_hid *cosmos);


#ifdef __cplusplus
} // extern "C"
#endif
