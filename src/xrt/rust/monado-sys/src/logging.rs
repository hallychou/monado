// Copyright 2026, UltraDev.
// SPDX-License-Identifier: BSL-1.0

//! Logging macros for Rust drivers
//!
//! Example usage:
//! ```
//! #[macro_use]
//! extern crate monado_sys;
//!
//! setup_logging!("DRIVER");
//! ```
//! The log level will be decided based on the DRIVER_LOG environment variable

use crate::{u_log, u_logging_level};
use std::ffi::CString;

/// Initialize the driver's LOG_LEVEL based on the `$name_LOG` environment variable
#[macro_export]
macro_rules! setup_logging {
    ($name:literal) => {
        static LOG_LEVEL: ::std::sync::LazyLock<::monado_sys::u_logging_level> =
            ::std::sync::LazyLock::new(|| unsafe {
                ::monado_sys::debug_get_log_option(
                    ::std::ffi::CStr::from_bytes_with_nul_unchecked(
                        concat!($name, "_LOG\0").as_bytes(),
                    )
                    .as_ptr(),
                    ::monado_sys::u_logging_level::U_LOGGING_INFO,
                )
            });
    };
}

pub fn enabled(level: u_logging_level, current_level: u_logging_level) -> bool {
    level.0 >= current_level.0
}

pub fn write(file: &str, line: u32, func: &str, level: u_logging_level, msg: String) {
    let c = |s: String| CString::new(s.replace('\0', "")).unwrap_or_default();
    let (file, func, msg) = (c(file.into()), c(func.into()), c(msg));
    unsafe { u_log(file.as_ptr(), line as i32, func.as_ptr(), level, c"%s".as_ptr(), msg.as_ptr()) };
}

#[macro_export]
macro_rules! log_at {
    ($level:ident, $($arg:tt)*) => {
        if monado_sys::logging::enabled(monado_sys::u_logging_level::$level, *crate::LOG_LEVEL) {
            monado_sys::logging::write(
                file!(),
                line!(),
                module_path!(),
                monado_sys::u_logging_level::$level,
                format!($($arg)*),
            );
        }
    };
}

#[allow(unused_macros)]
#[macro_export]
macro_rules! error { ($($arg:tt)*) => { monado_sys::log_at!(U_LOGGING_ERROR, $($arg)*) }; }
#[allow(unused_macros)]
#[macro_export]
macro_rules! warn { ($($arg:tt)*) => { monado_sys::log_at!(U_LOGGING_WARN, $($arg)*) }; }
#[allow(unused_macros)]
#[macro_export]
macro_rules! info { ($($arg:tt)*) => { monado_sys::log_at!(U_LOGGING_INFO, $($arg)*) }; }
#[allow(unused_macros)]
#[macro_export]
macro_rules! debug { ($($arg:tt)*) => { monado_sys::log_at!(U_LOGGING_DEBUG, $($arg)*) }; }
#[allow(unused_macros)]
#[macro_export]
macro_rules! trace { ($($arg:tt)*) => { monado_sys::log_at!(U_LOGGING_TRACE, $($arg)*) }; }
