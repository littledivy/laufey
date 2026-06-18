{
  description = "WEF Servo Backend";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    rust-overlay.url = "github:oxalica/rust-overlay";
  };

  outputs = { self, nixpkgs, flake-utils, rust-overlay }:
    flake-utils.lib.eachSystem [ "x86_64-darwin" "aarch64-darwin" "x86_64-linux" "aarch64-linux" ] (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ rust-overlay.overlays.default ];
        };

        isDarwin = pkgs.stdenv.isDarwin;
        isLinux = pkgs.stdenv.isLinux;

        llvmPackages = pkgs.llvmPackages_18;

        rustToolchain = pkgs.rust-bin.stable."1.91.0".default.override {
          extensions = [ "rust-src" "rust-analyzer" ];
        };

        buildInputs = with pkgs; [
          fontconfig
          freetype
          libunwind
        ] ++ pkgs.lib.optionals isDarwin [
          apple-sdk_15
          libiconv
        ] ++ pkgs.lib.optionals isLinux [
          xorg.libxcb
          xorg.libX11
          xorg.libXcursor
          xorg.libXrandr
          xorg.libXi
          libxkbcommon
          dbus
          udev
          vulkan-loader
          libGL
          gst_all_1.gstreamer
          gst_all_1.gst-plugins-base
          gst_all_1.gst-plugins-good
          gst_all_1.gst-plugins-bad
          gst_all_1.gst-plugins-ugly
        ];

        nativeBuildInputs = with pkgs; [
          rustToolchain
          cmake
          pkg-config
          python311
          perl
          git
          m4
          yasm
          llvmPackages.llvm
          llvmPackages.clang
          cacert
          gnumake
        ] ++ pkgs.lib.optionals isLinux [ gcc ];

        servoBackend = pkgs.stdenv.mkDerivation {
          pname = "wef-servo";
          version = "0.1.0";

          src = ./.;

          inherit buildInputs nativeBuildInputs;

          LIBCLANG_PATH = pkgs.lib.makeLibraryPath [ llvmPackages.clang-unwrapped.lib ];
          SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";

          # Disable sandbox - required for mozjs prebuilt downloads and aws-lc-sys assembly
          __noChroot = true;
          __darwinAllowLocalNetworking = true;

          dontUpdateAutotoolsGnuConfigScripts = true;
          dontUseCmakeConfigure = true;
          dontConfigure = true;
          dontFixup = true;

          buildPhase = ''
            export PATH="${rustToolchain}/bin:/usr/bin:$PATH"
            export HOME=$TMPDIR
            export CARGO_HOME=$TMPDIR/cargo
            mkdir -p $CARGO_HOME
            # Use system clang to avoid nix wrapper issues with assembly
            export CC=/usr/bin/clang
            export CXX=/usr/bin/clang++
            cargo build --release
          '';

          installPhase = ''
            mkdir -p $out/bin
            cp target/release/wef_servo $out/bin/
          '';

          meta.timeout = 14400;
        };

      in {
        packages = {
          default = servoBackend;
          servo = servoBackend;
        };

        devShells.default = pkgs.mkShell {
          inherit buildInputs nativeBuildInputs;

          LIBCLANG_PATH = pkgs.lib.makeLibraryPath [ llvmPackages.clang-unwrapped.lib ];
          SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";

          LD_LIBRARY_PATH = pkgs.lib.optionalString isLinux (pkgs.lib.makeLibraryPath [
            pkgs.xorg.libXcursor
            pkgs.xorg.libXrandr
            pkgs.xorg.libXi
            pkgs.libxkbcommon
            pkgs.vulkan-loader
            pkgs.libGL
          ]);

          shellHook = ''
            echo "WEF Servo Backend"
            echo "Build: cargo build"
            echo "Run:   WEF_RUNTIME_PATH=<path> cargo run"
          '';
        };
      }
    );
}
