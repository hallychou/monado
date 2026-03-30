// Copyright 2020-2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Very simple target main file to export the right symbol.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @ingroup st_ovrd
 */

#include "ovrd_interface.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_config_build.h"
#ifdef XRT_FEATURE_IPC_CLIENT
#include "client/ipc_client_interface.h"
#include "xrt/xrt_instance.h"
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#endif

#if defined(XRT_OS_WINDOWS)
#define HMD_DLL_EXPORT __declspec(dllexport)
#define HMD_DLL_IMPORT __declspec(dllimport)
#elif defined(__GNUC__) || defined(COMPILER_GCC) || defined(__APPLE__)
#define HMD_DLL_EXPORT __attribute__((visibility("default")))
#define HMD_DLL_IMPORT
#else
#error "Unsupported Platform."
#endif


#ifdef XRT_FEATURE_IPC_CLIENT
#define IPC_CONNECT_MAX_RETRIES 10
#define IPC_CONNECT_INITIAL_SLEEP_US 500000

xrt_result_t
xrt_instance_create(struct xrt_instance_info *ii, struct xrt_instance **out_xinst)
{
	struct xrt_instance_info default_info;
	if (ii == NULL) {
		memset(&default_info, 0, sizeof(default_info));
		ii = &default_info;
	}

	xrt_result_t xret;
	for (int attempt = 0; attempt < IPC_CONNECT_MAX_RETRIES; attempt++) {
		xret = ipc_instance_create(ii, out_xinst);
		if (xret == XRT_SUCCESS) {
			return XRT_SUCCESS;
		}

		fprintf(stderr, "monado: IPC connect attempt %d/%d failed, retrying...\n",
		        attempt + 1, IPC_CONNECT_MAX_RETRIES);
		usleep(IPC_CONNECT_INITIAL_SLEEP_US);
	}

	return xret;
}
#endif

HMD_DLL_EXPORT void *
HmdDriverFactory(const char *pInterfaceName, int *pReturnCode)
{
	return ovrd_hmd_driver_impl(pInterfaceName, pReturnCode);
}
