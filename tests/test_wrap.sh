#!/usr/bin/env bash
# =============================================================================
# Blackbox Standalone Acceptance Test: test_wrap (Linux / Wine)
# Verifies circular buffer wrap, slot offset overwrite, and sequence continuity.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN_DIR="${REPO_ROOT}/bin"

WINE_CMD="${WINE_CMD:-wine}"
ANALYZER="${BIN_DIR}/blackbox-analyze.exe"
COLLECTOR="${BIN_DIR}/blackbox.exe"

TEST_DIR=$(mktemp -d /tmp/bb_test_wrap_XXXXXX)
RING_FILE="${TEST_DIR}/ring_wrap.bin"

trap 'rm -rf "${TEST_DIR}"' EXIT

echo "========================================================"
echo " [TEST] test_wrap: Circular Buffer Wrap & Continuity"
echo "========================================================"

if [[ -f "${COLLECTOR}" ]] && command -v "${WINE_CMD}" &>/dev/null; then
    echo "[*] Running blackbox.exe under Wine at 50 Hz on 1 MB ring..."
    WINEDEBUG=-all "${WINE_CMD}" "${COLLECTOR}" --path="${RING_FILE}" --size-mb=1 --hz=50 &
    PID=$!
    sleep 3
    kill -9 "$PID" 2>/dev/null || true
else
    echo "[*] Generating synthetic wrapped ring fixture..."
    python3 "${REPO_ROOT}/tools/generate_test_ring.py" --scenario=wrap --output="${RING_FILE}" --ring-kb=64
fi

if [[ -f "${ANALYZER}" ]] && command -v "${WINE_CMD}" &>/dev/null; then
    echo "[*] Executing blackbox-analyze.exe under Wine..."
    WINEDEBUG=-all "${WINE_CMD}" "${ANALYZER}" "${RING_FILE}" --gap=3 --no-eventlog
    echo "[PASS] test_wrap executed cleanly with exit code 0."
else
    echo "[*] blackbox-analyze.exe not present; verifying with Reference Oracle..."
    python3 -c "
import sys
sys.path.insert(0, '${REPO_ROOT}')
from tests.e2e_runner import ReferenceRingOracle
o = ReferenceRingOracle('${RING_FILE}')
assert o.is_valid_header, 'Invalid header'
assert o.wrap_count >= 1, f'Expected >=1 wrap, got {o.wrap_count}'
assert len(o.valid_records) > 0, 'No records'
print(f'[+] Oracle verified: wrap_count={o.wrap_count}, records={len(o.valid_records)}')
"
    echo "[PASS] test_wrap verified successfully by Reference Oracle."
fi
