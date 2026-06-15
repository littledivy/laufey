{
  description = "laufey";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, rust-overlay }:
    flake-utils.lib.eachSystem [ "x86_64-darwin" "aarch64-darwin" ] (system:
      let
        overlays = [ (import rust-overlay) ];
        pkgs = import nixpkgs {
          inherit system overlays;
        };

        rustToolchain = pkgs.rust-bin.stable.latest.default;

        # Pre-fetch rusty_v8 static library
        rustyV8Version = "145.0.0";
        rustyV8Arch = if system == "aarch64-darwin" then "aarch64-apple-darwin" else "x86_64-apple-darwin";
        rustyV8 = pkgs.fetchurl {
          url = "https://github.com/denoland/rusty_v8/releases/download/v${rustyV8Version}/librusty_v8_release_${rustyV8Arch}.a.gz";
          sha256 = if system == "aarch64-darwin"
            then "sha256-yHa1eydVCrfYGgrZANbzgmmf25p7ui1VMas2A7BhG6k="
            else "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
        };

        helloRuntime = pkgs.rustPlatform.buildRustPackage {
          pname = "hello_runtime";
          version = "0.1.0";
          src = ./.;
          cargoLock.lockFile = ./Cargo.lock;
          buildAndTestSubdir = "examples/hello";
          nativeBuildInputs = [ rustToolchain pkgs.llvmPackages.libclang ];
          LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";
          buildPhase = ''
            cargo build --release -p hello_runtime
          '';
          installPhase = ''
            mkdir -p $out/lib
            cp target/release/libhello_runtime.dylib $out/lib/
          '';
          doCheck = false;
        };

        ddcoreRuntime = pkgs.rustPlatform.buildRustPackage {
          pname = "ddcore_runtime";
          version = "0.1.0";
          src = ./.;
          cargoLock.lockFile = ./Cargo.lock;
          buildAndTestSubdir = "examples/ddcore";
          nativeBuildInputs = [ rustToolchain pkgs.llvmPackages.libclang ];
          buildInputs = [ pkgs.libiconv ];
          LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";
          RUSTY_V8_ARCHIVE = rustyV8;
          buildPhase = ''
            cargo build --release -p ddcore_runtime
          '';
          installPhase = ''
            mkdir -p $out/lib
            cp target/release/libddcore_runtime.dylib $out/lib/
          '';
          doCheck = false;
        };

        presentationRuntime = pkgs.rustPlatform.buildRustPackage {
          pname = "presentation_runtime";
          version = "0.1.0";
          src = ./.;
          cargoLock.lockFile = ./Cargo.lock;
          buildAndTestSubdir = "examples/presentation";
          nativeBuildInputs = [ rustToolchain pkgs.llvmPackages.libclang ];
          LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";
          buildPhase = ''
            cargo build --release -p presentation_runtime
          '';
          installPhase = ''
            mkdir -p $out/lib
            cp target/release/libpresentation_runtime.dylib $out/lib/
          '';
          doCheck = false;
        };

        # Import sub-flakes
        laufeyInclude = ./capi/include;

        cefFlake = import ./cef/flake.nix;
        cefOutputs = cefFlake.outputs { self = cefFlake; inherit nixpkgs flake-utils; inherit laufeyInclude; };
        cefApp = cefOutputs.packages.${system}.default;

        webviewFlake = import ./webview/flake.nix;
        webviewOutputs = webviewFlake.outputs { self = webviewFlake; inherit nixpkgs flake-utils; inherit laufeyInclude; };
        webviewApp = webviewOutputs.packages.${system}.default;

      in {
        packages = {
          hello = helloRuntime;
          ddcore = ddcoreRuntime;

          cef = cefApp;
          webview = webviewApp;

          cef-hello = pkgs.stdenv.mkDerivation {
            pname = "laufey-cef-hello";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib $out/Applications
              cp -R ${cefApp}/Applications/laufey.app $out/Applications/
              cp ${helloRuntime}/lib/libhello_runtime.dylib $out/lib/
              cat > $out/bin/laufey-cef-hello <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec "$SCRIPT_DIR/Applications/laufey.app/Contents/MacOS/laufey" --runtime "$SCRIPT_DIR/lib/libhello_runtime.dylib" "$@"
EOF
              chmod +x $out/bin/laufey-cef-hello
            '';
          };

          cef-ddcore = pkgs.stdenv.mkDerivation {
            pname = "laufey-cef-ddcore";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib $out/share/ddcore $out/Applications
              cp -R ${cefApp}/Applications/laufey.app $out/Applications/
              cp ${ddcoreRuntime}/lib/libddcore_runtime.dylib $out/lib/
              cp ${./examples/ddcore/index.html} $out/share/ddcore/index.html
              cat > $out/bin/laufey-cef-ddcore <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARGS=()
for arg in "$@"; do
  if [[ -f "$arg" ]]; then
    ARGS+=("$(cd "$(dirname "$arg")" && pwd)/$(basename "$arg")")
  else
    ARGS+=("$arg")
  fi
done
cd "$SCRIPT_DIR/share/ddcore"
exec "$SCRIPT_DIR/Applications/laufey.app/Contents/MacOS/laufey" --runtime "$SCRIPT_DIR/lib/libddcore_runtime.dylib" "''${ARGS[@]}"
EOF
              chmod +x $out/bin/laufey-cef-ddcore
            '';
          };

          webview-hello = pkgs.stdenv.mkDerivation {
            pname = "laufey-webview-hello";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib $out/Applications
              cp -R ${webviewApp}/Applications/laufey_webview.app $out/Applications/
              cp ${helloRuntime}/lib/libhello_runtime.dylib $out/lib/
              cat > $out/bin/laufey-webview-hello <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec "$SCRIPT_DIR/Applications/laufey_webview.app/Contents/MacOS/laufey_webview" --runtime "$SCRIPT_DIR/lib/libhello_runtime.dylib" "$@"
EOF
              chmod +x $out/bin/laufey-webview-hello
            '';
          };

          webview-ddcore = pkgs.stdenv.mkDerivation {
            pname = "laufey-webview-ddcore";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib $out/share/ddcore $out/Applications
              cp -R ${webviewApp}/Applications/laufey_webview.app $out/Applications/
              cp ${ddcoreRuntime}/lib/libddcore_runtime.dylib $out/lib/
              cp ${./examples/ddcore/index.html} $out/share/ddcore/index.html
              cat > $out/bin/laufey-webview-ddcore <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARGS=()
for arg in "$@"; do
  if [[ -f "$arg" ]]; then
    ARGS+=("$(cd "$(dirname "$arg")" && pwd)/$(basename "$arg")")
  else
    ARGS+=("$arg")
  fi
done
cd "$SCRIPT_DIR/share/ddcore"
exec "$SCRIPT_DIR/Applications/laufey_webview.app/Contents/MacOS/laufey_webview" --runtime "$SCRIPT_DIR/lib/libddcore_runtime.dylib" "''${ARGS[@]}"
EOF
              chmod +x $out/bin/laufey-webview-ddcore
            '';
          };

          cef-presentation = pkgs.stdenv.mkDerivation {
            pname = "laufey-cef-presentation";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib $out/Applications
              cp -R ${cefApp}/Applications/laufey.app $out/Applications/
              cp ${presentationRuntime}/lib/libpresentation_runtime.dylib $out/lib/
              cat > $out/bin/laufey-cef-presentation <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec "$SCRIPT_DIR/Applications/laufey.app/Contents/MacOS/laufey" --runtime "$SCRIPT_DIR/lib/libpresentation_runtime.dylib" "$@"
EOF
              chmod +x $out/bin/laufey-cef-presentation
            '';
          };

          webview-presentation = pkgs.stdenv.mkDerivation {
            pname = "laufey-webview-presentation";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib $out/Applications
              cp -R ${webviewApp}/Applications/laufey_webview.app $out/Applications/
              cp ${presentationRuntime}/lib/libpresentation_runtime.dylib $out/lib/
              cat > $out/bin/laufey-webview-presentation <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec "$SCRIPT_DIR/Applications/laufey_webview.app/Contents/MacOS/laufey_webview" --runtime "$SCRIPT_DIR/lib/libpresentation_runtime.dylib" "$@"
EOF
              chmod +x $out/bin/laufey-webview-presentation
            '';
          };

          default = helloRuntime;
        };

        devShells.default = pkgs.mkShell {
          buildInputs = [
            rustToolchain
            pkgs.libiconv
            pkgs.cmake
            pkgs.ninja
            pkgs.llvmPackages.libclang
          ] ++ (with pkgs.darwin.apple_sdk.frameworks; [ Cocoa WebKit ]);

          LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";

          shellHook = ''
            echo "laufey dev shell"
            echo "nix build .#cef-hello"
          '';
        };
      }
    );
}
