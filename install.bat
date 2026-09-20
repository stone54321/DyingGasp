@echo off
setlocal EnableDelayedExpansion

:: ============================================================================
:: Blackbox Installation Script
:: Configures an elevated Windows Scheduled Task running at system startup
:: Canonical Task Name: BlackboxLogger
:: ============================================================================

echo ========================================================
echo  Blackbox Telemetry Service - Installation
echo ========================================================

:: 1. Elevation check via net session
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Administrator privileges are required!
    echo Please right-click install.bat and select "Run as administrator".
    exit /b 1
)

:: 2. Determine source and destination paths
set SCRIPT_DIR=%~dp0
set INSTALL_DIR=%ProgramFiles%\Blackbox
set DATA_DIR=%ProgramData%\blackbox
set TASK_NAME=BlackboxLogger

:: Locate binaries (check bin\ subfolder, then script directory)
if exist "%SCRIPT_DIR%bin\blackbox.exe" (
    set BIN_SRC=%SCRIPT_DIR%bin\
) else if exist "%SCRIPT_DIR%blackbox.exe" (
    set BIN_SRC=%SCRIPT_DIR%
) else (
    echo [ERROR] blackbox.exe not found in "%SCRIPT_DIR%" or "%SCRIPT_DIR%bin\"!
    echo Please compile the project before running install.bat.
    exit /b 1
)

:: 3. Stop running instance or task if already active
echo [*] Checking for running instances...
schtasks /end /tn "%TASK_NAME%" >nul 2>&1
taskkill /f /im blackbox.exe >nul 2>&1

:: 4. Create destination directories
echo [*] Creating target directories...
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
if not exist "%DATA_DIR%" mkdir "%DATA_DIR%"

:: Set permissions on %ProgramData%\blackbox (SYSTEM and Admins Full Control, Users Read/Execute)
icacls "%DATA_DIR%" /grant:r "*S-1-5-18:(OI)(CI)F" /grant:r "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-32-545:(OI)(CI)RX" >nul 2>&1

:: 5. Copy binaries
echo [*] Installing binaries to "%INSTALL_DIR%"...
copy /y "%BIN_SRC%blackbox.exe" "%INSTALL_DIR%\blackbox.exe" >nul
if exist "%BIN_SRC%blackbox-analyze.exe" (
    copy /y "%BIN_SRC%blackbox-analyze.exe" "%INSTALL_DIR%\blackbox-analyze.exe" >nul
)

:: 6. Create Windows Scheduled Task (no registry modification)
echo [*] Registering Scheduled Task '%TASK_NAME%'...
schtasks /create /tn "%TASK_NAME%" ^
    /tr "\"%INSTALL_DIR%\blackbox.exe\"" ^
    /sc ONSTART ^
    /ru "SYSTEM" ^
    /rl HIGHEST ^
    /f
if %errorlevel% neq 0 (
    echo [ERROR] Failed to register Scheduled Task via schtasks.exe!
    exit /b 1
)

:: 7. Start the daemon task immediately
echo [*] Starting telemetry daemon...
schtasks /run /tn "%TASK_NAME%"
timeout /t 2 >nul

:: 8. Operational verification
tasklist /fi "imagename eq blackbox.exe" | find /i "blackbox.exe" >nul
if %errorlevel% equ 0 (
    echo [OK] blackbox.exe is running in background.
) else (
    echo [WARNING] blackbox.exe did not start immediately. Check Event Viewer -> TaskScheduler logs.
)

echo ========================================================
echo  Installation Successful!
echo  Telemetry Storage: %DATA_DIR%\ring.bin (Pre-allocated 64 MB)
echo  Analyzer Utility:  %INSTALL_DIR%\blackbox-analyze.exe
echo  Scheduled Task:    %TASK_NAME% (Runs at boot under SYSTEM)
echo ========================================================
exit /b 0
