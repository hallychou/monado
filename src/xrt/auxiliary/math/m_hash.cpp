// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hashing function.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_math
 */

#include "math/m_api.h"
#include <string>


extern "C" size_t
math_hash_string(const char *str_c, size_t length)
{
	const std::string str = std::string(str_c, length);
	return std::hash<std::string>{}(str);
}
