@echo off
setlocal
rem First arg selects the vcvarsall target arch (default x64). Use "arm64" on a
rem native ARM64 host or "amd64_arm64" to cross-compile from x64. CMake picks the
rem matching WebView2 loader dir from the resulting toolchain automatically.
set "VS_ARCH=%~1"
if "%VS_ARCH%"=="" set "VS_ARCH=x64"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" %VS_ARCH%
if errorlevel 1 (
    echo Failed to set up VS environment
    exit /b 1
)

cd /d "%~dp0"
if exist build rmdir /s /q build

cmake -S . -B build -G Ninja ^
  -DWEBVIEW2_ROOT="%~dp0packages\Microsoft.Web.WebView2.1.0.3856.49" ^
  -DCMAKE_CXX_FLAGS="/I%~dp0packages\Microsoft.Windows.ImplementationLibrary.1.0.260126.7\include"
if errorlevel 1 (
    echo CMake configure failed
    exit /b 1
)

cmake --build build
if errorlevel 1 (
    echo Build failed
    exit /b 1
)

echo Build succeeded!
