// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for glue classes to wrap constellation blob sink interfaces.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup aux_util
 */

#pragma once

#include "tracking/t_constellation.h"

#include "g_catch_guard.hpp"
#include "g_traits.hpp"

#include <type_traits>


namespace xrt::util {

/*!
 * CRTP glue wrapper for @ref t_blob_sink. Relies on standard layout to
 * recover the derived object from the C struct, and has some requirements and
 * limitations because of that. See @ref cpp-glue-wrappers for the guide and
 * conventions for these wrappers.
 *
 * The derived class `T` must implement:
 * - `void pushBlobs(t_blob_observation &observation)`, which takes ownership of
 *   the observation's frame reference, see @ref t_blob_observation::frame.
 * - `static void destroyBlobSink(t_blob_sink *sink)`, called from
 *   @ref t_blob_sink_destroy. It is static because it usually frees the
 *   object, similar to `destroyFrameNode` in @ref FrameNodeBase.
 *
 * Exceptions thrown from `pushBlobs` are caught and logged.
 */
template <class T> class BlobSinkBase
{
public: // Methods
	BlobSinkBase() noexcept
	{
		static_assert(std::is_standard_layout_v<BlobSinkBase>,
		              "glue base must be standard layout for pointer recovery");
		static_assert(is_non_virtual_base_v<BlobSinkBase, T>,
		              "glue base must be a non-virtual base of T for pointer recovery");

		mBlobSink.push_blobs = pushBlobsWrap;
		mBlobSink.destroy = destroyBlobSinkWrap;
	}

	~BlobSinkBase() noexcept = default;

	const T &
	derived() const noexcept
	{
		return static_cast<const T &>(*this);
	}

	T &
	derived() noexcept
	{
		return static_cast<T &>(*this);
	}

	static const T *
	fromBlobSink(const t_blob_sink *sink) noexcept
	{
		return &(reinterpret_cast<const BlobSinkBase *>(sink)->derived());
	}

	static T *
	fromBlobSink(t_blob_sink *sink) noexcept
	{
		return &(reinterpret_cast<BlobSinkBase *>(sink)->derived());
	}

	const t_blob_sink *
	getBlobSink() const noexcept
	{
		return &mBlobSink;
	}

	t_blob_sink *
	getBlobSink() noexcept
	{
		return &mBlobSink;
	}


private: // Members
	/*!
	 * Wrapped @ref t_blob_sink. Must be the first data member: a pointer to
	 * it is then interconvertible with a pointer to this standard-layout base,
	 * which lets the glue cast a C pointer back to the derived C++ class. See
	 * @ref cpp-glue-wrappers.
	 */
	t_blob_sink mBlobSink = {};


private: // Functions
// Not `->derived()`, that is ambiguous when T has more than one glue base.
#define GET(sink) (*fromBlobSink(sink))

	static void
	pushBlobsWrap(struct t_blob_sink *sink, struct t_blob_observation *observation) noexcept
	try {
		GET(sink).pushBlobs(*observation);
	}
	G_CATCH_GUARDS_VOID

	static void
	destroyBlobSinkWrap(struct t_blob_sink *sink) noexcept
	try {
		T::destroyBlobSink(sink);
	}
	G_CATCH_GUARDS_VOID

#undef GET
};

} // namespace xrt::util
