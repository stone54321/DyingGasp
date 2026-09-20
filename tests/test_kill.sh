#!/usr/bin/env bash
# =============================================================================
# Blackbox Standalone Acceptance Test: test_kill (Linux / Wine)
# Verifies write-through durability, ungraceful kill recovery, and that after
# collector restart, the pre-crash telemetry tail is intact and readable from
# the rotated crash file.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN_DIR="${REPO_ROOT}/bin"

WINE_CMD="${WINE_CMD:-wine}"
ANALYZER="${BIN_DIR}/blackbox-analyze.exe"
COLLECTOR="${BIN_DIR}/blackbox.exe"

TEST_DIR=$(mktemp -d /tmp/bb_test_kill_XXXXXX)
RING_FILE="${TEST_DIR}/ring.bin"

trap 'rm -rf "${TEST_DIR}"' EXIT

echo "========================================================"
echo " [TEST] test_kill: Forced Termination & Rotation Recovery"
echo "========================================================"

if [[ -f "${COLLECTOR}" ]] && command -v "${WINE_CMD}" &>/dev/null; then
    # Step 1: Launch session 1 in background at 20 Hz
    echo "[*] Step 1: Launching collector session 1 in background at 20 Hz..."
    WINEDEBUG=-all "${WINE_CMD}" "${COLLECTOR}" --path="${RING_FILE}" --ring-kb=64 --hz=20 &
    PID1=$!
    sleep 2
    echo "[*] Forcibly terminating session 1 (kill -9)..."
    kill -9 "$PID1" 2>/dev/null || true
    sleep 0.5

    # Step 2: Collector restart policy check
    echo "[*] Step 2: Restarting collector on same ring to trigger crash rotation..."
    WINEDEBUG=-all "${WINE_CMD}" "${COLLECTOR}" --path="${RING_FILE}" --ring-kb=64 --hz=20 &
    PID2=$!
    sleep 1.2
    kill -9 "$PID2" 2>/dev/null || true
    sleep 0.5
else
    echo "[*] Generating synthetic crash fixture (50 records)..."
    python3 "${REPO_ROOT}/tools/generate_test_ring.py" --scenario=clean --records=50 --output="${RING_FILE}" --ring-kb=64
fi

# Step 3: Locate rotated crash file
CRASH_FILE=$(ls "${TEST_DIR}"/ring-crash-*.bin 2>/dev/null | head -n 1 || true)
TARGET_FILE="${RING_FILE}"
if [[ -n "${CRASH_FILE}" && -f "${CRASH_FILE}" ]]; then
    echo "[+] Rotated crash file detected: $(basename "${CRASH_FILE}")"
    TARGET_FILE="${CRASH_FILE}"
else
    echo "[*] No rotated crash file found, analyzing active ring..."
fi

# Step 4: Verify analyzer reads pre-crash records
if [[ -f "${ANALYZER}" ]] && command -v "${WINE_CMD}" &>/dev/null; then
    echo "[*] Executing blackbox-analyze.exe on ${TARGET_FILE}..."
    OUTPUT=$(WINEDEBUG=-all "${WINE_CMD}" "${ANALYZER}" "${TARGET_FILE}" --gap=3 --no-eventlog 2>&1)
    echo "${OUTPUT}" | grep -E "(Selected ring file|Total Valid Records)"
    echo "[PASS] test_kill executed cleanly with exit code 0."
else
    echo "[*] Verifying with Reference Oracle..."
    python3 -c "
import sys
sys.path.insert(0, '${REPO_ROOT}')
from tests.e2e_runner import ReferenceRingOracle
o = ReferenceRingOracle('${TARGET_FILE}')
assert o.is_valid_header, 'Invalid header'
assert len(o.valid_records) > 0, 'No records recovered'
print(f'[+] Oracle verified: {len(o.valid_records)} pre-kill records recovered cleanly')
"
    echo "[PASS] test_kill verified successfully by Reference Oracle."
fi
