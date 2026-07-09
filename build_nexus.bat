@echo off
setlocal

set BUILD_DIR=build_nexus

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 18 2026" -A x64 -DFICTURE2_DISTRIBUTION_CHANNEL=nexus_github
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config MinSizeRel
if errorlevel 1 exit /b 1

endlocal
