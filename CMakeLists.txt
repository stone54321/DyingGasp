cmake_minimum_required(VERSION 3.16)
project(blackbox C)

# Enforce C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Platform verification: Target must be Windows
if(NOT WIN32)
    message(FATAL_ERROR "Blackbox targets bare Windows 10/11 x64 only. For cross-compilation from Linux, use:\n"
                        "  cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake\n"
                        "or execute ./build.sh")
endif()

# Build configuration options
option(BUILD_TESTS "Build test helpers and synthetic anomaly generators" ON)
option(ENABLE_STRICT_WARNINGS "Treat all warnings as errors" ON)

# Output binaries directly to bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# Common compile definitions: pure Win32, lean & mean, safe CRT
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# Compiler flags: MSVC vs GCC / MinGW
if(MSVC)
    # Statically link MSVC C Runtime (/MT for Release, /MTd for Debug)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Warning level 4, optimization /O2, security checks /GS, string pooling /GF
    add_compile_options(/W4 /O2 /GS /GF /Gy)
    if(ENABLE_STRICT_WARNINGS)
        add_compile_options(/WX)
    endif()

    # Linker optimizations: eliminate unused functions/data, fold identical COMDATs
    add_link_options(/OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    # MinGW-w64 / GCC flags
    add_compile_options(-Wall -Wextra -O2 -fstack-protector-strong)
    if(ENABLE_STRICT_WARNINGS)
        add_compile_options(-Werror)
    endif()

    # Strict static linking: eliminate libwinpthread-1.dll, libgcc_s_seh-1.dll
    add_link_options(-static -static-libgcc)
endif()

# -----------------------------------------------------------------------------
# Core Library: Ring Buffer & Calibration
# -----------------------------------------------------------------------------
add_library(blackbox_core STATIC
    src/ring_buffer.c
    src/calibration.c
)

# -----------------------------------------------------------------------------
# Target 1: blackbox (Telemetry Collector Daemon)
# -----------------------------------------------------------------------------
set(COLLECTOR_SOURCES
    src/ring_buffer.c
    src/calibration.c
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/collector_main.c")
    list(APPEND COLLECTOR_SOURCES src/collector_main.c)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/collector/main.c")
    list(APPEND COLLECTOR_SOURCES src/collector/main.c)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry_nvml.c")
    list(APPEND COLLECTOR_SOURCES src/telemetry_nvml.c)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/collector/nvml_client.c")
    list(APPEND COLLECTOR_SOURCES src/collector/nvml_client.c)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry_process.c")
    list(APPEND COLLECTOR_SOURCES src/telemetry_process.c)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/collector/proc_monitor.c")
    list(APPEND COLLECTOR_SOURCES src/collector/proc_monitor.c)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/timer_loop.c")
    list(APPEND COLLECTOR_SOURCES src/timer_loop.c)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/collector/timer.c")
    list(APPEND COLLECTOR_SOURCES src/collector/timer.c)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/collector_main.c" OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/collector/main.c")
    add_executable(blackbox ${COLLECTOR_SOURCES})
    target_link_libraries(blackbox PRIVATE kernel32 user32 advapi32)
endif()

# -----------------------------------------------------------------------------
# Target 2: blackbox-analyze (Post-Mortem Crash Analyzer)
# -----------------------------------------------------------------------------
set(ANALYZER_SOURCES
    src/ring_buffer.c
    src/calibration.c
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer_main.c")
    list(APPEND ANALYZER_SOURCES src/analyzer_main.c)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer/main.c")
    list(APPEND ANALYZER_SOURCES src/analyzer/main.c)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/anomaly_engine.c")
    list(APPEND ANALYZER_SOURCES src/anomaly_engine.c)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer/anomaly_detector.c")
    list(APPEND ANALYZER_SOURCES src/analyzer/anomaly_detector.c)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/event_log.c")
    list(APPEND ANALYZER_SOURCES src/event_log.c)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer/event_log.c")
    list(APPEND ANALYZER_SOURCES src/analyzer/event_log.c)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry_nvml.c")
    list(APPEND ANALYZER_SOURCES src/telemetry_nvml.c)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/collector/nvml_client.c")
    list(APPEND ANALYZER_SOURCES src/collector/nvml_client.c)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer/live_replay.c")
    list(APPEND ANALYZER_SOURCES src/analyzer/live_replay.c)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer_main.c" OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer/main.c")
    add_executable(blackbox-analyze ${ANALYZER_SOURCES})
    target_link_libraries(blackbox-analyze PRIVATE kernel32 user32 advapi32 wevtapi)
endif()

# -----------------------------------------------------------------------------
# Tests & Generators
# -----------------------------------------------------------------------------
if(BUILD_TESTS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_gen.c")
        add_executable(synthetic_gen
            tests/synthetic_gen.c
            src/ring_buffer.c
            src/calibration.c
        )
        target_link_libraries(synthetic_gen PRIVATE kernel32 user32 advapi32)
    endif()
endif()
