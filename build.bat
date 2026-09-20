@echo off
setlocal EnableDelayedExpansion

:: ============================================================================
:: Blackbox Windows Native Build Script
:: Supports MSVC (cl.exe /W4 /WX /O2 /MT) and MinGW-w64 (gcc.exe -O2 -static)
:: ============================================================================

set SCRIPT_DIR=%~dp0
set BUILD_TYPE=Release
set COMPILER_CHOICE=auto
set CLEAN_FIRST=0

:parse_args
if "%~1"=="" goto validate_env
if /i "%~1"=="/debug"    set BUILD_TYPE=Debug& shift & goto parse_args
if /i "%~1"=="/release"  set BUILD_TYPE=Release& shift & goto parse_args
if /i "%~1"=="/clean"    set CLEAN_FIRST=1& shift & goto parse_args
if /i "%~1"=="/msvc"     set COMPILER_CHOICE=msvc& shift & goto parse_args
if /i "%~1"=="/mingw"    set COMPILER_CHOICE=mingw& shift & goto parse_args
if /i "%~1"=="/?"        goto show_help
if /i "%~1"=="-h"        goto show_help
if /i "%~1"=="/help"     goto show_help
echo [ERROR] Unknown option: %~1
goto show_help

:show_help
echo Usage: build.bat [OPTIONS]
echo Options:
echo   /release    Build optimized Release binaries (default)
echo   /debug      Build Debug binaries with symbols
echo   /clean      Clean build artifacts before building
echo   /msvc       Force use of Microsoft Visual C++ (cl.exe)
echo   /mingw      Force use of MinGW-w64 (gcc.exe)
echo   /?          Show this help message
exit /b 0

:validate_env
echo ========================================================
echo  Blackbox Windows Native Build
echo  Build Type: %BUILD_TYPE%
echo ========================================================

:: Create bin output directory
if not exist "%SCRIPT_DIR%bin" mkdir "%SCRIPT_DIR%bin"

:: Detect compiler choice
if "%COMPILER_CHOICE%"=="msvc" goto build_msvc
if "%COMPILER_CHOICE%"=="mingw" goto build_mingw

:: Auto-detection: Check if cl.exe is in PATH
where cl.exe >nul 2>&1
if %errorlevel% equ 0 goto build_msvc

:: Try locating vcvarsall.bat via vswhere.exe
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist %VSWHERE% (
    for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_PATH=%%i"
    )
    if defined VS_PATH (
        echo [*] Initializing MSVC environment from: !VS_PATH!
        call "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
        if !errorlevel! equ 0 goto build_msvc
    )
)

:: Check if gcc.exe is in PATH
where gcc.exe >nul 2>&1
if %errorlevel% equ 0 goto build_mingw

echo [ERROR] No supported C compiler found!
echo Please run this script from:
echo   1. Visual Studio x64 Native Tools Command Prompt (for MSVC cl.exe)
echo   2. Command prompt with MinGW-w64 gcc.exe in PATH
exit /b 1

:: ----------------------------------------------------------------------------
:: Build with MSVC (cl.exe)
:: ----------------------------------------------------------------------------
:build_msvc
echo [*] Building with MSVC (cl.exe) - Warning Level /W4 /WX /MT...
set BUILD_DIR=%SCRIPT_DIR%build-msvc
if "%CLEAN_FIRST%"=="1" if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set MSVC_FLAGS=/nologo /W4 /WX /GS /GF /Gy /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%SCRIPT_DIR%include" /I"%SCRIPT_DIR%src"
if "%BUILD_TYPE%"=="Release" (
    set MSVC_FLAGS=!MSVC_FLAGS! /O2 /MT /DNDEBUG /GL
    set MSVC_LINK_FLAGS=/INCREMENTAL:NO /OPT:REF /OPT:ICF /LTCG
) else (
    set MSVC_FLAGS=!MSVC_FLAGS! /Od /MTd /Zi /D_DEBUG
    set MSVC_LINK_FLAGS=/DEBUG /INCREMENTAL:NO
)

:: Locate source files for Collector
set COLLECTOR_SRCS="%SCRIPT_DIR%src\ring_buffer.c" "%SCRIPT_DIR%src\calibration.c"
if exist "%SCRIPT_DIR%src\collector_main.c" (
    set COLLECTOR_SRCS=!COLLECTOR_SRCS! "%SCRIPT_DIR%src\collector_main.c"
    if exist "%SCRIPT_DIR%src\telemetry_nvml.c" set COLLECTOR_SRCS=!COLLECTOR_SRCS! "%SCRIPT_DIR%src\telemetry_nvml.c"
    if exist "%SCRIPT_DIR%src\telemetry_process.c" set COLLECTOR_SRCS=!COLLECTOR_SRCS! "%SCRIPT_DIR%src\telemetry_process.c"
    if exist "%SCRIPT_DIR%src\timer_loop.c" set COLLECTOR_SRCS=!COLLECTOR_SRCS! "%SCRIPT_DIR%src\timer_loop.c"
    set HAS_COLLECTOR=1
) else if exist "%SCRIPT_DIR%src\collector\main.c" (
    set COLLECTOR_SRCS=!COLLECTOR_SRCS! "%SCRIPT_DIR%src\collector\main.c" "%SCRIPT_DIR%src\collector\ring_writer.c" "%SCRIPT_DIR%src\collector\proc_monitor.c" "%SCRIPT_DIR%src\collector\timer.c" "%SCRIPT_DIR%src\collector\nvml_client.c"
    set HAS_COLLECTOR=1
)

:: Locate source files for Analyzer
set ANALYZER_SRCS="%SCRIPT_DIR%src\ring_buffer.c" "%SCRIPT_DIR%src\calibration.c"
if exist "%SCRIPT_DIR%src\analyzer_main.c" (
    set ANALYZER_SRCS=!ANALYZER_SRCS! "%SCRIPT_DIR%src\analyzer_main.c"
    if exist "%SCRIPT_DIR%src\anomaly_engine.c" set ANALYZER_SRCS=!ANALYZER_SRCS! "%SCRIPT_DIR%src\anomaly_engine.c"
    if exist "%SCRIPT_DIR%src\event_log.c" set ANALYZER_SRCS=!ANALYZER_SRCS! "%SCRIPT_DIR%src\event_log.c"
    if exist "%SCRIPT_DIR%src\telemetry_nvml.c" set ANALYZER_SRCS=!ANALYZER_SRCS! "%SCRIPT_DIR%src\telemetry_nvml.c"
    set HAS_ANALYZER=1
) else if exist "%SCRIPT_DIR%src\analyzer\main.c" (
    set ANALYZER_SRCS=!ANALYZER_SRCS! "%SCRIPT_DIR%src\analyzer\main.c" "%SCRIPT_DIR%src\analyzer\ring_reader.c" "%SCRIPT_DIR%src\analyzer\anomaly_detector.c" "%SCRIPT_DIR%src\analyzer\event_log.c" "%SCRIPT_DIR%src\analyzer\live_replay.c"
    set HAS_ANALYZER=1
)

if defined HAS_COLLECTOR (
    echo [*] Compiling blackbox.exe...
    cl.exe !MSVC_FLAGS! !COLLECTOR_SRCS! /Fo"%BUILD_DIR%\\" /Fe"%SCRIPT_DIR%bin\blackbox.exe" /link !MSVC_LINK_FLAGS! kernel32.lib user32.lib advapi32.lib
    if !errorlevel! neq 0 (
        echo [ERROR] MSVC build of blackbox.exe failed!
        exit /b 1
    )
)

if defined HAS_ANALYZER (
    echo [*] Compiling blackbox-analyze.exe...
    cl.exe !MSVC_FLAGS! !ANALYZER_SRCS! /Fo"%BUILD_DIR%\\" /Fe"%SCRIPT_DIR%bin\blackbox-analyze.exe" /link !MSVC_LINK_FLAGS! kernel32.lib user32.lib advapi32.lib wevtapi.lib
    if !errorlevel! neq 0 (
        echo [ERROR] MSVC build of blackbox-analyze.exe failed!
        exit /b 1
    )
)

if exist "%SCRIPT_DIR%tests\synthetic_gen.c" (
    echo [*] Compiling synthetic_gen.exe...
    cl.exe !MSVC_FLAGS! "%SCRIPT_DIR%tests\synthetic_gen.c" "%SCRIPT_DIR%src\ring_buffer.c" "%SCRIPT_DIR%src\calibration.c" /Fo"%BUILD_DIR%\\" /Fe"%SCRIPT_DIR%bin\synthetic_gen.exe" /link !MSVC_LINK_FLAGS! kernel32.lib
)

goto build_success

:: ----------------------------------------------------------------------------
:: Build with MinGW-w64 (gcc.exe)
:: ----------------------------------------------------------------------------
:build_mingw
echo [*] Building with MinGW-w64 (gcc.exe) - Strict Static Linking...
set BUILD_DIR=%SCRIPT_DIR%build-mingw
if "%CLEAN_FIRST%"=="1" if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set MINGW_FLAGS=-std=c11 -Wall -Wextra -Wpedantic -Werror -static -static-libgcc -I"%SCRIPT_DIR%include" -I"%SCRIPT_DIR%src"
if "%BUILD_TYPE%"=="Release" (
    set MINGW_FLAGS=!MINGW_FLAGS! -O2 -DNDEBUG
) else (
    set MINGW_FLAGS=!MINGW_FLAGS! -g -O0 -D_DEBUG
)

set COLLECTOR_SRCS="%SCRIPT_DIR%src\ring_buffer.c" "%SCRIPT_DIR%src\calibration.c"
if exist "%SCRIPT_DIR%src\collector_main.c" (
    set COLLECTOR_SRCS=!COLLECTOR_SRCS! "%SCRIPT_DIR%src\collector_main.c"
    if exist "%SCRIPT_DIR%src\telemetry_nvml.c" set COLLECTOR_SRCS=!COLLECTOR_SRCS! "%SCRIPT_DIR%src\telemetry_nvml.c"
    if exist "%SCRIPT_DIR%src\telemetry_process.c" set COLLECTOR_SRCS=!COLLECTOR_SRCS! "%SCRIPT_DIR%src\telemetry_process.c"
    if exist "%SCRIPT_DIR%src\timer_loop.c" set COLLECTOR_SRCS=!COLLECTOR_SRCS! "%SCRIPT_DIR%src\timer_loop.c"
    set HAS_COLLECTOR=1
) else if exist "%SCRIPT_DIR%src\collector\main.c" (
    set COLLECTOR_SRCS=!COLLECTOR_SRCS! "%SCRIPT_DIR%src\collector\main.c" "%SCRIPT_DIR%src\collector\ring_writer.c" "%SCRIPT_DIR%src\collector\proc_monitor.c" "%SCRIPT_DIR%src\collector\timer.c" "%SCRIPT_DIR%src\collector\nvml_client.c"
    set HAS_COLLECTOR=1
)

set ANALYZER_SRCS="%SCRIPT_DIR%src\ring_buffer.c" "%SCRIPT_DIR%src\calibration.c"
if exist "%SCRIPT_DIR%src\analyzer_main.c" (
    set ANALYZER_SRCS=!ANALYZER_SRCS! "%SCRIPT_DIR%src\analyzer_main.c"
    if exist "%SCRIPT_DIR%src\anomaly_engine.c" set ANALYZER_SRCS=!ANALYZER_SRCS! "%SCRIPT_DIR%src\anomaly_engine.c"
    if exist "%SCRIPT_DIR%src\event_log.c" set ANALYZER_SRCS=!ANALYZER_SRCS! "%SCRIPT_DIR%src\event_log.c"
    if exist "%SCRIPT_DIR%src\telemetry_nvml.c" set ANALYZER_SRCS=!ANALYZER_SRCS! "%SCRIPT_DIR%src\telemetry_nvml.c"
    set HAS_ANALYZER=1
) else if exist "%SCRIPT_DIR%src\analyzer\main.c" (
    set ANALYZER_SRCS=!ANALYZER_SRCS! "%SCRIPT_DIR%src\analyzer\main.c" "%SCRIPT_DIR%src\analyzer\ring_reader.c" "%SCRIPT_DIR%src\analyzer\anomaly_detector.c" "%SCRIPT_DIR%src\analyzer\event_log.c" "%SCRIPT_DIR%src\analyzer\live_replay.c"
    set HAS_ANALYZER=1
)

if defined HAS_COLLECTOR (
    echo [*] Compiling blackbox.exe...
    gcc.exe !MINGW_FLAGS! !COLLECTOR_SRCS! -o "%SCRIPT_DIR%bin\blackbox.exe" -lkernel32 -luser32 -ladvapi32
    if !errorlevel! neq 0 (
        echo [ERROR] MinGW build of blackbox.exe failed!
        exit /b 1
    )
)

if defined HAS_ANALYZER (
    echo [*] Compiling blackbox-analyze.exe...
    gcc.exe !MINGW_FLAGS! !ANALYZER_SRCS! -o "%SCRIPT_DIR%bin\blackbox-analyze.exe" -lkernel32 -luser32 -ladvapi32 -lwevtapi
    if !errorlevel! neq 0 (
        echo [ERROR] MinGW build of blackbox-analyze.exe failed!
        exit /b 1
    )
)

if exist "%SCRIPT_DIR%tests\synthetic_gen.c" (
    echo [*] Compiling synthetic_gen.exe...
    gcc.exe !MINGW_FLAGS! "%SCRIPT_DIR%tests\synthetic_gen.c" "%SCRIPT_DIR%src\ring_buffer.c" "%SCRIPT_DIR%src\calibration.c" -o "%SCRIPT_DIR%bin\synthetic_gen.exe" -lkernel32
)

goto build_success

:build_success
echo ========================================================
echo  Build Succeeded!
echo  Artifacts available in: %SCRIPT_DIR%bin\
dir "%SCRIPT_DIR%bin\*.exe" 2>nul
echo ========================================================
exit /b 0
