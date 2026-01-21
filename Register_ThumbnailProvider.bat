@echo off
setlocal

set "SCRIPT_DIR=%~dp0"

if /i "%~1"=="/u" goto :set_unreg
if /i "%~1"=="unregister" goto :set_unreg
goto :set_reg

:set_unreg
set "ACTION=unregister"
shift
goto :admin_check

:set_reg
set "ACTION=register"

:admin_check
net session >nul 2>&1
if errorlevel 1 (
    echo [FICture2] Administrator rights are required.
    echo [FICture2] Attempting to relaunch with elevation...
    if "%~1"=="" (
        powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    ) else (
        powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs"
    )
    echo [FICture2] If you declined the UAC prompt, please run this script as Administrator.
    exit /b 1
)

set "DLL=%SCRIPT_DIR%ThumbnailProvider.dll"

if not exist "%DLL%" (
    echo [FICture2] ThumbnailProvider.dll not found.
    echo [FICture2] Place ThumbnailProvider.dll next to this script.
    echo.
    echo Expected location:
    echo   %SCRIPT_DIR%ThumbnailProvider.dll
    exit /b 1
)

echo [FICture2] Using: "%DLL%"
if /i "%ACTION%"=="unregister" (
    regsvr32 /u "%DLL%"
) else (
    regsvr32 "%DLL%"
)

if errorlevel 1 (
    echo [FICture2] Operation failed.
    exit /b 1
)

echo [FICture2] Operation completed successfully.
exit /b 0
