// Copyright 2026, UltraDev.
// SPDX-License-Identifier: BSL-1.0

use std::cell::OnceCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::{env, fs};

/// Remembers where clang found Monado's generated config headers.
#[derive(Debug, Default)]
struct ConfigDir(Rc<OnceCell<PathBuf>>);

impl bindgen::callbacks::ParseCallbacks for ConfigDir {
    fn include_file(&self, filename: &str) {
        let path = Path::new(filename);
        if path.file_name().is_some_and(|n| n == "xrt_config_build.h") {
            let _ = self.0.set(path.parent().unwrap().to_owned());
        }
    }
}

const HEADERS: &[(&str, &str)] = &[
    ("include/xrt", "*.h"),
    ("auxiliary/util", "*.h"),
    ("auxiliary/math", "*.h"),
    ("auxiliary/os", "*.h"),
    ("auxiliary/tracking", "*.h"),
];

const SKIP_HEADERS: &[&str] = &[
    "include/xrt/xrt_gfx_win32.h",
    "include/xrt/xrt_openxr_includes.h",
    "auxiliary/util/u_windows.h",
    "auxiliary/tracking/t_vit_loader.h",
    "auxiliary/util/u_builder_search.h",
    "auxiliary/util/u_distortion.h",
    "auxiliary/util/u_id_ringbuffer.h",
    "auxiliary/os/os_serial.h",
];

fn umbrella_header(src_xrt: &Path, manifest: &Path, out: &Path) -> PathBuf {
    let mut headers = Vec::new();
    for (dir, name) in HEADERS {
        let pattern = src_xrt.join(dir).join(name);
        println!("cargo::rerun-if-changed={}", src_xrt.join(dir).display());
        for header in glob::glob(pattern.to_str().unwrap()).unwrap().map(Result::unwrap) {
            let rel = header.strip_prefix(src_xrt).unwrap();
            if !SKIP_HEADERS.iter().any(|s| rel == Path::new(s)) {
                headers.push(header);
            }
        }
    }
    headers.sort();

    let mut text = String::from("#include \"xrt/xrt_config_build.h\"\n");
    for header in &headers {
        text += &format!("#include \"{}\"\n", header.display());
    }
    text += &format!("#include \"{}\"\n", manifest.join("wrapper.h").display());
    let path = out.join("monado.h");
    fs::write(&path, text).unwrap();
    path
}

fn args_file() -> PathBuf {
    env::var_os("MONADO_BINDGEN_ARGS").map(PathBuf::from).unwrap_or_else(|| {
        panic!(
            "MONADO_BINDGEN_ARGS is not set"
        )
    })
}

fn read_args(path: &Path) -> Vec<String> {
    let text = fs::read_to_string(path).unwrap_or_else(|e| {
        panic!(
            "could not read MONADO_BINDGEN_ARGS={}: {e}",
            path.display()
        )
    });
    text.lines().map(str::trim).filter(|l| !l.is_empty()).map(String::from).collect()
}

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let args_file = args_file();
    // os_threading.h (pulled in by u_sink.h) needs _GNU_SOURCE for pthread_setname_np.
    let args: Vec<String> = ["-D_GNU_SOURCE".to_owned(), format!("-I{}", manifest.display())]
        .into_iter()
        .chain(read_args(&args_file))
        .collect();

    let src_xrt = manifest.ancestors().nth(2).unwrap().canonicalize().unwrap();

    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let static_fns = out.join("static_fns.c");

    let header = umbrella_header(&src_xrt, &manifest, &out);

    let config_dir = ConfigDir::default();
    let found_config_dir = Rc::clone(&config_dir.0);

    let bindings = bindgen::Builder::default()
        .header(header.to_str().unwrap())
        .clang_arg("-std=gnu11")
        .clang_args(&args)

        // monado headers
        .allowlist_file(format!("{}/.*", regex::escape(src_xrt.to_str().unwrap())))
        // third-party headers we use
        .allowlist_function("cJSON_(Parse|Delete)")

        .bitfield_enum("xrt_space_relation_flags")
        .bitfield_enum("xrt_blend_mode")
        .default_enum_style(bindgen::EnumVariation::NewType {
            is_bitfield: false,
            is_global: false,
        })
        .derive_default(true)
        .no_default("xrt_quat")
        .derive_debug(true)
        .layout_tests(true)
        .wrap_static_fns(true)
        .wrap_static_fns_path(&static_fns)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .parse_callbacks(Box::new(config_dir))
        .generate()
        .expect("bindgen failed");

    bindings
        .write_to_file(out.join("bindings.rs"))
        .expect("could not write bindings");

    // needed by monado-build to generate rust cfg's for monado's cmake flags
    let config_dir = found_config_dir.get().expect("wrapper.h includes xrt_config_build.h");
    println!("cargo::metadata=config_dir={}", config_dir.display());

    let mut shims = cc::Build::new();
    shims.file(&static_fns).std("gnu11").warnings(false);
    for arg in &args {
        shims.flag(arg);
    }
    shims.compile("monado_sys_static_fns");

    println!("cargo:rerun-if-changed=wrapper.h");
    println!("cargo:rerun-if-env-changed=MONADO_BINDGEN_ARGS");
    println!("cargo:rerun-if-changed={}", args_file.display());
}
