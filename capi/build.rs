// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::env;
use std::path::PathBuf;

fn main() {
  let header = "include/laufey.h";
  println!("cargo:rerun-if-changed={}", header);

  let mut builder = bindgen::Builder::default()
    .header(header)
    .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
    .allowlist_type("laufey_.*")
    .allowlist_var("LAUFEY_.*");

  // MSVC's bundled clang headers conflict with the system LLVM clang headers.
  // Add compatibility flags to resolve wchar_t typedef redefinition and
  // __declspec attribute errors.
  if cfg!(target_os = "windows") {
    // Pass the real target triple so clang parses with the correct arch on both
    // x64 and ARM64 Windows (aarch64-pc-windows-msvc). Both are LLP64 so the
    // generated layouts match, but hardcoding x86_64 was still wrong.
    let target_arch =
      env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_else(|_| "x86_64".to_string());
    let clang_arch = if target_arch == "x86" {
      "i686"
    } else {
      target_arch.as_str()
    };
    builder = builder
      .clang_arg("-fms-extensions")
      .clang_arg("-fms-compatibility")
      .clang_arg("-fdelayed-template-parsing")
      .clang_arg("-fmsc-version=1950")
      .clang_arg("-Wno-everything")
      .clang_arg(format!("--target={clang_arch}-pc-windows-msvc"))
      .clang_arg("-D_WCHAR_T_DEFINED")
      .clang_arg("-D_NATIVE_WCHAR_T_DEFINED");
  }

  let bindings = builder.generate().expect("Unable to generate bindings");

  let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
  bindings
    .write_to_file(out_path.join("bindings.rs"))
    .expect("Couldn't write bindings!");
}
