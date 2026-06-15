{
  description = "LAUFEY - Web Engine Framework for macOS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, laufeyInclude ? null }:
    flake-utils.lib.eachSystem [ "x86_64-darwin" "aarch64-darwin" ] (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        arch = if system == "aarch64-darwin" then "macosarm64" else "macosx64";

        cefVersion = "144.0.11";
        cefFullVersion = "144.0.11+ge135be2+chromium-144.0.7559.97";

        cefUrlVersion = builtins.replaceStrings ["+"] ["%2B"] cefFullVersion;

        cefSrc = pkgs.fetchurl {
          url = "https://cef-builds.spotifycdn.com/cef_binary_${cefUrlVersion}_${arch}_minimal.tar.bz2";
          name = "cef-minimal.tar.bz2";
          hash = if system == "aarch64-darwin"
            then "sha256-Q94fht0yAkwAIqv29I1ZkpaS7WiYvJIcHWFcmBSvqHw="
            else "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
        };

        cef = pkgs.stdenv.mkDerivation {
          pname = "cef";
          version = cefVersion;

          src = cefSrc;
          sourceRoot = ".";

          dontConfigure = true;

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
          ];

          buildInputs = with pkgs; [
            apple-sdk_15
          ];

          unpackPhase = ''
            runHook preUnpack
            tar xf $src
            mv cef_binary_* source
            runHook postUnpack
          '';

          buildPhase = ''
            runHook preBuild
            cd source
            mkdir -p build
            cd build
            cmake -G Ninja \
              -DCMAKE_BUILD_TYPE=Release \
              -DPROJECT_ARCH=${if system == "aarch64-darwin" then "arm64" else "x86_64"} \
              -DUSE_SANDBOX=OFF \
              ..
            ninja libcef_dll_wrapper
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            cd ..
            mkdir -p $out

            cp -r include $out/
            cp -r cmake $out/

            mkdir -p $out/Release
            cp -r Release/* $out/Release/

            mkdir -p $out/libcef_dll_wrapper
            cp build/libcef_dll_wrapper/libcef_dll_wrapper.a $out/libcef_dll_wrapper/
            runHook postInstall
          '';
        };

        combinedSrc = ./.;

      in {
        packages = {
          cef = cef;

          default = pkgs.stdenv.mkDerivation {
            pname = "laufey";
            version = "1.0.0";

            src = combinedSrc;

            dontConfigure = true;

            nativeBuildInputs = with pkgs; [
              cmake
              ninja
            ];

            buildInputs = with pkgs; [
              apple-sdk_15
            ];

            buildPhase = ''
              runHook preBuild
              ${if laufeyInclude != null then ''
                mkdir -p include
                cp ${laufeyInclude}/laufey.h include/
              '' else ""}
              cmake -G Ninja \
                -DCMAKE_BUILD_TYPE=Release \
                -DCEF_ROOT=${cef} \
                ${if laufeyInclude != null then "-DLAUFEY_INCLUDE_DIR=$PWD/include" else ""} \
                .
              ninja
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/Applications
              cp -r Release/laufey.app $out/Applications/
              runHook postInstall
            '';
          };
        };

        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            cmake
            ninja
            apple-sdk_15
          ];

          CEF_ROOT = cef;

          shellHook = ''
            echo "LAUFEY Development Shell"
            echo "CEF_ROOT: ${cef}"
          '';
        };
      }
    );
}
