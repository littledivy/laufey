SHELL := /bin/bash

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
  HOST_OS := macos
else ifeq ($(UNAME_S),Linux)
  HOST_OS := linux
else
  HOST_OS := windows
endif

ifeq ($(filter arm64 aarch64,$(UNAME_M)),)
  HOST_ARCH := x86_64
else
  HOST_ARCH := aarch64
endif

ifeq ($(HOST_OS),macos)
  ifeq ($(HOST_ARCH),aarch64)
    CEF_ARCH := macosarm64
    PROJ_ARCH := arm64
  else
    CEF_ARCH := macosx64
    PROJ_ARCH := x86_64
  endif
else ifeq ($(HOST_OS),linux)
  ifeq ($(HOST_ARCH),aarch64)
    CEF_ARCH := linuxarm64
  else
    CEF_ARCH := linux64
  endif
  PROJ_ARCH := $(HOST_ARCH)
else ifeq ($(HOST_OS),windows)
  CEF_ARCH := windows64
  PROJ_ARCH := x86_64
endif

ifeq ($(HOST_OS),windows)
  LIB_EXT := .lib
else
  LIB_EXT := .a
endif

CEF_VERSION := 149.0.5
CEF_FULL_VERSION := 149.0.5+g6770623+chromium-149.0.7827.197
CEF_URL_VERSION := $(subst +,%2B,$(CEF_FULL_VERSION))
CEF_DOWNLOAD_URL := https://cef-builds.spotifycdn.com/cef_binary_$(CEF_URL_VERSION)_$(CEF_ARCH)_minimal.tar.bz2
CEF_DIR := $(CURDIR)/vendor/cef
CEF_TARBALL := $(CEF_DIR)/cef-minimal.tar.bz2
CEF_SRC := $(CEF_DIR)/src
CEF_BUILD := $(CEF_DIR)/build
CEF_ROOT := $(CEF_DIR)/install

ifeq ($(HOST_OS),macos)
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
  LLVM_PREFIX := $(BREW_PREFIX)/opt/llvm
  export LIBCLANG_PATH := $(LLVM_PREFIX)/lib
endif

BUILD_DIR := $(CURDIR)/build

.PHONY: all clean check-deps help
.PHONY: runtimes hello-runtime ddcore-runtime
.PHONY: winit webview cef
.PHONY: cef-deps
.PHONY: fmt fmt-check lint

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'

all: runtimes winit webview cef ## Build all backends

check-deps: ## Check that required build tools are installed
	@echo "Checking build dependencies..."
	@command -v cargo >/dev/null || (echo "ERROR: cargo not found. Install Rust: https://rustup.rs" && exit 1)
	@command -v cmake >/dev/null || (echo "ERROR: cmake not found" && exit 1)
	@command -v ninja >/dev/null || (echo "ERROR: ninja not found" && exit 1)
ifeq ($(HOST_OS),macos)
	@test -d "$(LLVM_PREFIX)" || (echo "ERROR: llvm not found. Run: brew install llvm" && exit 1)
endif
	@echo "All base dependencies OK."

runtimes: hello-runtime ddcore-runtime ## Build all example runtimes

hello-runtime: check-deps ## Build hello_runtime
	cargo build --release -p hello_runtime

ddcore-runtime: check-deps ## Build ddcore_runtime
	cargo build --release -p ddcore_runtime

winit: check-deps ## Build the winit backend
	cd winit && cargo build --release

webview: check-deps ## Build the webview backend
	cmake -G Ninja -B webview/build -S webview -DCMAKE_BUILD_TYPE=Release \
		$(if $(WEBVIEW2_ROOT),-DWEBVIEW2_ROOT=$(WEBVIEW2_ROOT))
	ninja -C webview/build

$(CEF_TARBALL):
	@echo "Downloading CEF $(CEF_VERSION) for $(CEF_ARCH)..."
	@mkdir -p $(CEF_DIR)
	curl -L -o $(CEF_TARBALL) "$(CEF_DOWNLOAD_URL)"

$(CEF_SRC)/CMakeLists.txt: $(CEF_TARBALL)
	@echo "Extracting CEF..."
	@mkdir -p $(CEF_SRC)
	tar xf $(CEF_TARBALL) --strip-components=1 -C $(CEF_SRC)

$(CEF_ROOT)/libcef_dll_wrapper/libcef_dll_wrapper$(LIB_EXT): $(CEF_SRC)/CMakeLists.txt
	@echo "Building CEF dll_wrapper..."
	cmake -G Ninja -B $(CEF_BUILD) -S $(CEF_SRC) \
		-DCMAKE_BUILD_TYPE=Release \
		-DPROJECT_ARCH=$(PROJ_ARCH) \
		-DUSE_SANDBOX=OFF
	ninja -C $(CEF_BUILD) libcef_dll_wrapper
	@echo "Installing CEF..."
	@mkdir -p $(CEF_ROOT)/include $(CEF_ROOT)/cmake $(CEF_ROOT)/Release $(CEF_ROOT)/libcef_dll_wrapper
	cp -r $(CEF_SRC)/include/* $(CEF_ROOT)/include/
	cp -r $(CEF_SRC)/cmake/* $(CEF_ROOT)/cmake/
	cp -r $(CEF_SRC)/Release/* $(CEF_ROOT)/Release/
	@if [ -d "$(CEF_SRC)/Resources" ]; then \
		mkdir -p $(CEF_ROOT)/Resources && \
		cp -r $(CEF_SRC)/Resources/* $(CEF_ROOT)/Resources/; \
	fi
	cp $(CEF_BUILD)/libcef_dll_wrapper/libcef_dll_wrapper$(LIB_EXT) $(CEF_ROOT)/libcef_dll_wrapper/

cef-deps: $(CEF_ROOT)/libcef_dll_wrapper/libcef_dll_wrapper$(LIB_EXT) ## Download and build CEF dependencies

cef: check-deps cef-deps ## Build the CEF backend
	cmake -G Ninja -B cef/build -S cef \
		-DCMAKE_BUILD_TYPE=Release \
		-DCEF_ROOT=$(CEF_ROOT) \
		-DPROJECT_ARCH=$(PROJ_ARCH)
	ninja -C cef/build

CPP_SOURCES := $(shell find capi cef/src webview/src -name '*.cc' -o -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.mm' 2>/dev/null)

fmt: ## Format all source files
	cargo fmt
	deno fmt
	@if command -v clang-format >/dev/null 2>&1 && [ -n "$(CPP_SOURCES)" ]; then \
		clang-format -i $(CPP_SOURCES); \
	fi

fmt-check: ## Check formatting without modifying files
	cargo fmt --check
	deno fmt --check
	@if command -v clang-format >/dev/null 2>&1 && [ -n "$(CPP_SOURCES)" ]; then \
		clang-format --dry-run --Werror $(CPP_SOURCES); \
	fi

lint: ## Run all linters
	cargo clippy --workspace -- -D warnings
	deno lint

clean: ## Remove all build artifacts
	cargo clean
	rm -rf webview/build cef/build

clean-cef-vendor: ## Remove downloaded CEF files
	rm -rf vendor/cef
