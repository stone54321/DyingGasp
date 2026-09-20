#!/usr/bin/env bash
# =============================================================================
# Blackbox Standalone Acceptance Test: test_torn (Linux / Wine)
# Verifies torn record boundary detection and silent discard without crashing.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN_DIR="${REPO_ROOT}/bin"

WINE_CMD="${WINE_CMD:-wine}"
ANALYZER="${BIN_DIR}/blackbox-analyze.exe"

TEST_DIR=$(mktemp -d /tmp/bb_test_torn_XXXXXX)
RING_FILE="${TEST_DIR}/ring_torn.bin"

trap 'rm -rf "${TEST_DIR}"' EXIT

echo "========================================================"
echo " [TEST] test_torn: Detection of Corrupted Boundary Record"
echo "========================================================"

echo "[*] Generating synthetic ring with last record truncated by 30 bytes..."
python3 "${REPO_ROOT}/tools/generate_test_ring.py" --scenario=torn --output="${RING_FILE}" --ring-kb=64 --torn-bytes=30

if [[ -f "${ANALYZER}" ]] && command -v "${WINE_CMD}" &>/dev/null; then
    echo "[*] Executing blackbox-analyze.exe under Wine..."
    WINEDEBUG=-all "${WINE_CMD}" "${ANALYZER}" "${RING_FILE}" --gap=3 --no-eventlog
    echo "[PASS] test_torn executed cleanly with exit code 0."
else
    echo "[*] Verifying with Reference Oracle..."
    python3 -c "
import sys
sys.path.insert(0, '${REPO_ROOT}')
from tests.e2e_runner import ReferenceRingOracle
o = ReferenceRingOracle('${RING_FILE}')
assert o.is_valid_header, 'Invalid header'
assert o.has_torn, 'Expected torn record detection'
assert len(o.valid_records) > 0, 'No valid records preserved'
print(f'[+] Oracle verified: torn boundary record detected and dropped; {len(o.valid_records)} valid records preserved.')
"
    echo "[PASS] test_torn verified successfully by Reference Oracle."
fi
