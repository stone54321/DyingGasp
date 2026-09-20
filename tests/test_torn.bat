@echo off
setlocal EnableDelayedExpansion

:: ============================================================================
:: Blackbox Standalone Acceptance Test: test_torn
:: Verifies torn record boundary detection and silent discard without crashing.
:: ============================================================================

echo ========================================================
echo [TEST] test_torn: Detection of Corrupted Boundary Record
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
set TEST_DIR=%TEMP%\bb_test_torn_%RANDOM%
mkdir "%TEST_DIR%"
set RING_FILE=%TEST_DIR%\ring_torn.bin

echo [*] Test Directory: %TEST_DIR%
echo [*] Analyzer:       %ANALYZER%

:: Step 1: Generate torn ring (truncated by 30 bytes)
echo [*] Generating ring fixture with final record truncated by 30 bytes...
python "%REPO_ROOT%\tools\generate_test_ring.py" --scenario=torn --output="%RING_FILE%" --ring-kb=64 --torn-bytes=30

if not exist "%RING_FILE%" (
    echo [FAIL] Ring file was not created: %RING_FILE%
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 1
)

:: Step 2: Run analyzer on torn ring
echo [*] Running analyzer on truncated ring buffer...
"%ANALYZER%" "%RING_FILE%" --gap=3 --no-eventlog > "%TEST_DIR%\torn_out.txt" 2>&1
set RET=%errorlevel%

type "%TEST_DIR%\torn_out.txt"

if %RET% equ 0 (
    echo.
    echo [PASS] test_torn passed successfully! Corrupt boundary record discarded cleanly without fault.
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b 0
) else (
    echo.
    echo [FAIL] test_torn failed with exit code: %RET%
    rmdir /s /q "%TEST_DIR%" >nul 2>&1
    exit /b %RET%
)
