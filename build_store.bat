@echo off
setlocal

set BUILD_DIR=build_store

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DFICTURE2_DISTRIBUTION_CHANNEL=windows_store
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config MinSizeRel
if errorlevel 1 exit /b 1

endlocal
