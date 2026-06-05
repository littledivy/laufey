@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

cd /d "%~dp0"
if exist build rmdir /s /q build

cmake -G Ninja -B build -S . ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCEF_ROOT="C:/Users/dhairy/laufey/vendor/cef/install"
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
