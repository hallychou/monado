// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cross platform loader wrapper.
 * @author Supremium <realsupremium@outlook.com>
 * @ingroup aux_os
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Opaque handle to a loaded library.
 */
typedef void *os_library_t;

/*!
 * Open a dynamic library.
 *
 * @param path Path to the library to open.
 * @return Handle to the library, or NULL on failure.
 */
os_library_t
os_library_open(const char *path);

/*!
 * Get a symbol from a loaded library.
 *
 * @param lib Library handle.
 * @param name Name of the symbol to look up.
 * @return Pointer to the symbol, or NULL on failure.
 */
void *
os_library_symbol(os_library_t lib, const char *name);

/*!
 * Close a loaded library.
 *
 * @param lib Library handle.
 */
void
os_library_close(os_library_t lib);

/*!
 * Get the last error message.
 *
 * @return Error message string, or NULL if no error.
 *         The string is valid until the next call to a library function.
 */
const char *
os_library_error(void);

#ifdef __cplusplus
}
#endif