// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows loader wrapper.
 * @author Supremium <realsupremium@outlook.com>
 * @ingroup aux_os
 */

#include "os_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

/*
 * Helper functions.
 */

static thread_local char g_error_buffer[1024];

static void
set_error_from_last_error(void)
{
	DWORD error = GetLastError();

	if (error == 0) {
		g_error_buffer[0] = '\0';
		return;
	}

	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error,
	               MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), g_error_buffer, sizeof(g_error_buffer), NULL);
}

/*
 * Exported functions.
 */

os_library_t
os_library_open(const char *path)
{
	// Not having a full path leads to undefined behavior with LOAD_WITH_ALTERED_SEARCH_PATH.
	// So, GetFullPathNameA will be used to ensure we have a full path.
	char full_path[MAX_PATH];
	if (GetFullPathNameA(path, MAX_PATH, full_path, NULL) == 0) {
		set_error_from_last_error();
		return nullptr;
	}
	
	HMODULE lib = LoadLibraryExA(full_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

	if (lib == NULL) {
		set_error_from_last_error();
		return nullptr;
	}

	return (os_library_t)lib;
}

void *
os_library_symbol(os_library_t lib, const char *name)
{
	if (lib == NULL) {
		return NULL;
	}

	void *sym = (void *)GetProcAddress((HMODULE)lib, name);

	if (sym == NULL) {
		set_error_from_last_error();
	}

	return sym;
}

void
os_library_close(os_library_t lib)
{
	if (lib == NULL) {
		return;
	}

	FreeLibrary((HMODULE)lib);
}

const char *
os_library_error(void)
{
	return g_error_buffer;
}