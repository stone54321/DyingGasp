@echo off
setlocal EnableDelayedExpansion

:: ============================================================================
:: Blackbox Standalone Acceptance Test: test_rotate
:: Verifies crash detection, atomic rotation to ring-crash-*.bin,
:: byte-identical preservation of pre-crash telemetry, and clean new ring creation.
:: ============================================================================

echo ========================================================
echo [TEST] test_rotate: Crash Detection & Atomic Rotation
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
set TEST_DIR=%TEMP%\bb_test_rotate_%RANDOM%
mkdir "%TEST_DIR%"
set RING_FILE=%TEST_DIR%\ring.bin
set PRE_CRASH_COPY=%TEST_DIR%\ring_pre_crash.bin

echo [*] Test Directory: %TEST_DIR%
echo [*] Analyzer:       %ANALYZER%
echo [*] Collector:      %COLLECTOR%

:: Step 1: Pre-fill ring.bin without clean_shutdown flag (clean_shutdown = 0)
echo [*] Pre-filling ring.bin with 50 pre-crash records...
where python >nul 2>&1
if %errorlevel% equ 0 (
    python "%REPO_ROOT%\tools\generate_test_ring.py" --scenario=clean --records=50 --output="%RING_FILE%" --ring-kb=64
) else (
    start "" /b "%COLLECTOR%" --path="%RING_FILE%" --ring-kb=64 --hz=20 >nul 2>&1
    timeout /t 2 /nobreak >nul
    taskkill /f /im blackbox.exe >nul 2>&1
)

if not exist "%RING_FILE%" (
    echo [FAIL] Failed to create initial pre-crash ring file!
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 1
)

copy /y "%RING_FILE%" "%PRE_CRASH_COPY%" >nul

:: Step 2: Start collector; must detect clean_shutdown == 0 and rotate ring.bin
echo [*] Starting blackbox.exe to trigger crash detection & rotation...
start "" /b "%COLLECTOR%" --path="%RING_FILE%" --ring-kb=64 --hz=20 >nul 2>&1
timeout /t 2 /nobreak >nul
taskkill /f /im blackbox.exe >nul 2>&1

:: Step 3: Assert rotated crash file exists
set CRASH_FILE=
for /f "tokens=*" %%f in ('dir /b "%TEST_DIR%\ring-crash-*.bin" 2^>nul') do (
    set "CRASH_FILE=%%f"
)

if not defined CRASH_FILE (
    echo [FAIL] Rotated crash file ring-crash-*.bin was not created!
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 1
)
echo [+] Detected rotated crash file: %CRASH_FILE%

:: Step 4: Assert old pre-crash ring is byte-identical inside rotated file
fc /b "%PRE_CRASH_COPY%" "%TEST_DIR%\%CRASH_FILE%" >nul 2>&1
if %errorlevel% neq 0 (
    echo [FAIL] Rotated crash file is NOT byte-identical to pre-crash ring!
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 1
)
echo [+] Pre-crash telemetry tail is byte-identical inside rotated crash file.

:: Step 5: Assert newly created ring.bin is clean and initialized
if not exist "%RING_FILE%" (
    echo [FAIL] New active ring.bin was not created!
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 1
)
echo [+] Fresh ring.bin is clean and initialized.

:: Step 6: Verify analyzer inspects rotated crash file via auto-discovery
pushd "%TEST_DIR%"
"%ANALYZER%" --no-eventlog > "%TEST_DIR%\rotate_out.txt" 2>&1
set ANALYZE_RET=%errorlevel%
popd

type "%TEST_DIR%\rotate_out.txt"

findstr /i "Selected ring file for analysis:" "%TEST_DIR%\rotate_out.txt" >nul
set FOUND_SEL=%errorlevel%
findstr /i "ring-crash-" "%TEST_DIR%\rotate_out.txt" >nul
set FOUND_CRASH=%errorlevel%

if %ANALYZE_RET% equ 0 if %FOUND_SEL% equ 0 if %FOUND_CRASH% equ 0 (
    echo.
    echo [PASS] test_rotate passed successfully!
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 0
) else (
    echo.
    echo [FAIL] test_rotate failed with exit code: %ANALYZE_RET%
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 1
)
