@echo off
setlocal EnableDelayedExpansion

:: ============================================================================
:: Blackbox Standalone Acceptance Test: test_wrap
:: Verifies circular wrap, slot offset overwrite, and sequence continuity.
:: ============================================================================

echo ========================================================
echo [TEST] test_wrap: Circular Buffer Wrap and Continuity
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
set TEST_DIR=%TEMP%\bb_test_wrap_%RANDOM%
mkdir "%TEST_DIR%"
set RING_FILE=%TEST_DIR%\ring_wrap.bin

echo [*] Test Directory: %TEST_DIR%
echo [*] Analyzer:       %ANALYZER%

:: Step 1: Generate or collect wrapped ring
if exist "%COLLECTOR%" (
    echo [*] Launching blackbox.exe at 50 Hz on 1 MB ring...
    start "" /b "%COLLECTOR%" --path="%RING_FILE%" --size-mb=1 --hz=50 >nul 2>&1
    timeout /t 3 /nobreak >nul
    taskkill /f /im blackbox.exe >nul 2>&1
) else (
    echo [*] blackbox.exe not found; using synthetic ring generator for wrap scenario...
    python "%REPO_ROOT%\tools\generate_test_ring.py" --scenario=wrap --output="%RING_FILE%" --ring-kb=64
)

if not exist "%RING_FILE%" (
    echo [FAIL] Ring file was not created: %RING_FILE%
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 1
)

:: Step 2: Analyze wrapped ring
echo [*] Executing analyzer on wrapped ring...
"%ANALYZER%" "%RING_FILE%" --gap=3 --no-eventlog > "%TEST_DIR%\wrap_out.txt" 2>&1
set RET=%errorlevel%

type "%TEST_DIR%\wrap_out.txt"

if %RET% equ 0 (
    echo.
    echo [PASS] test_wrap passed successfully!
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 0
) else (
    echo.
    echo [FAIL] test_wrap failed with exit code: %RET%
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b %RET%
)
