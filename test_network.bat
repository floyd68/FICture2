@echo off
REM ========================================
REM FICture2 Network Activity Test Script
REM ========================================

echo.
echo ========================================
echo FICture2 Network Test
echo ========================================
echo.

REM Check if executable exists
set "EXE_PATH=build_winget\bin\RelWithDebInfo\FICture2.exe"
if not exist "%EXE_PATH%" (
    echo ERROR: FICture2.exe not found at %EXE_PATH%
    echo.
    echo Please build first using: build_winget.bat
    echo.
    pause
    exit /b 1
)

echo Testing executable: %EXE_PATH%
echo.
echo ========================================
echo Test Options:
echo ========================================
echo 1. Simple netstat check (before/after)
echo 2. Continuous monitoring with netstat
echo 3. Open Resource Monitor (manual check)
echo 4. Instructions for Process Monitor
echo.
set /p choice="Select option (1-4): "

if "%choice%"=="1" goto simple
if "%choice%"=="2" goto continuous
if "%choice%"=="3" goto resmon
if "%choice%"=="4" goto procmon
goto end

:simple
echo.
echo ========================================
echo Simple Network Check
echo ========================================
echo.
echo [1/3] Checking network connections BEFORE starting FICture2...
netstat -ano | findstr "ESTABLISHED" > before.txt
echo       Saved to: before.txt
echo.
echo [2/3] Starting FICture2.exe...
echo       Please use the app (open images, use menus, etc.)
echo       Press any key when done testing...
start "" "%EXE_PATH%"
pause
echo.
echo [3/3] Checking network connections AFTER...
netstat -ano | findstr "ESTABLISHED" > after.txt
echo       Saved to: after.txt
echo.
echo Comparing results...
fc before.txt after.txt
echo.
echo Check the files manually if needed:
echo   - before.txt
echo   - after.txt
echo.
pause
goto end

:continuous
echo.
echo ========================================
echo Continuous Monitoring
echo ========================================
echo.
echo Starting FICture2.exe...
start "" "%EXE_PATH%"
echo.
echo Monitoring network connections (Ctrl+C to stop)...
echo.
:loop
netstat -ano | findstr "FICture2"
timeout /t 2 /nobreak >nul
goto loop

:resmon
echo.
echo ========================================
echo Resource Monitor
echo ========================================
echo.
echo Opening Resource Monitor...
echo.
echo Manual steps:
echo   1. Go to "Network" tab
echo   2. Look for "FICture2.exe" in processes
echo   3. Check "TCP Connections" section
echo.
start resmon
echo.
echo Now starting FICture2.exe...
start "" "%EXE_PATH%"
echo.
pause
goto end

:procmon
echo.
echo ========================================
echo Process Monitor (Sysinternals)
echo ========================================
echo.
echo Process Monitor is the most detailed tool for checking network activity.
echo.
echo Download: https://docs.microsoft.com/en-us/sysinternals/downloads/procmon
echo.
echo Steps:
echo   1. Download and run Procmon.exe (as Administrator)
echo   2. Set filters:
echo      - Process Name is FICture2.exe
echo      - Operation is TCP Connect
echo      - Operation is TCP Send
echo      - Operation is UDP Send
echo   3. Start capture
echo   4. Run FICture2.exe
echo   5. Stop capture and review results
echo.
echo Press any key to start FICture2.exe when ready...
pause
start "" "%EXE_PATH%"
echo.
pause
goto end

:end
echo.
echo ========================================
echo Test complete
echo ========================================
echo.
