@echo off
setlocal

set BUILD_DIR=build_winget

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

REM Releases are built by .github/workflows/release.yml; this is the local fallback.
REM --fresh: a build directory configured with another Visual Studio is reconfigured.
cmake --fresh -S . -B "%BUILD_DIR%" -G "Visual Studio 18 2026" -A x64 -DFICTURE2_DISTRIBUTION_CHANNEL=winget -DBUILD_DX12=OFF
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config MinSizeRel
if errorlevel 1 exit /b 1

endlocal
