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
echo [DEBUG] Checking installer info file: %INFO_FILE%
if not exist "%INFO_FILE%" (
    echo Error: installer_info.txt is missing. Run pack_winget.bat first.
    exit /b 1
)
echo [DEBUG] Installer info file found

REM Read installer info
echo [DEBUG] Reading installer info...
set /p INSTALLER_PATH=< "%INFO_FILE%"
set /p SHA256= < "%INFO_FILE%"
for /f "skip=1 tokens=*" %%L in (%INFO_FILE%) do set SHA256=%%L
echo [DEBUG] INSTALLER_PATH=!INSTALLER_PATH!
echo [DEBUG] SHA256=!SHA256!

echo [DEBUG] Checking if installer exists...
if not exist "!INSTALLER_PATH!" (
    echo Error: Installer is missing at !INSTALLER_PATH!
    exit /b 1
)
echo [DEBUG] Installer found

echo Installer: !INSTALLER_PATH!
echo SHA256:    !SHA256!
echo.

REM Extract version - try multiple methods
echo Detecting version...
echo [DEBUG] Starting version detection...
set VERSION=

REM Method 1: Try CMakeCache.txt
echo [1/4] Checking CMakeCache.txt...
echo [DEBUG] Method 1 starting...
for /f "tokens=2 delims==" %%V in ('findstr "FICTURE2_VERSION_STRING:STRING" "%BUILD_DIR%\CMakeCache.txt" 2^>nul') do set VERSION=%%V
echo [DEBUG] Method 1 result: VERSION=!VERSION!
if defined VERSION (
    echo       Found: !VERSION!
    echo [DEBUG] Jumping to version_found
    goto version_found
)
echo [DEBUG] Method 1 failed, continuing...

REM Method 2: Try generated FICture2.iss file
echo [2/4] Checking FICture2.iss...
echo [DEBUG] Method 2 starting...
for /f "tokens=2 delims==" %%V in ('findstr "AppVersion=" "%BUILD_DIR%\FICture2.iss" 2^>nul') do set VERSION=%%V
echo [DEBUG] Method 2 result: VERSION=!VERSION!
if defined VERSION (
    echo       Found: !VERSION!
    echo [DEBUG] Jumping to version_found
    goto version_found
)
echo [DEBUG] Method 2 failed, continuing...

REM Method 3: Extract from installer filename (FICture2-Setup-X.Y.exe)
echo [3/4] Parsing installer filename...
echo [DEBUG] Method 3 starting...
echo [DEBUG] INSTALLER_PATH=!INSTALLER_PATH!
for %%F in ("!INSTALLER_PATH!") do set "FILENAME=%%~nF"
echo [DEBUG] FILENAME=!FILENAME!
set "VERSION=!FILENAME:FICture2-Setup-=!"
echo [DEBUG] VERSION after substitution=!VERSION!
echo [DEBUG] Comparing VERSION=!VERSION! with FILENAME=!FILENAME!
if "!VERSION!" NEQ "!FILENAME!" (
    echo       Found: !VERSION!
    echo [DEBUG] Jumping to version_found
    goto version_found
)
echo [DEBUG] Method 3 failed (no substitution occurred)
set "VERSION="

REM Method 4: Use Version.h if available
echo [4/4] Checking Version.h...
echo [DEBUG] Method 4 starting...
for /f "tokens=3 delims= " %%V in ('findstr "FICTURE2_VERSION_STRING" "%BUILD_DIR%\Version.h" 2^>nul') do (
    set VERSION=%%V
    set VERSION=!VERSION:"=!
)
echo [DEBUG] Method 4 result: VERSION=!VERSION!
if defined VERSION (
    echo       Found: !VERSION!
    echo [DEBUG] Jumping to version_found
    goto version_found
)
echo [DEBUG] Method 4 failed

echo.
echo Error: Unable to determine version from any source
echo.
echo Please enter version manually (e.g., 1.1.0.0):
set /p VERSION=Version: 
if not defined VERSION (
    exit /b 1
)

:version_found
echo [DEBUG] At version_found label
echo [DEBUG] Final VERSION=!VERSION!
echo.
echo Version: !VERSION!
echo.

REM Check git status
echo [DEBUG] Starting git status check...
echo Checking git status...
where git > nul 2>&1
if errorlevel 1 (
    echo Error: Git is missing from PATH
    echo Please install Git or add it to PATH
    pause
    exit /b 1
)

git status --porcelain > nul 2>&1
if errorlevel 1 (
    echo Error: This is ^not a git repository
    pause
    exit /b 1
)

echo [DEBUG] Counting uncommitted changes...
for /f %%i in ('git status --porcelain ^| find /c /v ""') do set CHANGES=%%i
echo [DEBUG] CHANGES=!CHANGES!
if !CHANGES! gtr 0 (
    echo Warning: You have uncommitted changes.
    echo.
    git status --short
    echo.
    echo [DEBUG] Asking user for confirmation...
    choice /C YN /M "Do you want to continue anyway?"
    echo [DEBUG] User choice: errorlevel=!errorlevel!
    if errorlevel 2 (
        exit /b 1
    )
)
echo [DEBUG] Git status check passed

REM Confirm release
echo [DEBUG] Preparing release confirmation...
echo.
echo ========================================
echo Ready to create GitHub Release
echo ========================================
echo Tag:     v!VERSION!
echo File:    !INSTALLER_PATH!
echo.
echo [DEBUG] Asking user for release confirmation...
choice /C YN /M "Do you want to create the release?"
echo [DEBUG] User choice: errorlevel=!errorlevel!
if errorlevel 2 (
    echo Cancelled.
    exit /b 0
)
echo [DEBUG] User confirmed release

REM Check if gh CLI is installed
echo [DEBUG] Checking for gh CLI...
where gh > nul 2>&1
echo [DEBUG] gh check errorlevel=!errorlevel!
if errorlevel 1 (
    echo.
    echo Warning: GitHub CLI ^(gh^) is missing.
    echo Please install from https://cli.github.com/
    echo.
    echo Manual steps:
    echo 1. Push code: git push origin master
    echo 2. Create tag: git tag v!VERSION!
    echo 3. Push tag: git push origin v!VERSION!
    echo 4. Create release on GitHub with installer
    echo 5. Get release URL and run: update_winget_manifests.bat
    pause
    exit /b 1
)

REM Create and push tag
echo [DEBUG] Starting git operations...
echo.
echo Creating git tag v!VERSION!...
echo [DEBUG] Running: git tag v!VERSION!
git tag v!VERSION!
if errorlevel 1 (
    echo Warning: Tag may already exist
)

echo Pushing to origin...
git push origin master
git push origin v!VERSION!
if errorlevel 1 (
    echo Error: Failed to push tag
    exit /b 1
)

REM Create GitHub release
echo [DEBUG] About to create GitHub release...
echo [DEBUG] VERSION=!VERSION!
echo [DEBUG] INSTALLER_PATH=!INSTALLER_PATH!
echo.
echo Creating GitHub release...
echo [DEBUG] Running gh release create command...
gh release create v!VERSION! "!INSTALLER_PATH!" ^
    --title "FICture2 v!VERSION!" ^
    --notes "Release v!VERSION!" ^
    --latest
echo [DEBUG] gh release create completed with errorlevel=!errorlevel!

if errorlevel 1 (
    echo Error: Failed to create GitHub release
    exit /b 1
)

REM Get release URL
echo [DEBUG] Getting release URL...
echo.
echo Getting release asset URL...
echo [DEBUG] Running: gh release view v!VERSION!
for /f "delims=" %%U in ('gh release view v!VERSION! --json assets --jq ".assets[0].url"') do set RELEASE_URL=%%U
echo [DEBUG] RELEASE_URL=!RELEASE_URL!

echo [DEBUG] Checking if RELEASE_URL is defined...
if not defined RELEASE_URL (
    echo Error: Unable to get release URL
    exit /b 1
)
echo [DEBUG] RELEASE_URL is valid

echo Release URL: !RELEASE_URL!
echo.

REM Update CMake cache with URL and SHA256
echo [DEBUG] Updating CMake configuration...
echo Updating CMake configuration...
echo [DEBUG] Running cmake with RELEASE_URL=!RELEASE_URL! SHA256=!SHA256!
cmake -S . -B "%BUILD_DIR%" ^
    -DFICTURE2_WINGET_INSTALLER_URL="!RELEASE_URL!" ^
    -DFICTURE2_WINGET_INSTALLER_SHA256="!SHA256!"

echo [DEBUG] CMake completed with errorlevel=!errorlevel!
if errorlevel 1 (
    echo Error: Failed to update CMake configuration
    exit /b 1
)
echo [DEBUG] CMake configuration updated successfully

echo.
echo ========================================
echo Release Created Successfully!
echo ========================================
echo Tag:      v!VERSION!
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
