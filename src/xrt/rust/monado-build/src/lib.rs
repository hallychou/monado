// Copyright 2026, UltraDev.
// SPDX-License-Identifier: BSL-1.0

use std::fs;
use std::path::PathBuf;

/// Emits all cmake flags from Monado's *_build.h files as Rust cfg's
pub fn emit_cfgs() {
    let dir = std::env::var_os("DEP_MONADO_CONFIG_DIR")
        .expect("monado_build::emit_cfgs() needs a dependency on monado-sys");
    let mut headers: Vec<PathBuf> = fs::read_dir(&dir)
        .unwrap_or_else(|e| panic!("could not list {}: {e}", PathBuf::from(&dir).display()))
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| n.starts_with("xrt_config_") && n.ends_with(".h"))
        })
        .collect();
    headers.sort();

    for header in headers {
        println!("cargo::rerun-if-changed={}", header.display());
        let text = fs::read_to_string(&header)
            .unwrap_or_else(|e| panic!("could not read {}: {e}", header.display()));
        for line in text.lines().map(str::trim) {
            if let Some(name) = option(line.strip_prefix("#define ")) {
                println!("cargo::rustc-check-cfg=cfg({name})");
                println!("cargo::rustc-cfg={name}");
            } else if let Some(name) = option(line.strip_prefix("/* #undef ").and_then(|l| l.strip_suffix(" */"))) {
                println!("cargo::rustc-check-cfg=cfg({name})");
            }
        }
    }
}

/// Only XRT_* defines become cfgs
fn option(rest: Option<&str>) -> Option<&str> {
    let name = rest?.trim();
    let valid = name.starts_with("XRT_")
        && name.bytes().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit() || b == b'_');
    valid.then_some(name)
}
