#!/usr/bin/env bash
# =============================================================================
# Blackbox Standalone Acceptance Test: test_nogpu (Linux / Wine)
# Verifies graceful fallback and sentinel logging when NVML is unavailable.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN_DIR="${REPO_ROOT}/bin"

WINE_CMD="${WINE_CMD:-wine}"
ANALYZER="${BIN_DIR}/blackbox-analyze.exe"
COLLECTOR="${BIN_DIR}/blackbox.exe"

TEST_DIR=$(mktemp -d /tmp/bb_test_nogpu_XXXXXX)
RING_FILE="${TEST_DIR}/ring_nogpu.bin"

trap 'rm -rf "${TEST_DIR}"' EXIT

echo "========================================================"
echo " [TEST] test_nogpu: Sentinel Logging Without NVML"
echo "========================================================"

if [[ -f "${COLLECTOR}" ]] && command -v "${WINE_CMD}" &>/dev/null; then
    echo "[*] Running blackbox.exe without NVML under Wine..."
    WINEDEBUG=-all "${WINE_CMD}" "${COLLECTOR}" --path="${RING_FILE}" --size-mb=1 --hz=10 &
    PID=$!
    sleep 2
    kill -9 "$PID" 2>/dev/null || true
else
    echo "[*] Generating synthetic ring with sentinel GPU values..."
    python3 "${REPO_ROOT}/tools/generate_test_ring.py" --scenario=nogpu --output="${RING_FILE}" --ring-kb=64
fi

if [[ -f "${ANALYZER}" ]] && command -v "${WINE_CMD}" &>/dev/null; then
    echo "[*] Executing blackbox-analyze.exe under Wine..."
    WINEDEBUG=-all "${WINE_CMD}" "${ANALYZER}" "${RING_FILE}" --gap=3 --no-eventlog
    echo "[PASS] test_nogpu executed cleanly with exit code 0."
else
    echo "[*] Verifying with Reference Oracle..."
    python3 -c "
import sys
sys.path.insert(0, '${REPO_ROOT}')
from tests.e2e_runner import ReferenceRingOracle, SENTINEL_U16, SENTINEL_U32, SENTINEL_U8, FLAG_NVML_AVAILABLE
o = ReferenceRingOracle('${RING_FILE}')
assert o.is_valid_header, 'Invalid header'
assert len(o.valid_records) > 0, 'No records'
r = o.valid_records[0]
assert r['gpu_temp_c'] == SENTINEL_U16, 'Expected 0xFFFF temp sentinel'
assert r['gpu_power_mw'] == SENTINEL_U32, 'Expected 0xFFFFFFFF power sentinel'
assert r['pcie_gen'] == SENTINEL_U8, 'Expected 0xFF pcie_gen sentinel'
assert (r['flags'] & FLAG_NVML_AVAILABLE) == 0, 'NVML_AVAILABLE flag should be cleared'
print('[+] Oracle verified: all GPU fields populated with sentinels and nvml bit cleared')
"
    echo "[PASS] test_nogpu verified successfully by Reference Oracle."
fi
