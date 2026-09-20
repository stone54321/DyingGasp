#!/usr/bin/env python3
"""
tests/test_adversarial.py - Comprehensive Adversarial Stress Test Suite
Crash Analyzer (blackbox-analyze.exe)

Empirical verification of:
- Suite 1: Header Truncation & Zero/Small Files (0B, 1B, 2B, 64B, 512B, 4095B, 4096B)
- Suite 2: Header Invariant Violations (BADMAG01, zero magic, 0xFF magic, version 0/2/9999,
           header_size 2048/8192, record_size 32/128, record_count 0, massive record_count 0xFFFFFFFF)
- Suite 3: Truncated Ring Slots at Arbitrary Byte Boundaries (1B, 15B, 30B, 63B, mid-ring torn,
           corrupt type, seq misalignment, test_torn 30B truncation)
- Suite 4: Massive Sequence Numbers & UINT64_MAX Wrap-around (10 trillion monotonic,
           near-UINT64_MAX monotonic, wrap across UINT64_MAX boundary)
- Suite 5: 10,000 Synthetic Records with Multiple Overlapping Anomalies (Quad-anomaly cascade,
           dual anomalies, pre-crash tail quad-anomaly)
- Suite 6: Multi-Gap Selection & CLI Ergonomics (default latest gap, --gap=1, --gap=2, --gap=0,
           --gap=999 out of bounds, --gap=-1, clean tail with --gap, missing argument error)
- Suite 7: Auto-Discovery Directory Stress (single ring.bin, multi-crash selection of newest,
           skip clean crash and pick gap crash, fallback to ring.bin, empty dir, non-existent path)
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from typing import Dict, List, Optional, Tuple

# Ensure project root is in python path
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT_DIR)

from tools.generate_test_ring import (
    BLACKBOX_MAGIC,
    BLACKBOX_VERSION,
    HEADER_SIZE,
    RECORD_SIZE,
    RingBufferGenerator,
    build_header,
    pack_type0_data,
    pack_type1_process,
    pack_type2_cal,
    pack_type3_heartbeat,
)

ANALYZER_BIN = os.path.join(ROOT_DIR, "bin", "blackbox-analyze.exe")
TMP_TEST_DIR = os.path.join(tempfile.gettempdir(), "blackbox_m6_adversarial")


@dataclass
class TestCaseResult:
    test_id: str
    suite: str
    name: str
    command: List[str]
    expected_exit: int
    actual_exit: int
    stdout_snippets: List[str]
    stderr_snippets: List[str]
    assertions_passed: bool
    segfault_detected: bool
    verdict: str  # "PASS" or "FAIL"
    error_message: str = ""


class AdversarialTestSuite:
    def __init__(self, analyzer_path: str = ANALYZER_BIN):
        self.analyzer_path = analyzer_path
        self.results: List[TestCaseResult] = []
        os.makedirs(TMP_TEST_DIR, exist_ok=True)

    def run_analyzer(
        self,
        args: List[str],
        cwd: Optional[str] = None,
        timeout_sec: int = 30,
    ) -> Tuple[int, str, str]:
        """Runs blackbox-analyze.exe via Wine and returns (exit_code, stdout, stderr)."""
        cmd = ["wine", self.analyzer_path] + args
        env = os.environ.copy()
        env["WINEDEBUG"] = "-all"
        try:
            res = subprocess.run(
                cmd,
                cwd=cwd or ROOT_DIR,
                capture_output=True,
                text=True,
                env=env,
                timeout=timeout_sec,
            )
            return res.returncode, res.stdout, res.stderr
        except subprocess.TimeoutExpired as te:
            out = te.stdout.decode("utf-8", errors="replace") if isinstance(te.stdout, bytes) else (te.stdout or "")
            err = te.stderr.decode("utf-8", errors="replace") if isinstance(te.stderr, bytes) else (te.stderr or "")
            return -999, out, err + "\nTIMEOUT"
        except Exception as e:
            return -998, "", str(e)

    def add_result(
        self,
        test_id: str,
        suite: str,
        name: str,
        command: List[str],
        expected_exit: int,
        actual_exit: int,
        stdout: str,
        stderr: str,
        expected_stdout_substrings: List[str],
        expected_stderr_substrings: List[str],
    ):
        segfault = (
            actual_exit in (139, -11)
            or "segmentation fault" in stderr.lower()
            or "page fault" in stderr.lower()
            or "unhandled exception" in stderr.lower()
            or actual_exit < 0
        )

        checks_passed = True
        err_msgs = []

        if actual_exit != expected_exit:
            checks_passed = False
            err_msgs.append(f"Exit code mismatch: expected {expected_exit}, got {actual_exit}")

        if segfault:
            checks_passed = False
            err_msgs.append("Process crash or segmentation fault detected!")

        for s in expected_stdout_substrings:
            if s not in stdout:
                checks_passed = False
                err_msgs.append(f"Missing expected stdout substring: '{s}'")

        for s in expected_stderr_substrings:
            if s not in stderr:
                checks_passed = False
                err_msgs.append(f"Missing expected stderr substring: '{s}'")

        verdict = "PASS" if checks_passed else "FAIL"

        result = TestCaseResult(
            test_id=test_id,
            suite=suite,
            name=name,
            command=command,
            expected_exit=expected_exit,
            actual_exit=actual_exit,
            stdout_snippets=[line.strip() for line in stdout.splitlines()[:10]],
            stderr_snippets=[line.strip() for line in stderr.splitlines()[:5]],
            assertions_passed=checks_passed,
            segfault_detected=segfault,
            verdict=verdict,
            error_message="; ".join(err_msgs),
        )
        self.results.append(result)
        status_sym = "[+]" if verdict == "PASS" else "[-]"
        print(f"  {status_sym} {test_id}: {name} -> {verdict} (exit={actual_exit})")
        if not checks_passed:
            for em in err_msgs:
                print(f"      ERROR: {em}")

    # =========================================================================
    # Suite 1: Header Truncation & Zero/Small Files
    # =========================================================================
    def run_suite_1(self):
        print("\n========================================================")
        print(" SUITE 1: Header Truncation & Zero/Small Files")
        print("========================================================")
        suite_dir = os.path.join(TMP_TEST_DIR, "suite1")
        os.makedirs(suite_dir, exist_ok=True)

        # T1.1: 0-byte file
        p_0b = os.path.join(suite_dir, "file_0b.bin")
        open(p_0b, "wb").close()
        code, out, err = self.run_analyzer([p_0b, "--no-eventlog"])
        self.add_result(
            "T1.1", "Suite 1", "0-byte empty file", ["wine", self.analyzer_path, p_0b],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["smaller than 4 KB header"],
        )

        # T1.2: 1-byte file
        p_1b = os.path.join(suite_dir, "file_1b.bin")
        with open(p_1b, "wb") as f:
            f.write(b"X")
        code, out, err = self.run_analyzer([p_1b, "--no-eventlog"])
        self.add_result(
            "T1.2", "Suite 1", "1-byte truncated file", ["wine", self.analyzer_path, p_1b],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["smaller than 4 KB header"],
        )

        # T1.3: 2-byte file
        p_2b = os.path.join(suite_dir, "file_2b.bin")
        with open(p_2b, "wb") as f:
            f.write(b"\x42\x43")
        code, out, err = self.run_analyzer([p_2b, "--no-eventlog"])
        self.add_result(
            "T1.3", "Suite 1", "2-byte truncated file", ["wine", self.analyzer_path, p_2b],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["smaller than 4 KB header"],
        )

        # T1.4: 64-byte file
        p_64b = os.path.join(suite_dir, "file_64b.bin")
        with open(p_64b, "wb") as f:
            f.write(b"\xAA" * 64)
        code, out, err = self.run_analyzer([p_64b, "--no-eventlog"])
        self.add_result(
            "T1.4", "Suite 1", "64-byte truncated file", ["wine", self.analyzer_path, p_64b],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["smaller than 4 KB header"],
        )

        # T1.5: 512-byte file
        p_512b = os.path.join(suite_dir, "file_512b.bin")
        with open(p_512b, "wb") as f:
            f.write(b"\x55" * 512)
        code, out, err = self.run_analyzer([p_512b, "--no-eventlog"])
        self.add_result(
            "T1.5", "Suite 1", "512-byte truncated file", ["wine", self.analyzer_path, p_512b],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["smaller than 4 KB header"],
        )

        # T1.6: 4095-byte file (1 byte short of full 4 KB header)
        hdr_full = build_header(ring_bytes=64 * 1024)
        p_4095b = os.path.join(suite_dir, "file_4095b.bin")
        with open(p_4095b, "wb") as f:
            f.write(hdr_full[:4095])
        code, out, err = self.run_analyzer([p_4095b, "--no-eventlog"])
        self.add_result(
            "T1.6", "Suite 1", "4095-byte file (truncated header by 1 byte)", ["wine", self.analyzer_path, p_4095b],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["smaller than 4 KB header"],
        )

        # T1.7: Exactly 4096-byte file (Header only, zero slots written)
        p_4096b = os.path.join(suite_dir, "file_4096b.bin")
        with open(p_4096b, "wb") as f:
            f.write(hdr_full)
        code, out, err = self.run_analyzer([p_4096b, "--no-eventlog"])
        self.add_result(
            "T1.7", "Suite 1", "Exactly 4096-byte header with zero records", ["wine", self.analyzer_path, p_4096b],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "RING HEALTH SUMMARY",
                "Total Valid Records   : 0",
                "[TORN RECORD]",
                "NO GAP DETECTED (clean tail)",
            ],
            expected_stderr_substrings=[],
        )

    # =========================================================================
    # Suite 2: Header Invariant Violations
    # =========================================================================
    def run_suite_2(self):
        print("\n========================================================")
        print(" SUITE 2: Header Invariant Violations")
        print("========================================================")
        suite_dir = os.path.join(TMP_TEST_DIR, "suite2")
        os.makedirs(suite_dir, exist_ok=True)

        base_hdr = build_header(ring_bytes=64 * 1024)

        # T2.1: Corrupt Magic ASCII "BADMAG01"
        hdr_badmag = bytearray(base_hdr)
        hdr_badmag[0:8] = b"BADMAG01"
        p_badmag = os.path.join(suite_dir, "badmag.bin")
        with open(p_badmag, "wb") as f:
            f.write(hdr_badmag)
        code, out, err = self.run_analyzer([p_badmag, "--no-eventlog"])
        self.add_result(
            "T2.1", "Suite 2", "Corrupt magic 'BADMAG01'", ["wine", self.analyzer_path, p_badmag],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Invalid blackbox ring buffer header"],
        )

        # T2.2: Corrupt Magic All Zeroes
        hdr_zero_mag = bytearray(base_hdr)
        hdr_zero_mag[0:8] = b"\x00" * 8
        p_zero_mag = os.path.join(suite_dir, "zero_mag.bin")
        with open(p_zero_mag, "wb") as f:
            f.write(hdr_zero_mag)
        code, out, err = self.run_analyzer([p_zero_mag, "--no-eventlog"])
        self.add_result(
            "T2.2", "Suite 2", "Corrupt magic all zeroes", ["wine", self.analyzer_path, p_zero_mag],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Invalid blackbox ring buffer header"],
        )

        # T2.3: Corrupt Magic All 0xFF
        hdr_ff_mag = bytearray(base_hdr)
        hdr_ff_mag[0:8] = b"\xFF" * 8
        p_ff_mag = os.path.join(suite_dir, "ff_mag.bin")
        with open(p_ff_mag, "wb") as f:
            f.write(hdr_ff_mag)
        code, out, err = self.run_analyzer([p_ff_mag, "--no-eventlog"])
        self.add_result(
            "T2.3", "Suite 2", "Corrupt magic all 0xFF", ["wine", self.analyzer_path, p_ff_mag],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Invalid blackbox ring buffer header"],
        )

        # T2.4: Invalid Version = 0
        hdr_v0 = bytearray(base_hdr)
        hdr_v0[8:12] = (0).to_bytes(4, "little")
        p_v0 = os.path.join(suite_dir, "version_0.bin")
        with open(p_v0, "wb") as f:
            f.write(hdr_v0)
        code, out, err = self.run_analyzer([p_v0, "--no-eventlog"])
        self.add_result(
            "T2.4", "Suite 2", "Invalid version 0", ["wine", self.analyzer_path, p_v0],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Invalid blackbox ring buffer header"],
        )

        # T2.5: Invalid Version = 2
        hdr_v2 = bytearray(base_hdr)
        hdr_v2[8:12] = (2).to_bytes(4, "little")
        p_v2 = os.path.join(suite_dir, "version_2.bin")
        with open(p_v2, "wb") as f:
            f.write(hdr_v2)
        code, out, err = self.run_analyzer([p_v2, "--no-eventlog"])
        self.add_result(
            "T2.5", "Suite 2", "Invalid version 2", ["wine", self.analyzer_path, p_v2],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Invalid blackbox ring buffer header"],
        )

        # T2.6: Invalid Version = 9999
        hdr_v9999 = bytearray(base_hdr)
        hdr_v9999[8:12] = (9999).to_bytes(4, "little")
        p_v9999 = os.path.join(suite_dir, "version_9999.bin")
        with open(p_v9999, "wb") as f:
            f.write(hdr_v9999)
        code, out, err = self.run_analyzer([p_v9999, "--no-eventlog"])
        self.add_result(
            "T2.6", "Suite 2", "Invalid version 9999", ["wine", self.analyzer_path, p_v9999],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Invalid blackbox ring buffer header"],
        )

        # T2.7: Invalid Header Size = 2048
        hdr_hs2048 = bytearray(base_hdr)
        hdr_hs2048[12:16] = (2048).to_bytes(4, "little")
        p_hs2048 = os.path.join(suite_dir, "hdrsize_2048.bin")
        with open(p_hs2048, "wb") as f:
            f.write(hdr_hs2048)
        code, out, err = self.run_analyzer([p_hs2048, "--no-eventlog"])
        self.add_result(
            "T2.7", "Suite 2", "Invalid header_size 2048", ["wine", self.analyzer_path, p_hs2048],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Invalid blackbox ring buffer header"],
        )

        # T2.8: Invalid Record Size = 32
        hdr_rs32 = bytearray(base_hdr)
        hdr_rs32[24:28] = (32).to_bytes(4, "little")
        p_rs32 = os.path.join(suite_dir, "recsize_32.bin")
        with open(p_rs32, "wb") as f:
            f.write(hdr_rs32)
        code, out, err = self.run_analyzer([p_rs32, "--no-eventlog"])
        self.add_result(
            "T2.8", "Suite 2", "Invalid record_size 32", ["wine", self.analyzer_path, p_rs32],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Invalid blackbox ring buffer header"],
        )

        # T2.9: Record Count = 0
        hdr_rc0 = bytearray(base_hdr)
        hdr_rc0[28:32] = (0).to_bytes(4, "little")
        p_rc0 = os.path.join(suite_dir, "reccount_0.bin")
        with open(p_rc0, "wb") as f:
            f.write(hdr_rc0)
        code, out, err = self.run_analyzer([p_rc0, "--no-eventlog"])
        self.add_result(
            "T2.9", "Suite 2", "Header record_count = 0", ["wine", self.analyzer_path, p_rc0],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Header specifies 0 record slots"],
        )

        # T2.10: Massive Record Count = 0xFFFFFFFF (274 GB RAM allocation stress)
        hdr_rc_max = bytearray(base_hdr)
        hdr_rc_max[28:32] = b"\xFF\xFF\xFF\xFF"
        p_rc_max = os.path.join(suite_dir, "reccount_max.bin")
        with open(p_rc_max, "wb") as f:
            f.write(hdr_rc_max)
        code, out, err = self.run_analyzer([p_rc_max, "--no-eventlog"])
        self.add_result(
            "T2.10", "Suite 2", "Massive record_count 0xFFFFFFFF allocation guard", ["wine", self.analyzer_path, p_rc_max],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Failed to allocate memory for 4294967295 record slots"],
        )

    # =========================================================================
    # Suite 3: Truncated Ring Slots at Arbitrary Byte Boundaries
    # =========================================================================
    def run_suite_3(self):
        print("\n========================================================")
        print(" SUITE 3: Truncated Ring Slots at Arbitrary Byte Boundaries")
        print("========================================================")
        suite_dir = os.path.join(TMP_TEST_DIR, "suite3")
        os.makedirs(suite_dir, exist_ok=True)

        record_count = 100
        ring_bytes = HEADER_SIZE + record_count * RECORD_SIZE
        hdr = build_header(ring_bytes=ring_bytes, nominal_hz=10)

        # Create 50 valid records
        records = [
            pack_type0_data(seq=i, qpc_100ns=100_000_000 + i * 1_000_000, tsc=3_000_000_000 + i * 300_000)
            for i in range(50)
        ]
        base_body = bytearray()
        for r in records:
            base_body.extend(r)

        rec50 = pack_type0_data(seq=50, qpc_100ns=100_000_000 + 50 * 1_000_000, tsc=3_000_000_000 + 50 * 300_000)

        # T3.1: Physical truncation at +1 byte
        data_1b = hdr + base_body + rec50[:1]
        p_1b = os.path.join(suite_dir, "trunc_1b.bin")
        with open(p_1b, "wb") as f:
            f.write(data_1b)
        code, out, err = self.run_analyzer([p_1b, "--no-eventlog"])
        self.add_result(
            "T3.1", "Suite 3", "Arbitrary truncation at +1 byte boundary", ["wine", self.analyzer_path, p_1b],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Total Valid Records   : 50", "[TORN RECORD]"],
            expected_stderr_substrings=[],
        )

        # T3.2: Physical truncation at +15 bytes
        data_15b = hdr + base_body + rec50[:15]
        p_15b = os.path.join(suite_dir, "trunc_15b.bin")
        with open(p_15b, "wb") as f:
            f.write(data_15b)
        code, out, err = self.run_analyzer([p_15b, "--no-eventlog"])
        self.add_result(
            "T3.2", "Suite 3", "Arbitrary truncation at +15 bytes boundary", ["wine", self.analyzer_path, p_15b],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Total Valid Records   : 50", "[TORN RECORD]"],
            expected_stderr_substrings=[],
        )

        # T3.3: Physical truncation at +30 bytes
        data_30b = hdr + base_body + rec50[:30]
        p_30b = os.path.join(suite_dir, "trunc_30b.bin")
        with open(p_30b, "wb") as f:
            f.write(data_30b)
        code, out, err = self.run_analyzer([p_30b, "--no-eventlog"])
        self.add_result(
            "T3.3", "Suite 3", "Arbitrary truncation at +30 bytes boundary", ["wine", self.analyzer_path, p_30b],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Total Valid Records   : 50", "[TORN RECORD]"],
            expected_stderr_substrings=[],
        )

        # T3.4: Physical truncation at +63 bytes (1 byte short of record 50)
        data_63b = hdr + base_body + rec50[:63]
        p_63b = os.path.join(suite_dir, "trunc_63b.bin")
        with open(p_63b, "wb") as f:
            f.write(data_63b)
        code, out, err = self.run_analyzer([p_63b, "--no-eventlog"])
        self.add_result(
            "T3.4", "Suite 3", "Arbitrary truncation at +63 bytes boundary", ["wine", self.analyzer_path, p_63b],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Total Valid Records   : 50", "[TORN RECORD]"],
            expected_stderr_substrings=[],
        )

        # T3.5: Pre-allocated full ring with mid-slot write interrupted at 25 bytes (before sequence committed)
        data_padded = bytearray(hdr)
        data_padded.extend(base_body)
        data_padded.extend(rec50[:25])
        remaining = ring_bytes - len(data_padded)
        data_padded.extend(b"\x00" * remaining)
        p_padded = os.path.join(suite_dir, "midring_torn_25b.bin")
        with open(p_padded, "wb") as f:
            f.write(data_padded)
        code, out, err = self.run_analyzer([p_padded, "--no-eventlog"])
        self.add_result(
            "T3.5", "Suite 3", "Mid-slot write interruption at 25 bytes in pre-allocated ring",
            ["wine", self.analyzer_path, p_padded],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Total Valid Records   : 50", "[TORN RECORD]"],
            expected_stderr_substrings=[],
        )

        # T3.6: Corrupt record type in slot 20 (type=99)
        data_corrupt_type = bytearray(hdr)
        for i in range(50):
            if i == 20:
                corrupt_rec = bytearray(records[i])
                corrupt_rec[0] = 99  # Invalid record type
                data_corrupt_type.extend(corrupt_rec)
            else:
                data_corrupt_type.extend(records[i])
        data_corrupt_type.extend(b"\x00" * (ring_bytes - len(data_corrupt_type)))
        p_corrupt_type = os.path.join(suite_dir, "corrupt_type.bin")
        with open(p_corrupt_type, "wb") as f:
            f.write(data_corrupt_type)
        code, out, err = self.run_analyzer([p_corrupt_type, "--no-eventlog"])
        self.add_result(
            "T3.6", "Suite 3", "Corrupt record type=99 in slot 20", ["wine", self.analyzer_path, p_corrupt_type],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Total Valid Records   : 49", "[TORN RECORD]"],
            expected_stderr_substrings=[],
        )

        # T3.7: Slot sequence misalignment in slot 10 (seq=11 instead of 10)
        data_misaligned = bytearray(hdr)
        for i in range(50):
            if i == 10:
                bad_rec = pack_type0_data(seq=11, qpc_100ns=100_000_000, tsc=3_000_000_000)
                data_misaligned.extend(bad_rec)
            else:
                data_misaligned.extend(records[i])
        data_misaligned.extend(b"\x00" * (ring_bytes - len(data_misaligned)))
        p_misaligned = os.path.join(suite_dir, "misaligned_seq.bin")
        with open(p_misaligned, "wb") as f:
            f.write(data_misaligned)
        code, out, err = self.run_analyzer([p_misaligned, "--no-eventlog"])
        self.add_result(
            "T3.7", "Suite 3", "Misaligned slot sequence invariant violation", ["wine", self.analyzer_path, p_misaligned],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Total Valid Records   : 49", "[TORN RECORD]"],
            expected_stderr_substrings=[],
        )

        # T3.8: Authoritative test_torn fixture truncated by 30 bytes
        gen_torn = RingBufferGenerator(ring_bytes=64 * 1024)
        gen_torn.generate_scenario("torn")
        p_torn30 = os.path.join(suite_dir, "authoritative_torn30.bin")
        gen_torn.write_to_file(p_torn30, truncate_bytes=30)
        code, out, err = self.run_analyzer([p_torn30, "--no-eventlog"])
        self.add_result(
            "T3.8", "Suite 3", "Authoritative test_torn 30-byte truncation scenario",
            ["wine", self.analyzer_path, p_torn30],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["[TORN RECORD]"],
            expected_stderr_substrings=[],
        )

    # =========================================================================
    # Suite 4: Massive Sequence Numbers & UINT64_MAX Wrap-around
    # =========================================================================
    def run_suite_4(self):
        print("\n========================================================")
        print(" SUITE 4: Massive Sequence Numbers & UINT64_MAX Wrap")
        print("========================================================")
        suite_dir = os.path.join(TMP_TEST_DIR, "suite4")
        os.makedirs(suite_dir, exist_ok=True)

        record_count = 100
        ring_bytes = HEADER_SIZE + record_count * RECORD_SIZE
        hdr = build_header(ring_bytes=ring_bytes, nominal_hz=10)

        # T4.1: Massive monotonic sequence: 10 trillion (10,000,000,000,000)
        base_seq_10t = 10_000_000_000_000
        slots_10t = [b"\x00" * 64] * record_count
        for i in range(100):
            s = base_seq_10t + i
            slot = s % record_count
            slots_10t[slot] = pack_type0_data(
                seq=s,
                qpc_100ns=100_000_000 + i * 1_000_000,
                tsc=3_000_000_000 + i * 300_000,
            )
        data_10t = bytearray(hdr)
        for s in slots_10t:
            data_10t.extend(s)
        p_10t = os.path.join(suite_dir, "massive_10t.bin")
        with open(p_10t, "wb") as f:
            f.write(data_10t)
        code, out, err = self.run_analyzer([p_10t, "--no-eventlog"])
        self.add_result(
            "T4.1", "Suite 4", "Massive monotonic sequence (10 Trillion)", ["wine", self.analyzer_path, p_10t],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "Total Valid Records   : 100",
                "Seq 10000000000000",
                "Seq 10000000000099",
                "NO GAP DETECTED (clean tail)",
            ],
            expected_stderr_substrings=[],
        )

        # T4.2: Massive monotonic sequence near UINT64_MAX limit
        base_seq_max = 18_446_744_073_709_551_000
        slots_max = [b"\x00" * 64] * record_count
        for i in range(100):
            s = base_seq_max + i
            slot = s % record_count
            slots_max[slot] = pack_type0_data(
                seq=s,
                qpc_100ns=100_000_000 + i * 1_000_000,
                tsc=3_000_000_000 + i * 300_000,
            )
        data_max = bytearray(hdr)
        for s in slots_max:
            data_max.extend(s)
        p_max = os.path.join(suite_dir, "massive_near_uint64_max.bin")
        with open(p_max, "wb") as f:
            f.write(data_max)
        code, out, err = self.run_analyzer([p_max, "--no-eventlog"])
        self.add_result(
            "T4.2", "Suite 4", "Massive sequence near UINT64_MAX", ["wine", self.analyzer_path, p_max],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "Total Valid Records   : 100",
                "18446744073709551000",
                "18446744073709551099",
                "NO GAP DETECTED (clean tail)",
            ],
            expected_stderr_substrings=[],
        )

        # T4.3: Sequence wrap-around across UINT64_MAX boundary (UINT64_MAX -> 0)
        rc_wrap = 10
        hdr_wrap = build_header(ring_bytes=HEADER_SIZE + rc_wrap * RECORD_SIZE, nominal_hz=10)
        slots_wrap = [b"\x00" * 64] * rc_wrap
        UINT64_MAX = 0xFFFFFFFFFFFFFFFF

        seq_list = [UINT64_MAX - 2, UINT64_MAX - 1, UINT64_MAX, 0, 1, 2]
        for idx, s in enumerate(seq_list):
            slot = s % rc_wrap
            slots_wrap[slot] = pack_type0_data(
                seq=s,
                qpc_100ns=100_000_000 + idx * 1_000_000,
                tsc=3_000_000_000 + idx * 300_000,
            )
        data_wrap = bytearray(hdr_wrap)
        for s in slots_wrap:
            data_wrap.extend(s)
        p_wrap = os.path.join(suite_dir, "uint64_max_wrap.bin")
        with open(p_wrap, "wb") as f:
            f.write(data_wrap)
        code, out, err = self.run_analyzer([p_wrap, "--no-eventlog"])
        self.add_result(
            "T4.3", "Suite 4", "Sequence wrap-around across UINT64_MAX boundary", ["wine", self.analyzer_path, p_wrap],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "Total Valid Records   : 6",
                "DETECTED TIMELINE GAPS",
                "Pre-Gap Seq",
            ],
            expected_stderr_substrings=[],
        )

    # =========================================================================
    # Suite 5: 10,000 Synthetic Records with Multiple Overlapping Anomalies
    # =========================================================================
    def run_suite_5(self):
        print("\n========================================================")
        print(" SUITE 5: 10,000 Records with Multiple Overlapping Anomalies")
        print("========================================================")
        suite_dir = os.path.join(TMP_TEST_DIR, "suite5")
        os.makedirs(suite_dir, exist_ok=True)

        record_count = 10000
        ring_bytes = HEADER_SIZE + record_count * RECORD_SIZE
        hdr = build_header(ring_bytes=ring_bytes, nominal_hz=10)

        records = []
        qpc = 100_000_000
        tsc = 3_000_000_000
        power = 150_000
        gen = 4
        width = 16
        replay = 0
        cpu = 20

        for i in range(record_count):
            if i == 2500:
                # Quad-Anomaly: HANG + POWER SPIKE + PCIE DROP + REPLAY JUMP
                qpc += 6_000_000  # 600 ms (> 300 ms = HANG)
                tsc += 1_800_000_000
                power += 150_000  # +150 W (> 100 W = POWER SPIKE)
                gen = 3           # Gen4 -> Gen3 = PCIE DROP
                width = 8         # x16 -> x8 = PCIE DROP
                replay += 25      # +25 = REPLAY JUMP
            elif i == 5000:
                # Dual Anomaly: HANG + POWER SPIKE (drop)
                qpc += 5_000_000  # 500 ms = HANG
                tsc += 1_500_000_000
                power -= 120_000  # -120 W = POWER SPIKE
            elif i == 7500:
                # Dual Anomaly: PCIE DROP + REPLAY JUMP
                qpc += 1_000_000
                tsc += 300_000_000
                gen = 2           # Gen3 -> Gen2 = PCIE DROP
                replay += 10      # +10 = REPLAY JUMP
            elif i == 9999:
                # Quad Anomaly at Tail: HANG + POWER SPIKE + PCIE DROP + REPLAY JUMP
                qpc += 8_000_000  # 800 ms = HANG
                tsc += 2_400_000_000
                power += 180_000  # +180 W = POWER SPIKE
                gen = 1           # Gen2 -> Gen1 = PCIE DROP
                width = 4         # x8 -> x4 = PCIE DROP
                replay += 50      # +50 = REPLAY JUMP
            else:
                qpc += 1_000_000
                tsc += 300_000_000

            rec = pack_type0_data(
                seq=i,
                qpc_100ns=qpc,
                tsc=tsc,
                pcie_gen=gen,
                pcie_width=width,
                cpu_pct=cpu,
                gpu_temp_c=65,
                gpu_power_mw=power,
                pcie_replay=replay,
            )
            records.append(rec)

        data = bytearray(hdr)
        for r in records:
            data.extend(r)

        p_10k = os.path.join(suite_dir, "10k_overlapping.bin")
        with open(p_10k, "wb") as f:
            f.write(data)

        code, out, err = self.run_analyzer([p_10k, "--no-eventlog"])

        # T5.1: Quad-anomaly cascade at Seq 2500
        self.add_result(
            "T5.1", "Suite 5", "Quad-Anomaly Cascade at Seq 2500 (HANG+POWER+PCIE+REPLAY)",
            ["wine", self.analyzer_path, p_10k],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "[HANG] Seq 2500",
                "[POWER SPIKE] Seq 2500",
                "[PCIE DROP] Seq 2500",
                "[REPLAY JUMP] Seq 2500",
            ],
            expected_stderr_substrings=[],
        )

        # T5.2: Overlapping Dual Anomaly at Seq 5000 (HANG + POWER SPIKE)
        self.add_result(
            "T5.2", "Suite 5", "Dual Anomaly at Seq 5000 (HANG + POWER SPIKE)",
            ["wine", self.analyzer_path, p_10k],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "[HANG] Seq 5000",
                "[POWER SPIKE] Seq 5000",
            ],
            expected_stderr_substrings=[],
        )

        # T5.3: Overlapping Dual Anomaly at Seq 7500 (PCIE DROP + REPLAY JUMP)
        self.add_result(
            "T5.3", "Suite 5", "Dual Anomaly at Seq 7500 (PCIE DROP + REPLAY JUMP)",
            ["wine", self.analyzer_path, p_10k],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "[PCIE DROP] Seq 7500",
                "[REPLAY JUMP] Seq 7500",
            ],
            expected_stderr_substrings=[],
        )

        # T5.4: Quad Anomaly at Tail Record 9999 + Table Inspection Window
        self.add_result(
            "T5.4", "Suite 5", "Pre-Crash Tail Quad Anomaly at Seq 9999",
            ["wine", self.analyzer_path, p_10k],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "Total Valid Records   : 10000",
                "[HANG] Seq 9999",
                "[POWER SPIKE] Seq 9999",
                "[PCIE DROP] Seq 9999",
                "[REPLAY JUMP] Seq 9999",
                "9999",
                "<-- [HANG]",
            ],
            expected_stderr_substrings=[],
        )

    # =========================================================================
    # Suite 6: Multi-Gap Selection & CLI Ergonomics
    # =========================================================================
    def run_suite_6(self):
        print("\n========================================================")
        print(" SUITE 6: Multi-Gap Selection & CLI Ergonomics")
        print("========================================================")
        suite_dir = os.path.join(TMP_TEST_DIR, "suite6")
        os.makedirs(suite_dir, exist_ok=True)

        # Multi-gap fixture (3 gaps)
        gen = RingBufferGenerator(ring_bytes=64 * 1024)
        gen.generate_scenario("multigap")
        p_mg = os.path.join(suite_dir, "multigap.bin")
        gen.write_to_file(p_mg)

        # Clean fixture (0 gaps)
        gen_clean = RingBufferGenerator(ring_bytes=64 * 1024)
        gen_clean.generate_scenario("clean")
        p_clean = os.path.join(suite_dir, "clean.bin")
        gen_clean.write_to_file(p_clean)

        # T6.1: Default invocation (no --gap): analyzes latest gap (Gap #3)
        code, out, err = self.run_analyzer([p_mg, "--no-eventlog"])
        self.add_result(
            "T6.1", "Suite 6", "Default gap selection (latest gap)", ["wine", self.analyzer_path, p_mg],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "Analyzing Gap #3 (LATEST GAP) by default",
                "Pre-gap telemetry window displayed",
            ],
            expected_stderr_substrings=[],
        )

        # T6.2: --gap=1: analyzes Gap #1
        code, out, err = self.run_analyzer([p_mg, "--gap=1", "--no-eventlog"])
        self.add_result(
            "T6.2", "Suite 6", "Explicit --gap=1 (first gap)", ["wine", self.analyzer_path, p_mg, "--gap=1"],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Analyzing Gap #1 (selected via --gap=1)..."],
            expected_stderr_substrings=[],
        )

        # T6.3: --gap=2: analyzes Gap #2
        code, out, err = self.run_analyzer([p_mg, "--gap=2", "--no-eventlog"])
        self.add_result(
            "T6.3", "Suite 6", "Explicit --gap=2 (second gap)", ["wine", self.analyzer_path, p_mg, "--gap=2"],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Analyzing Gap #2 (selected via --gap=2)..."],
            expected_stderr_substrings=[],
        )

        # T6.4: --gap=0: graceful fallback to latest gap
        code, out, err = self.run_analyzer([p_mg, "--gap=0", "--no-eventlog"])
        self.add_result(
            "T6.4", "Suite 6", "Graceful fallback for --gap=0", ["wine", self.analyzer_path, p_mg, "--gap=0"],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Analyzing Gap #3 (LATEST GAP) by default"],
            expected_stderr_substrings=[],
        )

        # T6.5: --gap=999: out of bounds, prints notice and analyzes latest gap
        code, out, err = self.run_analyzer([p_mg, "--gap=999", "--no-eventlog"])
        self.add_result(
            "T6.5", "Suite 6", "Graceful handling for out-of-bounds --gap=999", ["wine", self.analyzer_path, p_mg, "--gap=999"],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "Notice: Requested Gap #999 not found",
                "Analyzing latest Gap #3",
            ],
            expected_stderr_substrings=[],
        )

        # T6.6: --gap=-1: negative integer graceful fallback to latest gap
        code, out, err = self.run_analyzer([p_mg, "--gap=-1", "--no-eventlog"])
        self.add_result(
            "T6.6", "Suite 6", "Graceful handling for negative --gap=-1", ["wine", self.analyzer_path, p_mg, "--gap=-1"],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Analyzing Gap #3 (LATEST GAP) by default"],
            expected_stderr_substrings=[],
        )

        # T6.7: Clean ring with --gap=1: prints NO GAP DETECTED
        code, out, err = self.run_analyzer([p_clean, "--gap=1", "--no-eventlog"])
        self.add_result(
            "T6.7", "Suite 6", "Clean ring with --gap=1 flag", ["wine", self.analyzer_path, p_clean, "--gap=1"],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["NO GAP DETECTED (clean tail)"],
            expected_stderr_substrings=[],
        )

        # T6.8: Clean ring with --gap=999: prints NO GAP DETECTED
        code, out, err = self.run_analyzer([p_clean, "--gap=999", "--no-eventlog"])
        self.add_result(
            "T6.8", "Suite 6", "Clean ring with --gap=999 flag", ["wine", self.analyzer_path, p_clean, "--gap=999"],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["NO GAP DETECTED (clean tail)"],
            expected_stderr_substrings=[],
        )

        # T6.9: Missing value for --gap flag
        code, out, err = self.run_analyzer([p_mg, "--gap"])
        self.add_result(
            "T6.9", "Suite 6", "Missing parameter for --gap", ["wine", self.analyzer_path, p_mg, "--gap"],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Error: --gap requires an integer argument"],
        )

    # =========================================================================
    # Suite 7: Auto-Discovery Directory Stress
    # =========================================================================
    def run_suite_7(self):
        print("\n========================================================")
        print(" SUITE 7: Auto-Discovery Directory Stress")
        print("========================================================")
        suite_dir = os.path.join(TMP_TEST_DIR, "suite7")
        os.makedirs(suite_dir, exist_ok=True)

        def make_ring_file(path: str, clean_shutdown: int = 0, scenario: str = "clean"):
            g = RingBufferGenerator(ring_bytes=64 * 1024)
            g.generate_scenario(scenario)
            g.write_to_file(path)
            # Patch clean_shutdown at offset 88
            with open(path, "r+b") as f:
                f.seek(88)
                f.write((1 if clean_shutdown else 0).to_bytes(4, "little"))

        # T7.1: Isolated directory with only ring.bin
        d1 = os.path.join(suite_dir, "dir_ring_only")
        os.makedirs(d1, exist_ok=True)
        make_ring_file(os.path.join(d1, "ring.bin"), clean_shutdown=1)
        code, out, err = self.run_analyzer([d1, "--no-eventlog"])
        self.add_result(
            "T7.1", "Suite 7", "Auto-discover single ring.bin in directory", ["wine", self.analyzer_path, d1],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=["Selected ring file for analysis:", "ring.bin"],
            expected_stderr_substrings=[],
        )

        # T7.2: Directory with multiple crash files (all crashed clean_shutdown=0) + ring.bin
        # Must select the NEWEST crash file (20260913-180000)
        d2 = os.path.join(suite_dir, "dir_multi_crash")
        os.makedirs(d2, exist_ok=True)
        make_ring_file(os.path.join(d2, "ring-crash-20260910-100000.bin"), clean_shutdown=0)
        make_ring_file(os.path.join(d2, "ring-crash-20260912-140000.bin"), clean_shutdown=0)
        make_ring_file(os.path.join(d2, "ring-crash-20260913-180000.bin"), clean_shutdown=0)
        make_ring_file(os.path.join(d2, "ring.bin"), clean_shutdown=1)

        code, out, err = self.run_analyzer([d2, "--no-eventlog"])
        self.add_result(
            "T7.2", "Suite 7", "Auto-discover newest crash ring among multiple candidates",
            ["wine", self.analyzer_path, d2],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "Selected ring file for analysis:",
                "ring-crash-20260913-180000.bin",
            ],
            expected_stderr_substrings=[],
        )

        # T7.3: Directory where newest crash file has clean_shutdown=1, older has clean_shutdown=0
        # Must skip clean crash file and select older crash file ending with gap
        d3 = os.path.join(suite_dir, "dir_skip_clean_crash")
        os.makedirs(d3, exist_ok=True)
        make_ring_file(os.path.join(d3, "ring-crash-20260913-200000.bin"), clean_shutdown=1)  # clean
        make_ring_file(os.path.join(d3, "ring-crash-20260912-150000.bin"), clean_shutdown=0)  # crashed
        make_ring_file(os.path.join(d3, "ring.bin"), clean_shutdown=1)

        code, out, err = self.run_analyzer([d3, "--no-eventlog"])
        self.add_result(
            "T7.3", "Suite 7", "Skip clean crash file and select crash ring with gap",
            ["wine", self.analyzer_path, d3],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "Selected ring file for analysis:",
                "ring-crash-20260912-150000.bin",
            ],
            expected_stderr_substrings=[],
        )

        # T7.4: Directory where all crash files have clean_shutdown=1 -> Fallback to ring.bin
        d4 = os.path.join(suite_dir, "dir_all_clean_crashes")
        os.makedirs(d4, exist_ok=True)
        make_ring_file(os.path.join(d4, "ring-crash-20260913-200000.bin"), clean_shutdown=1)
        make_ring_file(os.path.join(d4, "ring-crash-20260912-150000.bin"), clean_shutdown=1)
        make_ring_file(os.path.join(d4, "ring.bin"), clean_shutdown=1)

        code, out, err = self.run_analyzer([d4, "--no-eventlog"])
        self.add_result(
            "T7.4", "Suite 7", "Fallback to ring.bin when all crash files clean",
            ["wine", self.analyzer_path, d4],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "Selected ring file for analysis:",
                "ring.bin",
            ],
            expected_stderr_substrings=[],
        )

        # T7.5: Explicit --ring=<path> overrides auto-discovery
        code, out, err = self.run_analyzer([
            f"--ring={os.path.join(d2, 'ring-crash-20260910-100000.bin')}",
            "--no-eventlog",
        ])
        self.add_result(
            "T7.5", "Suite 7", "Explicit --ring=<path> overrides auto-discovery",
            ["wine", self.analyzer_path, f"--ring={os.path.join(d2, 'ring-crash-20260910-100000.bin')}"],
            expected_exit=0, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[
                "Selected ring file for analysis:",
                "ring-crash-20260910-100000.bin",
            ],
            expected_stderr_substrings=[],
        )

        # T7.6: Empty directory -> exit 1, clean error
        d_empty = os.path.join(suite_dir, "dir_empty")
        os.makedirs(d_empty, exist_ok=True)
        code, out, err = self.run_analyzer([d_empty, "--no-eventlog"])
        self.add_result(
            "T7.6", "Suite 7", "Empty directory auto-discovery rejection",
            ["wine", self.analyzer_path, d_empty],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Error: Ring file not found at default locations"],
        )

        # T7.7: Non-existent path -> exit 1, clean error
        p_nonexistent = os.path.join(suite_dir, "nonexistent_file.bin")
        code, out, err = self.run_analyzer([p_nonexistent, "--no-eventlog"])
        self.add_result(
            "T7.7", "Suite 7", "Non-existent file path error handling",
            ["wine", self.analyzer_path, p_nonexistent],
            expected_exit=1, actual_exit=code, stdout=out, stderr=err,
            expected_stdout_substrings=[],
            expected_stderr_substrings=["Error: Unable to open ring file"],
        )

    def print_summary(self, json_out: Optional[str] = None) -> bool:
        total = len(self.results)
        passed = sum(1 for r in self.results if r.verdict == "PASS")
        failed = total - passed

        print("\n========================================================")
        print("          M6 ADVERSARIAL STRESS TEST RUN SUMMARY        ")
        print("========================================================")
        print(f" Total Adversarial Tests : {total}")
        print(f" Passed                 : {passed}")
        print(f" Failed                 : {failed}")
        rate = (passed / total) * 100.0 if total > 0 else 0.0
        print(f" Success Rate           : {rate:.1f}%")
        print("========================================================")

        # Print breakdown by suite
        suites = sorted(list(set(r.suite for r in self.results)))
        for s in suites:
            s_tests = [r for r in self.results if r.suite == s]
            s_passed = sum(1 for r in s_tests if r.verdict == "PASS")
            print(f"  {s}: {s_passed}/{len(s_tests)} Passed")

        if json_out:
            report_data = {
                "summary": {
                    "total": total,
                    "passed": passed,
                    "failed": failed,
                    "success_rate": rate,
                    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                },
                "tests": [asdict(r) for r in self.results],
            }
            with open(json_out, "w") as f:
                json.dump(report_data, f, indent=2)
            print(f"\n[+] Detailed JSON report exported to: {json_out}")

        return failed == 0


def main():
    suite = AdversarialTestSuite()
    print("========================================================")
    print(" Blackbox M6 Adversarial & Stress Testing Suite")
    print(f" Target Analyzer: {ANALYZER_BIN}")
    print("========================================================")

    suite.run_suite_1()
    suite.run_suite_2()
    suite.run_suite_3()
    suite.run_suite_4()
    suite.run_suite_5()
    suite.run_suite_6()
    suite.run_suite_7()

    json_path = os.path.join(ROOT_DIR, "tests", "adversarial_m6_report.json")
    success = suite.print_summary(json_out=json_path)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
