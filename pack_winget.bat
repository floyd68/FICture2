@echo off
setlocal enabledelayedexpansion

echo ========================================
echo FICture2 Winget Packaging Script
echo ========================================
echo.

set BUILD_DIR=build_winget
set OUTPUT_DIR=%BUILD_DIR%\Output
set INNO_SCRIPT=%BUILD_DIR%\FICture2.iss

REM Check if build directory exists
if not exist "%BUILD_DIR%" (
    echo Error: Build directory not found. Run build_winget.bat first.
    exit /b 1
)

REM Check if Inno Setup script exists
if not exist "%INNO_SCRIPT%" (
    echo Error: Inno Setup script not found at %INNO_SCRIPT%
    exit /b 1
)

REM Find Inno Setup Compiler
set ISCC=
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" (
    set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
) else if exist "C:\Program Files\Inno Setup 6\ISCC.exe" (
    set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
) else (
    echo Error: Inno Setup Compiler not found.
    echo Please install Inno Setup 6 from https://jrsoftware.org/isdl.php
    exit /b 1
)

echo Found Inno Setup Compiler: %ISCC%
echo.

REM Run Inno Setup Compiler
echo Compiling installer...
"%ISCC%" "%INNO_SCRIPT%"
if errorlevel 1 (
    echo Error: Inno Setup compilation failed.
    exit /b 1
)

REM Find the generated installer
echo.
echo Searching for generated installer...
set INSTALLER=
for %%F in ("%OUTPUT_DIR%\*.exe") do (
    set "INSTALLER=%%F"
    echo Found: %%F
)

if not defined INSTALLER (
    echo Error: Installer not found in %OUTPUT_DIR%
    exit /b 1
)

REM Calculate SHA256
echo.
echo Calculating SHA256 hash...
for /f "skip=1 tokens=*" %%H in ('certutil -hashfile "!INSTALLER!" SHA256') do (
    if not defined SHA256 (
        set "SHA256=%%H"
        set "SHA256=!SHA256: =!"
    )
)

echo.
echo ========================================
echo Packaging Complete!
echo ========================================
echo Installer: !INSTALLER!
echo SHA256:    !SHA256!
echo.
echo Save this information for release_winget.bat
echo.

REM Save info to a temp file for automation
echo !INSTALLER!> "%BUILD_DIR%\installer_info.txt"
echo !SHA256!>> "%BUILD_DIR%\installer_info.txt"

echo Press any key to test the installer manually...
pause > nul

endlocal
