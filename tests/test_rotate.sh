#!/usr/bin/env bash
# =============================================================================
# Blackbox Standalone Acceptance Test: test_rotate (Linux / Wine)
# Verifies crash detection, atomic rotation to ring-crash-*.bin,
# byte-identical preservation of pre-crash telemetry, and clean new ring creation.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN_DIR="${REPO_ROOT}/bin"

WINE_CMD="${WINE_CMD:-wine}"
ANALYZER="${BIN_DIR}/blackbox-analyze.exe"
COLLECTOR="${BIN_DIR}/blackbox.exe"

TEST_DIR=$(mktemp -d /tmp/bb_test_rotate_XXXXXX)
RING_FILE="${TEST_DIR}/ring.bin"
PRE_CRASH_COPY="${TEST_DIR}/ring_pre_crash.bin"

trap 'rm -rf "${TEST_DIR}"' EXIT

echo "========================================================"
echo " [TEST] test_rotate: Crash Detection & Atomic Rotation"
echo "========================================================"

# Step 1: Pre-fill ring.bin without clean_shutdown flag (simulating crash)
echo "[*] Pre-filling ring.bin with 50 ungracefully-terminated records (clean_shutdown = 0)..."
python3 "${REPO_ROOT}/tools/generate_test_ring.py" --scenario=clean --records=50 --output="${RING_FILE}" --ring-kb=64

# Keep exact copy of pre-crash ring to verify byte-identical preservation
cp -f "${RING_FILE}" "${PRE_CRASH_COPY}"

# Step 2: Start collector; must detect clean_shutdown == 0 and rotate ring.bin
if [[ -f "${COLLECTOR}" ]] && command -v "${WINE_CMD}" &>/dev/null; then
    echo "[*] Starting blackbox.exe under Wine to trigger crash detection & rotation..."
    WINEDEBUG=-all "${WINE_CMD}" "${COLLECTOR}" --path="${RING_FILE}" --ring-kb=64 --hz=20 &
    PID=$!
    sleep 1.5
    kill -9 "$PID" 2>/dev/null || true
    sleep 0.5
else
    echo "[ERROR] blackbox.exe or wine not found!"
    exit 1
fi

# Step 3: Assert rotated crash file exists
CRASH_FILE=$(ls "${TEST_DIR}"/ring-crash-*.bin 2>/dev/null | head -n 1 || true)
if [[ -z "${CRASH_FILE}" || ! -f "${CRASH_FILE}" ]]; then
    echo "[FAIL] Rotated crash file ring-crash-*.bin was not created in ${TEST_DIR}!"
    exit 1
fi
echo "[+] Detected rotated crash file: $(basename "${CRASH_FILE}")"

# Step 4: Assert old pre-crash ring is byte-identical inside rotated file
if cmp -s "${PRE_CRASH_COPY}" "${CRASH_FILE}"; then
    echo "[+] Pre-crash telemetry tail is byte-identical inside rotated crash file."
else
    echo "[FAIL] Rotated crash file is NOT byte-identical to original pre-crash ring!"
    exit 1
fi

# Step 5: Assert newly created ring.bin is clean and initialized
if [[ ! -f "${RING_FILE}" ]]; then
    echo "[FAIL] New active ring.bin was not created!"
    exit 1
fi
NEW_SIZE=$(stat -c%s "${RING_FILE}" 2>/dev/null || stat -f%z "${RING_FILE}")
if [[ "${NEW_SIZE}" -ne 65536 ]]; then
    echo "[FAIL] New ring.bin size mismatch: expected 65536 bytes, got ${NEW_SIZE}"
    exit 1
fi
echo "[+] Fresh ring.bin is clean and initialized (size: ${NEW_SIZE} bytes)."

# Step 6: Verify analyzer can inspect rotated crash file via auto-discovery in test directory
if [[ -f "${ANALYZER}" ]] && command -v "${WINE_CMD}" &>/dev/null; then
    echo "[*] Executing blackbox-analyze.exe (auto-discovery in test directory)..."
    OUTPUT=$(cd "${TEST_DIR}" && WINEDEBUG=-all "${WINE_CMD}" "${ANALYZER}" --no-eventlog 2>&1)
    echo "${OUTPUT}" | grep "Selected ring file for analysis:"
    echo "${OUTPUT}" | grep "Total Valid Records"
    echo "[PASS] test_rotate executed cleanly with exit code 0."
else
    echo "[FAIL] Analyzer binary not found!"
    exit 1
fi
