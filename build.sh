#!/usr/bin/env bash
# =============================================================================
# Blackbox Linux Cross-Compilation Build Script
# Targets: Windows x86_64 (blackbox.exe, blackbox-analyze.exe)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-mingw"
BUILD_TYPE="Release"
CLEAN_FIRST=0

# Parse CLI options
while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --clean)
            CLEAN_FIRST=1
            shift
            ;;
        -h|--help)
            echo "Usage: ./build.sh [OPTIONS]"
            echo "Options:"
            echo "  --release     Build Release binaries (default, -O2)"
            echo "  --debug       Build Debug binaries (-g)"
            echo "  --clean       Remove build directory before compiling"
            echo "  -h, --help    Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Run './build.sh --help' for usage."
            exit 1
            ;;
    esac
done

echo "========================================================"
echo " Blackbox Build & Cross-Compilation: Linux Host"
echo " Build Type: ${BUILD_TYPE}"
echo "========================================================"

# Step 1: Verify prerequisites
if ! command -v cmake &>/dev/null; then
    echo "[ERROR] CMake not found. Please install cmake."
    exit 1
fi

if ! command -v x86_64-w64-mingw32-gcc &>/dev/null; then
    echo "[ERROR] x86_64-w64-mingw32-gcc not found."
    echo "Please install MinGW-w64 cross compiler or ensure wrapper is in PATH."
    exit 1
fi

if [[ ${CLEAN_FIRST} -eq 1 && -d "${BUILD_DIR}" ]]; then
    echo "[*] Cleaning build directory: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

# Step 2: Configure with CMake
echo "[*] Configuring CMake with toolchain-mingw64.cmake..."
cmake -B "${BUILD_DIR}" -S "${SCRIPT_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/toolchain-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DENABLE_STRICT_WARNINGS=ON \
    -DBUILD_TESTS=ON

# Step 3: Build targets
echo "[*] Building targets in parallel..."
cmake --build "${BUILD_DIR}" --parallel "$(nproc 2>/dev/null || echo 2)"

# Step 4: Verify static linking & dependencies on any generated .exe
OBJDUMP_TOOL="objdump"
if command -v x86_64-w64-mingw32-objdump &>/dev/null; then
    OBJDUMP_TOOL="x86_64-w64-mingw32-objdump"
fi

mkdir -p "${SCRIPT_DIR}/bin"

for exe in "${BUILD_DIR}/bin"/*.exe; do
    if [[ -f "${exe}" ]]; then
        exe_name="$(basename "${exe}")"
        echo "[*] Verifying static linking on ${exe_name}..."
        DYNAMIC_DEPS=$("${OBJDUMP_TOOL}" -p "${exe}" 2>/dev/null | grep "DLL Name" || true)
        echo "    Imports for ${exe_name}:"
        echo "${DYNAMIC_DEPS}"
        if echo "${DYNAMIC_DEPS}" | grep -Ei "(pthread|gcc_s|stdc\+\+)"; then
            echo "[ERROR] Forbidden non-system runtime DLL detected in ${exe_name}! Static linking failed."
            exit 1
        fi
        cp -f "${exe}" "${SCRIPT_DIR}/bin/"
    fi
done

echo "========================================================"
echo " Build Succeeded!"
echo " Artifacts in: ${SCRIPT_DIR}/bin/"
ls -la "${SCRIPT_DIR}/bin" || true
echo "========================================================"
