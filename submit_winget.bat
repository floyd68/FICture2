@echo off
setlocal enabledelayedexpansion

echo ========================================
echo FICture2 Winget Submission Script
echo ========================================
echo.

set BUILD_DIR=build_winget
set MANIFEST_DIR=%BUILD_DIR%\winget_manifests
set WINGET_PKGS_DIR=winget-pkgs
set PACKAGE_ID=floyd68.FICture2

REM Check if manifests exist
if not exist "%MANIFEST_DIR%" (
    echo Error: Manifest directory not found. Run release_winget.bat first.
    exit /b 1
)

REM Count manifest files
set COUNT=0
for %%F in ("%MANIFEST_DIR%\*.yaml") do set /a COUNT+=1

if !COUNT! neq 3 (
    echo Error: Expected 3 manifest files, found !COUNT!
    echo Make sure you have:
    echo - %PACKAGE_ID%.yaml
    echo - %PACKAGE_ID%.installer.yaml
    echo - %PACKAGE_ID%.locale.en-US.yaml
    exit /b 1
)

echo Found manifest files:
dir /b "%MANIFEST_DIR%\*.yaml"
echo.

REM Extract version
for /f "tokens=2 delims==" %%V in ('findstr "FICTURE2_VERSION_STRING:STRING" "%BUILD_DIR%\CMakeCache.txt"') do set VERSION=%%V

if not defined VERSION (
    echo Error: Could not determine version
    exit /b 1
)

echo Version: %VERSION%
echo.

REM Validate manifests using winget
echo Validating manifests...
where winget > nul 2>&1
if not errorlevel 1 (
    winget validate "%MANIFEST_DIR%"
    if errorlevel 1 (
        echo Error: Manifest validation failed
        echo Please fix the errors before submitting
        pause
        exit /b 1
    )
    echo Validation passed!
    echo.
) else (
    echo Warning: winget not found, skipping validation
    echo.
)

REM Check if winget-pkgs exists
if not exist "%WINGET_PKGS_DIR%" (
    echo Winget-pkgs repository not found.
    echo.
    choice /C YN /M "Do you want to clone it now? (This may take a while)"
    if errorlevel 2 (
        echo.
        echo Manual steps:
        echo 1. Fork https://github.com/microsoft/winget-pkgs on GitHub
        echo 2. Clone your fork: git clone https://github.com/YOUR_USERNAME/winget-pkgs
        echo 3. Run this script again
        pause
        exit /b 0
    )
    
    echo.
    echo Please enter your GitHub username:
    set /p GITHUB_USER=Username: 
    
    echo Cloning winget-pkgs fork...
    git clone https://github.com/!GITHUB_USER!/winget-pkgs.git
    if errorlevel 1 (
        echo Error: Failed to clone repository
        echo Make sure you have forked microsoft/winget-pkgs first
        pause
        exit /b 1
    )
)

REM Enter winget-pkgs directory
cd "%WINGET_PKGS_DIR%"

REM Sync with upstream
echo.
echo Syncing with upstream...
git remote add upstream https://github.com/microsoft/winget-pkgs.git 2> nul
git fetch upstream
git checkout master
git merge upstream/master
if errorlevel 1 (
    echo Warning: Merge conflict detected. Please resolve manually.
    pause
)

REM Create new branch
set BRANCH_NAME=%PACKAGE_ID%-%VERSION%
echo.
echo Creating branch: %BRANCH_NAME%
git checkout -b %BRANCH_NAME%
if errorlevel 1 (
    echo Warning: Branch may already exist
    git checkout %BRANCH_NAME%
)

REM Create package directory structure
set PKG_DIR=manifests\f\floyd68\FICture2\%VERSION%
echo Creating directory: %PKG_DIR%
if not exist "%PKG_DIR%" mkdir "%PKG_DIR%"

REM Copy manifest files
echo Copying manifest files...
copy /Y "..\%MANIFEST_DIR%\%PACKAGE_ID%.yaml" "%PKG_DIR%\"
copy /Y "..\%MANIFEST_DIR%\%PACKAGE_ID%.installer.yaml" "%PKG_DIR%\"
copy /Y "..\%MANIFEST_DIR%\%PACKAGE_ID%.locale.en-US.yaml" "%PKG_DIR%\"

if errorlevel 1 (
    echo Error: Failed to copy manifest files
    cd ..
    exit /b 1
)

echo.
echo Files copied to %PKG_DIR%
dir /b "%PKG_DIR%"
echo.

REM Commit changes
echo Committing changes...
git add "%PKG_DIR%"
git commit -m "New version: %PACKAGE_ID% version %VERSION%"
if errorlevel 1 (
    echo Error: Commit failed
    cd ..
    exit /b 1
)

REM Check if gh CLI is available
where gh > nul 2>&1
if errorlevel 1 (
    echo.
    echo ========================================
    echo Manual PR Submission Required
    echo ========================================
    echo.
    echo GitHub CLI not found. Please complete manually:
    echo.
    echo 1. Push branch:
    echo    git push origin %BRANCH_NAME%
    echo.
    echo 2. Create PR on GitHub:
    echo    https://github.com/microsoft/winget-pkgs/compare
    echo.
    echo 3. In PR description, include:
    echo    - Tested installation
    echo    - Tested uninstallation
    echo    - Link to release
    echo.
    cd ..
    pause
    exit /b 0
)

REM Push and create PR
echo.
choice /C YN /M "Do you want to push and create PR now?"
if errorlevel 2 (
    echo.
    echo Branch created. You can push manually:
    echo git push origin %BRANCH_NAME%
    cd ..
    pause
    exit /b 0
)

echo Pushing branch...
git push -u origin %BRANCH_NAME%
if errorlevel 1 (
    echo Error: Failed to push branch
    cd ..
    exit /b 1
)

echo Creating pull request...
gh pr create --repo microsoft/winget-pkgs ^
    --title "New version: %PACKAGE_ID% version %VERSION%" ^
    --body "- Tested installation and uninstallation - Release: https://github.com/floyd68/FICture2/releases/tag/v%VERSION%"

if errorlevel 1 (
    echo Error: Failed to create PR
    echo Please create PR manually at:
    echo https://github.com/microsoft/winget-pkgs/compare
) else (
    echo.
    echo ========================================
    echo PR Created Successfully!
    echo ========================================
)

cd ..
echo.
pause
endlocal
