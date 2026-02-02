@echo off
setlocal enabledelayedexpansion

echo ========================================
echo FICture2 Winget Release Script
echo ========================================
echo.

set BUILD_DIR=build_winget
set OUTPUT_DIR=%BUILD_DIR%\Output
set INFO_FILE=%BUILD_DIR%\installer_info.txt

REM Check if installer info exists
if not exist "%INFO_FILE%" (
    echo Error: installer_info.txt not found. Run pack_winget.bat first.
    exit /b 1
)

REM Read installer info
set /p INSTALLER_PATH=< "%INFO_FILE%"
set /p SHA256= < "%INFO_FILE%"
for /f "skip=1 tokens=*" %%L in (%INFO_FILE%) do set SHA256=%%L

if not exist "!INSTALLER_PATH!" (
    echo Error: Installer not found at !INSTALLER_PATH!
    exit /b 1
)

echo Installer: !INSTALLER_PATH!
echo SHA256:    !SHA256!
echo.

REM Extract version - try multiple methods
echo Detecting version...
set VERSION=

REM Method 1: Try CMakeCache.txt
echo [1/4] Checking CMakeCache.txt...
for /f "tokens=2 delims==" %%V in ('findstr "FICTURE2_VERSION_STRING:STRING" "%BUILD_DIR%\CMakeCache.txt" 2^>nul') do set VERSION=%%V
if defined VERSION (
    echo       Found: !VERSION!
    goto version_found
)

REM Method 2: Try generated FICture2.iss file
echo [2/4] Checking FICture2.iss...
for /f "tokens=2 delims==" %%V in ('findstr "AppVersion=" "%BUILD_DIR%\FICture2.iss" 2^>nul') do set VERSION=%%V
if defined VERSION (
    echo       Found: !VERSION!
    goto version_found
)

REM Method 3: Extract from installer filename (FICture2-Setup-X.Y.exe)
echo [3/4] Parsing installer filename...
for %%F in ("!INSTALLER_PATH!") do set "FILENAME=%%~nF"
set "VERSION=!FILENAME:FICture2-Setup-=!"
if not "!VERSION!"=="!FILENAME!" (
    echo       Found: !VERSION!
    goto version_found
)
set "VERSION="

REM Method 4: Use Version.h if available
echo [4/4] Checking Version.h...
for /f "tokens=3 delims= " %%V in ('findstr "FICTURE2_VERSION_STRING" "%BUILD_DIR%\Version.h" 2^>nul') do (
    set VERSION=%%V
    set VERSION=!VERSION:"=!
)
if defined VERSION (
    echo       Found: !VERSION!
    goto version_found
)

echo.
echo Error: Could not determine version from any source
echo.
echo Please enter version manually (e.g., 1.1.0.0):
set /p VERSION=Version: 
if not defined VERSION exit /b 1

:version_found
echo.
echo Version: %VERSION%
echo.

REM Check git status
echo Checking git status...
where git > nul 2>&1
if errorlevel 1 (
    echo Error: Git not found in PATH
    echo Please install Git or add it to PATH
    pause
    exit /b 1
)

git status --porcelain > nul 2>&1
if errorlevel 1 (
    echo Error: Not a git repository
    pause
    exit /b 1
)

for /f %%i in ('git status --porcelain ^| find /c /v ""') do set CHANGES=%%i
if !CHANGES! gtr 0 (
    echo Warning: You have uncommitted changes.
    echo.
    git status --short
    echo.
    choice /C YN /M "Do you want to continue anyway?"
    if errorlevel 2 exit /b 1
)

REM Confirm release
echo.
echo ========================================
echo Ready to create GitHub Release
echo ========================================
echo Tag:     v%VERSION%
echo File:    !INSTALLER_PATH!
echo.
choice /C YN /M "Do you want to create the release?"
if errorlevel 2 (
    echo Cancelled.
    exit /b 0
)

REM Check if gh CLI is installed
where gh > nul 2>&1
if errorlevel 1 (
    echo.
    echo Warning: GitHub CLI (gh) not found.
    echo Please install from https://cli.github.com/
    echo.
    echo Manual steps:
    echo 1. Push code: git push origin master
    echo 2. Create tag: git tag v%VERSION%
    echo 3. Push tag: git push origin v%VERSION%
    echo 4. Create release on GitHub with installer
    echo 5. Get release URL and run: update_winget_manifests.bat
    pause
    exit /b 1
)

REM Create and push tag
echo.
echo Creating git tag v%VERSION%...
git tag v%VERSION%
if errorlevel 1 (
    echo Warning: Tag may already exist
)

echo Pushing to origin...
git push origin master
git push origin v%VERSION%
if errorlevel 1 (
    echo Error: Failed to push tag
    exit /b 1
)

REM Create GitHub release
echo.
echo Creating GitHub release...
gh release create v%VERSION% "!INSTALLER_PATH!" ^
    --title "FICture2 v%VERSION%" ^
    --notes "Release v%VERSION%"^
    --latest

if errorlevel 1 (
    echo Error: Failed to create GitHub release
    exit /b 1
)

REM Get release URL
echo.
echo Getting release asset URL...
for /f "delims=" %%U in ('gh release view v%VERSION% --json assets --jq ".assets[0].url"') do set RELEASE_URL=%%U

if not defined RELEASE_URL (
    echo Error: Could not get release URL
    exit /b 1
)

echo Release URL: !RELEASE_URL!
echo.

REM Update CMake cache with URL and SHA256
echo Updating CMake configuration...
cmake -S . -B "%BUILD_DIR%" ^
    -DFICTURE2_WINGET_INSTALLER_URL="!RELEASE_URL!" ^
    -DFICTURE2_WINGET_INSTALLER_SHA256="!SHA256!"

if errorlevel 1 (
    echo Error: Failed to update CMake configuration
    exit /b 1
)

echo.
echo ========================================
echo Release Created Successfully!
echo ========================================
echo Tag:      v%VERSION%
echo URL:      !RELEASE_URL!
echo SHA256:   !SHA256!
echo.
echo Winget manifests have been updated in:
echo %BUILD_DIR%\winget_manifests\
echo.
echo Next step: Run submit_winget.bat to create PR
echo.

pause
endlocal
