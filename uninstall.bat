@echo off
setlocal EnableDelayedExpansion

:: ============================================================================
:: Blackbox Uninstallation Script
:: Cleanly unregisters Scheduled Task, terminates process, removes binaries
:: Canonical Task Name: BlackboxLogger
:: ============================================================================

echo ========================================================
echo  Blackbox Telemetry Service - Uninstallation
echo ========================================================

:: 1. Elevation check
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Administrator privileges are required!
    echo Please right-click uninstall.bat and select "Run as administrator".
    exit /b 1
)

set TASK_NAME=BlackboxLogger
set INSTALL_DIR=%ProgramFiles%\Blackbox
set DATA_DIR=%ProgramData%\blackbox

:: 2. Stop and delete Scheduled Task
echo [*] Stopping and unregistering Scheduled Task '%TASK_NAME%'...
schtasks /end /tn "%TASK_NAME%" >nul 2>&1
schtasks /delete /tn "%TASK_NAME%" /f >nul 2>&1

:: 3. Terminate running process if still active
echo [*] Terminating running blackbox.exe instances...
taskkill /f /im blackbox.exe >nul 2>&1

:: 4. Remove installation directory and binaries
if exist "%INSTALL_DIR%" (
    echo [*] Removing installed binaries from "%INSTALL_DIR%"...
    del /f /q "%INSTALL_DIR%\blackbox.exe" >nul 2>&1
    del /f /q "%INSTALL_DIR%\blackbox-analyze.exe" >nul 2>&1
    rmdir "%INSTALL_DIR%" >nul 2>&1
)

:: 5. Telemetry preservation notice
echo [*] Checking telemetry ring buffer and crash dumps...
if exist "%DATA_DIR%\ring.bin" (
    echo [NOTICE] Telemetry ring buffer and crash dumps have been preserved for post-mortem analysis:
    echo          "%DATA_DIR%\ring.bin"
    echo          "%DATA_DIR%\ring-crash-*.bin"
    echo If you wish to delete all telemetry data permanently, run:
    echo   rmdir /s /q "%DATA_DIR%"
)

echo ========================================================
echo  Uninstallation Complete!
echo  Scheduled Task '%TASK_NAME%' has been removed.
echo ========================================================
exit /b 0
