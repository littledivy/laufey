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
    builder = builder
      .clang_arg("-fms-extensions")
      .clang_arg("-fms-compatibility")
      .clang_arg("-fdelayed-template-parsing")
      .clang_arg("-fmsc-version=1950")
      .clang_arg("-Wno-everything")
      .clang_arg("--target=x86_64-pc-windows-msvc")
      .clang_arg("-D_WCHAR_T_DEFINED")
      .clang_arg("-D_NATIVE_WCHAR_T_DEFINED");
  }

  let bindings = builder.generate().expect("Unable to generate bindings");

  let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
  bindings
    .write_to_file(out_path.join("bindings.rs"))
    .expect("Couldn't write bindings!");
}
