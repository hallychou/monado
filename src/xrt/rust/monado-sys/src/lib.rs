// Copyright 2026, UltraDev.
// SPDX-License-Identifier: BSL-1.0

#![allow(
    non_upper_case_globals,
    non_camel_case_types,
    non_snake_case,
    dead_code,
    unsafe_op_in_unsafe_fn,
    clippy::all
)]
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

pub mod math;
pub mod logging;
