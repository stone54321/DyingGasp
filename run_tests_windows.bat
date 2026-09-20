@echo off
setlocal EnableDelayedExpansion

:: ============================================================================
:: Blackbox Windows Acceptance Test Suite
:: Runs all 5 automated acceptance tests:
::   1. test_wrap   - Circular wrap and monotonic sequence continuity
::   2. test_kill   - Forced termination and recovery after restart
::   3. test_nogpu  - Graceful fallback and sentinel logging without NVML
::   4. test_torn   - Corrupted boundary record detection and silent discard
::   5. test_rotate - Crash detection, atomic rotation, and new ring creation
:: ============================================================================

echo ========================================================
echo  Blackbox Windows Automated Acceptance Test Suite
echo ========================================================

set SCRIPT_DIR=%~dp0
set BIN_DIR=%SCRIPT_DIR%bin
if not exist "%BIN_DIR%\blackbox-analyze.exe" (
    if exist "%SCRIPT_DIR%blackbox-analyze.exe" (
        set BIN_DIR=%SCRIPT_DIR%
    ) else (
        echo [ERROR] Test binaries not found in "%BIN_DIR%" or "%SCRIPT_DIR%"!
        echo Please build the project first using build.bat or build.sh.
        exit /b 1
    )
)

set TESTS_DIR=%SCRIPT_DIR%tests
set /a TESTS_RUN=0
set /a TESTS_PASSED=0
set /a TESTS_FAILED=0

echo [*] Binary Directory: %BIN_DIR%
echo [*] Tests Directory:  %TESTS_DIR%
echo.

:: ----------------------------------------------------------------------------
:: Test 1: test_wrap
:: ----------------------------------------------------------------------------
echo ============================================================================
echo [TEST 1/5] test_wrap: Circular Buffer Wrap and Continuity
echo ============================================================================
set /a TESTS_RUN+=1
call "%TESTS_DIR%\test_wrap.bat"
if !errorlevel! equ 0 (
    set /a TESTS_PASSED+=1
) else (
    set /a TESTS_FAILED+=1
)
echo.

:: ----------------------------------------------------------------------------
:: Test 2: test_kill
:: ----------------------------------------------------------------------------
echo ============================================================================
echo [TEST 2/5] test_kill: Forced Termination Durability & Rotation
echo ============================================================================
set /a TESTS_RUN+=1
call "%TESTS_DIR%\test_kill.bat"
if !errorlevel! equ 0 (
    set /a TESTS_PASSED+=1
) else (
    set /a TESTS_FAILED+=1
)
echo.

:: ----------------------------------------------------------------------------
:: Test 3: test_nogpu
:: ----------------------------------------------------------------------------
echo ============================================================================
echo [TEST 3/5] test_nogpu: Sentinel Logging Without NVML
echo ============================================================================
set /a TESTS_RUN+=1
call "%TESTS_DIR%\test_nogpu.bat"
if !errorlevel! equ 0 (
    set /a TESTS_PASSED+=1
) else (
    set /a TESTS_FAILED+=1
)
echo.

:: ----------------------------------------------------------------------------
:: Test 4: test_torn
:: ----------------------------------------------------------------------------
echo ============================================================================
echo [TEST 4/5] test_torn: Detection of Corrupted Boundary Record
echo ============================================================================
set /a TESTS_RUN+=1
call "%TESTS_DIR%\test_torn.bat"
if !errorlevel! equ 0 (
    set /a TESTS_PASSED+=1
) else (
    set /a TESTS_FAILED+=1
)
echo.

:: ----------------------------------------------------------------------------
:: Test 5: test_rotate
:: ----------------------------------------------------------------------------
echo ============================================================================
echo [TEST 5/5] test_rotate: Crash Detection & Atomic Rotation
echo ============================================================================
set /a TESTS_RUN+=1
call "%TESTS_DIR%\test_rotate.bat"
if !errorlevel! equ 0 (
    set /a TESTS_PASSED+=1
) else (
    set /a TESTS_FAILED+=1
)
echo.

:: ----------------------------------------------------------------------------
:: Summary
:: ----------------------------------------------------------------------------
echo ========================================================
echo  Acceptance Test Summary: %TESTS_PASSED% / %TESTS_RUN% Passed
echo ========================================================

if %TESTS_FAILED% equ 0 (
    echo [OK] All 5 Windows acceptance tests PASSED successfully.
    exit /b 0
) else (
    echo [FAIL] %TESTS_FAILED% test(s) FAILED.
    exit /b 1
)
