# Copyright 2026, UltraDev.
# SPDX-License-Identifier: BSL-1.0

include(MonadoRustBindgen)

function(monado_rust_finalize)
	get_property(features GLOBAL PROPERTY XRT_RUST_LIBS)
	get_property(link_libraries GLOBAL PROPERTY XRT_RUST_LINK_LIBRARIES)
	if(NOT features)
		return()
	endif()
	if(NOT XRT_HAVE_RUST)
		message(FATAL_ERROR "Rust libraries ${features} are enabled while Rust support is missing, they should gate on XRT_HAVE_RUST")
	endif()
	list(REMOVE_DUPLICATES features)
	list(REMOVE_DUPLICATES link_libraries)

	corrosion_import_crate(
		MANIFEST_PATH ${PROJECT_SOURCE_DIR}/Cargo.toml
		CRATES monado-rust
		FEATURES ${features}
	)
	target_link_libraries(
		monado_rust-static
		INTERFACE
			xrt-interfaces
			aux_util
			aux_math
			aux_tracking
			tracking-includes
			target_helpers
			drv_includes
			${link_libraries}
		)
	monado_rust_bindgen_args()
endfunction()


function(rust_add_library name)
	set(single_value CRATE)
	cmake_parse_arguments(PARSE_ARGV 1 arg "" "${single_value}" "")

	if(NOT arg_CRATE)
		message(FATAL_ERROR "rust_add_library must specify the crate (feature) e.g. rust_add_library(drv_example CRATE example)")
	endif()
	if(arg_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "rust_add_library got unexpected arguments ${arg_UNPARSED_ARGUMENTS}")
	endif()

	set_property(GLOBAL APPEND PROPERTY XRT_RUST_LIBS ${arg_CRATE})
	add_library(${name} INTERFACE)
	target_link_libraries(${name} INTERFACE monado_rust)
endfunction()

function(rust_link_libraries)
	set_property(GLOBAL APPEND PROPERTY XRT_RUST_LINK_LIBRARIES ${ARGV})
endfunction()
