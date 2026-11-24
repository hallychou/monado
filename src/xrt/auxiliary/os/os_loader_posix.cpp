// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  POSIX loader wrapper.
 * @author Supremium <realsupremium@outlook.com>
 * @ingroup aux_os
 */

#include "os_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

/*
 * Exported functions.
 */

os_library_t
os_library_open(const char *path)
{
	void *lib = dlopen(path, RTLD_LAZY);
	return (os_library_t)lib;
}

void *
os_library_symbol(os_library_t lib, const char *name)
{
	if (lib == NULL) {
		return NULL;
	}

	return dlsym(lib, name);
}

void
os_library_close(os_library_t lib)
{
	if (lib == NULL) {
		return;
	}

	dlclose(lib);
}

const char *
os_library_error(void)
{
	return dlerror();
}