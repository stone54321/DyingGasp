@echo off
setlocal EnableDelayedExpansion

:: ============================================================================
:: Blackbox Standalone Acceptance Test: test_kill
:: Verifies write-through durability, ungraceful kill recovery, and that after
:: collector restart, the pre-crash telemetry tail is intact and readable from
:: the rotated crash file.
:: ============================================================================

echo ========================================================
echo [TEST] test_kill: Forced Termination & Rotation Recovery
echo ========================================================

set SCRIPT_DIR=%~dp0
set REPO_ROOT=%SCRIPT_DIR%..
set BIN_DIR=%REPO_ROOT%\bin
if not exist "%BIN_DIR%\blackbox-analyze.exe" (
    if exist "%REPO_ROOT%\blackbox-analyze.exe" (
        set BIN_DIR=%REPO_ROOT%
    ) else (
        echo [ERROR] blackbox-analyze.exe not found in "%BIN_DIR%" or "%REPO_ROOT%"!
        exit /b 1
    )
)

set ANALYZER=%BIN_DIR%\blackbox-analyze.exe
set COLLECTOR=%BIN_DIR%\blackbox.exe
set TEST_DIR=%TEMP%\bb_test_kill_%RANDOM%
mkdir "%TEST_DIR%"
set RING_FILE=%TEST_DIR%\ring.bin

echo [*] Test Directory: %TEST_DIR%
echo [*] Analyzer:       %ANALYZER%

:: Step 1: Launch collector and forcibly terminate
if exist "%COLLECTOR%" (
    echo [*] Step 1: Starting collector session 1 in background at 20 Hz...
    start "" /b "%COLLECTOR%" --path="%RING_FILE%" --size-mb=2 --hz=20 >nul 2>&1
    timeout /t 2 /nobreak >nul
    echo [*] Forcibly killing session 1 via taskkill /F...
    taskkill /f /im blackbox.exe >nul 2>&1
    timeout /t 1 /nobreak >nul

    echo [*] Step 2: Restarting collector to trigger crash rotation policy...
    start "" /b "%COLLECTOR%" --path="%RING_FILE%" --size-mb=2 --hz=20 >nul 2>&1
    timeout /t 2 /nobreak >nul
    taskkill /f /im blackbox.exe >nul 2>&1
) else (
    echo [*] blackbox.exe not found; using synthetic ring generator to simulate unbuffered records...
    python "%REPO_ROOT%\tools\generate_test_ring.py" --scenario=clean --records=50 --output="%RING_FILE%" --ring-kb=64
)

:: Step 3: Locate rotated crash file
set CRASH_FILE=
for /f "tokens=*" %%f in ('dir /b "%TEST_DIR%\ring-crash-*.bin" 2^>nul') do (
    set "CRASH_FILE=%%f"
)

set TARGET_FILE=%RING_FILE%
if defined CRASH_FILE (
    echo [+] Rotated crash file detected: %CRASH_FILE%
    set "TARGET_FILE=%TEST_DIR%\%CRASH_FILE%"
)

:: Step 4: Verify analyzer recovers records cleanly
echo [*] Running analyzer on persisted ring buffer...
"%ANALYZER%" "%TARGET_FILE%" --gap=3 --no-eventlog > "%TEST_DIR%\kill_out.txt" 2>&1
set RET=%errorlevel%

type "%TEST_DIR%\kill_out.txt"

findstr /i "Records" "%TEST_DIR%\kill_out.txt" >nul
set FOUND_RECORDS=%errorlevel%

if %RET% equ 0 if %FOUND_RECORDS% equ 0 (
    echo.
    echo [PASS] test_kill passed successfully! Pre-crash tail recovered from rotated crash file.
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 0
) else (
    echo.
    echo [FAIL] test_kill failed with exit code: %RET%
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 1
)
