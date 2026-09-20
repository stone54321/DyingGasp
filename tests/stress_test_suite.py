#!/usr/bin/env python3
"""
===============================================================================
Blackbox Telemetry: Adversarial Stress & Hardening Test Suite
===============================================================================
Target: Telemetry Collector Daemon (blackbox.exe) and Storage Subsystem

Covers:
  1. Mutex Conflict (Simultaneous launch, code 1 exit, mutex lifecycle)
  2. Rapid Kill & Restart Cycles (100ms, 500ms, 2s; rotation & max 3 pruning)
  3. High Sampling Frequency Stress (--hz=100 under Wine, wrap & continuity)
  4. Malformed CLI Arguments (--hz=0, --hz=500, --ring-size-mb=0, overflows)
  5. Permission & Path Failure Simulation (Read-only dir/file, invalid path)
  6. CPU Utilization Audit (< 0.5% single-core continuous 10 Hz)
===============================================================================
"""

import os
import sys
import time
import glob
import stat
import shutil
import struct
import subprocess
from typing import List, Dict, Any, Tuple

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO_ROOT)

from tests.e2e_runner import ReferenceRingOracle

COLLECTOR_EXE = os.path.join(REPO_ROOT, "bin", "blackbox.exe")
ANALYZER_EXE = os.path.join(REPO_ROOT, "bin", "blackbox-analyze.exe")
WINE_CMD = os.environ.get("WINE_CMD", "wine")

TEST_TEMP_ROOT = "/tmp/blackbox_m6_stress"

class StressTestRunner:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.results: List[Dict[str, Any]] = []

    def log(self, msg: str):
        print(f"[*] {msg}", flush=True)

    def record_result(self, name: str, passed: bool, details: str = ""):
        if passed:
            self.passed += 1
            print(f"[PASS] {name} {details}", flush=True)
        else:
            self.failed += 1
            print(f"[FAIL] {name} {details}", flush=True)
        self.results.append({"name": name, "passed": passed, "details": details})

    def cleanup_wine(self):
        # Brief pause to let wineserver release named mutexes
        time.sleep(1.2)

    def wait_for_ring_file(self, ring_path: str, expected_size: int, timeout_sec: float = 5.0) -> bool:
        t0 = time.time()
        while time.time() - t0 < timeout_sec:
            if os.path.exists(ring_path) and os.path.getsize(ring_path) == expected_size:
                return True
            time.sleep(0.02)
        return False

    # -------------------------------------------------------------------------
    # Scenario 1: Mutex Conflict
    # -------------------------------------------------------------------------
    def test_mutex_conflict(self):
        print("\n========================================================")
        print(" SCENARIO 1: Mutex Conflict & Single-Instance Enforcement")
        print("========================================================")
        test_ring = os.path.join(TEST_TEMP_ROOT, "mutex_ring.bin")
        if os.path.exists(test_ring):
            os.remove(test_ring)

        # 1.1 Sequential conflict check: launch P1, verify P2 rejected with code 1
        p1 = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={test_ring}", "--ring-kb=64", "--hz=10"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        ready = self.wait_for_ring_file(test_ring, 65536)
        self.record_result("MUTEX_P1_INIT", ready, "Primary instance initialized ring.bin")

        p2 = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={test_ring}", "--ring-kb=64", "--hz=10"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        p2_out, p2_err = p2.communicate(timeout=5)
        p2_code = p2.returncode
        err_msg = p2_err.decode("utf-8", errors="replace").strip()

        is_conflict_rejected = (p2_code == 1 and "already running" in err_msg)
        self.record_result("MUTEX_P2_REJECTED", is_conflict_rejected,
                           f"(Code={p2_code}, Stderr='{err_msg}')")

        # Verify P1 is still active
        is_p1_alive = (p1.poll() is None)
        self.record_result("MUTEX_P1_STILL_ALIVE", is_p1_alive, "P1 continues sampling undisturbed")

        p1.terminate()
        p1.wait(timeout=5)
        self.cleanup_wine()

        # 1.2 Simultaneous launch race condition
        if os.path.exists(test_ring):
            os.remove(test_ring)
        pa = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={test_ring}", "--ring-kb=64", "--hz=10"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        pb = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={test_ring}", "--ring-kb=64", "--hz=10"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        time.sleep(1.5)
        codes = [pa.poll(), pb.poll()]
        simul_ok = (None in codes and 1 in codes)
        self.record_result("MUTEX_SIMULTANEOUS_RACE", simul_ok,
                           f"Exactly one winner (None) and one reject (1): {codes}")

        if pa.poll() is None:
            pa.terminate(); pa.wait()
        if pb.poll() is None:
            pb.terminate(); pb.wait()
        self.cleanup_wine()

    # -------------------------------------------------------------------------
    # Scenario 2: Multiple Rapid Kill & Restart Cycles
    # -------------------------------------------------------------------------
    def test_rapid_kill_and_restart(self):
        print("\n========================================================")
        print(" SCENARIO 2: Rapid Kill & Restart Cycles + Pruning")
        print("========================================================")
        kill_dir = os.path.join(TEST_TEMP_ROOT, "rapid_kill")
        if os.path.exists(kill_dir):
            shutil.rmtree(kill_dir)
        os.makedirs(kill_dir, exist_ok=True)
        ring_path = os.path.join(kill_dir, "ring.bin")

        intervals = [0.10, 0.50, 2.00, 0.10, 0.50, 2.00]
        records_per_run: List[int] = []

        for run_idx, interval in enumerate(intervals):
            self.log(f"Run {run_idx + 1}/6: Launching collector, killing abruptly after {interval}s active...")
            p = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={ring_path}", "--ring-kb=64", "--hz=20"],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.wait_for_ring_file(ring_path, 65536)
            time.sleep(interval)
            p.kill()
            p.wait()

            # Inspect pre-restart records in ring.bin
            oracle = ReferenceRingOracle(ring_path)
            rec_cnt = len(oracle.valid_records)
            records_per_run.append(rec_cnt)
            self.log(f"  Captured {rec_cnt} pre-crash records in ring.bin (clean_shutdown = {oracle.header.get('clean_shutdown', 0)})")

            # Pause to ensure unique second timestamp for next rotation
            time.sleep(1.1)

        # Trigger final restart to rotate the 6th run
        self.log("Triggering final launch to rotate 6th crash session...")
        p_final = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={ring_path}", "--ring-kb=64", "--hz=20"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.wait_for_ring_file(ring_path, 65536)
        time.sleep(0.5)
        p_final.terminate()
        p_final.wait()
        self.cleanup_wine()

        crash_files = sorted(glob.glob(os.path.join(kill_dir, "ring-crash-*.bin")))
        self.log(f"Rotated crash files found ({len(crash_files)}): {[os.path.basename(f) for f in crash_files]}")

        # Assert max 3 newest files retained
        has_max_3 = (len(crash_files) == 3)
        self.record_result("CRASH_PRUNING_MAX_3", has_max_3, f"Retained exactly 3 crash files out of 6 runs")

        # Verify all 3 retained crash files contain intact pre-crash records with sequence continuity
        all_continuous = True
        for cf in crash_files:
            co = ReferenceRingOracle(cf)
            if not co.is_valid_header or len(co.valid_records) == 0:
                all_continuous = False
                break
            seqs = [r["seq"] for r in co.valid_records]
            for i in range(1, len(seqs)):
                if seqs[i] != seqs[i-1] + 1:
                    all_continuous = False
                    break
        self.record_result("CRASH_RECORD_PRESERVATION", all_continuous,
                           "All crash files have valid headers and contiguous sequences")

        # Verify new active ring.bin is clean and initialized
        active_oracle = ReferenceRingOracle(ring_path)
        is_fresh = (active_oracle.is_valid_header and os.path.getsize(ring_path) == 65536)
        self.record_result("CRASH_FRESH_ACTIVE_RING", is_fresh, "New ring.bin is clean 64 KB file")

    # -------------------------------------------------------------------------
    # Scenario 3: High Sampling Frequency Stress (--hz=100)
    # -------------------------------------------------------------------------
    def test_high_frequency_stress(self):
        print("\n========================================================")
        print(" SCENARIO 3: High Sampling Frequency Stress (--hz=100)")
        print("========================================================")
        hz_ring = os.path.join(TEST_TEMP_ROOT, "hz100_ring.bin")
        if os.path.exists(hz_ring):
            os.remove(hz_ring)

        self.log("Launching collector at --hz=100 on 64 KB ring (960 capacity)...")
        p = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={hz_ring}", "--ring-kb=64", "--hz=100"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.wait_for_ring_file(hz_ring, 65536)

        # Run for 14 seconds: at 100 Hz, ~1400 records will be written, ensuring >=1 full circular wrap
        self.log("Sampling for 14 seconds to trigger circular buffer wrap...")
        time.sleep(14.0)

        p.terminate()
        p.wait(timeout=5)
        self.cleanup_wine()

        oracle = ReferenceRingOracle(hz_ring)
        self.record_result("HZ100_HEADER_VALID", oracle.is_valid_header, f"(Nominal Hz = {oracle.header.get('nominal_hz')})")
        self.record_result("HZ100_BUFFER_WRAP", oracle.wrap_count >= 1, f"(Wrap count = {oracle.wrap_count})")
        self.record_result("HZ100_FULL_CAPACITY", len(oracle.valid_records) == 960,
                           f"(Valid records = {len(oracle.valid_records)} / 960)")
        self.record_result("HZ100_ZERO_TORN", not oracle.has_torn, "Zero torn boundary records")

        # Verify analyzer execution
        res = subprocess.run([WINE_CMD, ANALYZER_EXE, hz_ring, "--no-eventlog"],
                             capture_output=True, text=True)
        analyzer_ok = (res.returncode == 0 and "Total Valid Records   : 960" in res.stdout and "Buffer Wrap Count     : 1" in res.stdout)
        self.record_result("HZ100_ANALYZER_CLEAN", analyzer_ok,
                           f"(Analyzer return code: {res.returncode})")

    # -------------------------------------------------------------------------
    # Scenario 4: Malformed CLI Arguments
    # -------------------------------------------------------------------------
    def test_malformed_cli_arguments(self):
        print("\n========================================================")
        print(" SCENARIO 4: Malformed CLI Arguments Resilience")
        print("========================================================")
        test_cases = [
            ("--hz=0", "Clamp/default to 10 Hz"),
            ("--hz=500", "Reject/clamp to 10 Hz"),
            ("--ring-size-mb=0", "Default to 64 MB"),
            ("--ring-kb=0", "Clamp to minimum ring size (4224 B)"),
            ("--hz=-10", "Negative Hz safe handling"),
            ("--ring-size-mb=-1", "Negative size safe handling"),
            ("--hz=invalid_string", "Non-numeric parameter handling"),
            ("--bogus-unrecognized-flag", "Unknown flag resilience"),
            ("--hz=999999999999999999999999999999", "Integer overflow resilience"),
            ("--ring-size-mb=999999999999999999999999999999", "Size overflow resilience"),
        ]

        for flag, desc in test_cases:
            ring_path = os.path.join(TEST_TEMP_ROOT, f"cli_test_{abs(hash(flag))}.bin")
            if os.path.exists(ring_path):
                os.remove(ring_path)

            cmd = [WINE_CMD, COLLECTOR_EXE, f"--path={ring_path}"]
            if "--ring" not in flag:
                cmd.append("--ring-kb=64")
            cmd.append(flag)

            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(0.8)
            poll = p.poll()

            no_crash = False
            if poll is None:
                # Daemon running cleanly with clamped/defaulted parameter
                p.terminate()
                p.wait(timeout=5)
                no_crash = True
            else:
                out, err = p.communicate()
                # Exited with clean returncode without crashing
                no_crash = (poll >= 0)

            if os.path.exists(ring_path):
                os.remove(ring_path)
            self.cleanup_wine()

            self.record_result(f"CLI_RESILIENCE_{flag.split('=')[0].replace('-', '_')}",
                               no_crash, f"Flag: '{flag}' -> {desc}")

    # -------------------------------------------------------------------------
    # Scenario 5: Permission & Path Failure Simulation
    # -------------------------------------------------------------------------
    def test_permission_and_path_failures(self):
        print("\n========================================================")
        print(" SCENARIO 5: Permission & Path Failure Simulation")
        print("========================================================")

        # 5.1 Read-Only Directory
        ro_dir = os.path.join(TEST_TEMP_ROOT, "ro_directory")
        if os.path.exists(ro_dir):
            os.chmod(ro_dir, stat.S_IRWXU)
            shutil.rmtree(ro_dir)
        os.makedirs(ro_dir, exist_ok=True)
        os.chmod(ro_dir, stat.S_IRUSR | stat.S_IXUSR) # r-x

        p_ro_dir = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={ro_dir}/ring.bin"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out1, err1 = p_ro_dir.communicate(timeout=5)
        code1 = p_ro_dir.returncode
        err1_str = err1.decode("utf-8", errors="replace")
        ro_dir_ok = (code1 == 1 and "Failed to pre-allocate" in err1_str)
        self.record_result("PATH_FAIL_RO_DIR", ro_dir_ok,
                           f"(Code={code1}, Error detected properly)")

        os.chmod(ro_dir, stat.S_IRWXU)
        shutil.rmtree(ro_dir)
        self.cleanup_wine()

        # 5.2 Read-Only Existing File
        ro_file = os.path.join(TEST_TEMP_ROOT, "ro_file.bin")
        if os.path.exists(ro_file):
            os.chmod(ro_file, stat.S_IRWXU)
            os.remove(ro_file)
        with open(ro_file, "wb") as f:
            f.write(b"\x00" * 65536)
        os.chmod(ro_file, stat.S_IRUSR) # r--

        p_ro_file = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={ro_file}"],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out2, err2 = p_ro_file.communicate(timeout=5)
        code2 = p_ro_file.returncode
        err2_str = err2.decode("utf-8", errors="replace")
        ro_file_ok = (code2 == 1 and "Failed to pre-allocate" in err2_str)
        self.record_result("PATH_FAIL_RO_FILE", ro_file_ok,
                           f"(Code={code2}, Access violation handled cleanly)")

        os.chmod(ro_file, stat.S_IRWXU)
        os.remove(ro_file)
        self.cleanup_wine()

        # 5.3 Invalid Drive / Nonexistent Path
        p_inv = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, "--path=X:\\invalid\\drive\\ring.bin"],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out3, err3 = p_inv.communicate(timeout=5)
        code3 = p_inv.returncode
        err3_str = err3.decode("utf-8", errors="replace")
        inv_ok = (code3 == 1 and "Failed to pre-allocate" in err3_str)
        self.record_result("PATH_FAIL_INVALID_DRIVE", inv_ok,
                           f"(Code={code3}, Invalid drive handled cleanly)")
        self.cleanup_wine()

        # 5.4 Mutex Cleanup Verification after Path Failure
        valid_path = os.path.join(TEST_TEMP_ROOT, "valid_after_failure.bin")
        if os.path.exists(valid_path):
            os.remove(valid_path)
        p_mut = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={valid_path}", "--ring-kb=64"],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(1.2)
        mut_clean = (p_mut.poll() is None)
        self.record_result("PATH_FAIL_MUTEX_RELEASED", mut_clean,
                           "Mutex was cleanly closed and reusable after errors")
        if mut_clean:
            p_mut.terminate()
            p_mut.wait()
        if os.path.exists(valid_path):
            os.remove(valid_path)
        self.cleanup_wine()

    # -------------------------------------------------------------------------
    # Scenario 6: CPU Utilization Audit (< 0.5% single-core)
    # -------------------------------------------------------------------------
    def test_cpu_utilization_audit(self):
        print("\n========================================================")
        print(" SCENARIO 6: CPU Utilization Audit (< 0.5% single-core)")
        print("========================================================")
        cpu_ring = os.path.join(TEST_TEMP_ROOT, "cpu_audit.bin")
        if os.path.exists(cpu_ring):
            os.remove(cpu_ring)

        clk_tck = os.sysconf(os.sysconf_names["SC_CLK_TCK"])

        p = subprocess.Popen([WINE_CMD, COLLECTOR_EXE, f"--path={cpu_ring}", "--ring-kb=64", "--hz=10"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.wait_for_ring_file(cpu_ring, 65536)

        # Discover processes
        time.sleep(1.5)
        bb_pids = []
        for entry in os.listdir("/proc"):
            if entry.isdigit():
                try:
                    with open(f"/proc/{entry}/cmdline", "rb") as f:
                        cmd = f.read().decode("utf-8", errors="replace")
                        if "blackbox.exe" in cmd:
                            bb_pids.append(int(entry))
                except Exception:
                    pass

        self.log(f"Discovered Collector PIDs: {bb_pids}")

        def get_ticks(pid: int) -> int:
            try:
                with open(f"/proc/{pid}/stat", "r") as f:
                    fields = f.read().split()
                    return int(fields[13]) + int(fields[14])
            except Exception:
                return 0

        t_start = time.time()
        start_ticks = {pid: get_ticks(pid) for pid in bb_pids}

        audit_duration = 12.0
        time.sleep(audit_duration)

        t_end = time.time()
        end_ticks = {pid: get_ticks(pid) for pid in bb_pids}

        p.terminate()
        p.wait(timeout=5)
        self.cleanup_wine()

        wall_time = t_end - t_start
        total_cpu_sec = sum((end_ticks[pid] - start_ticks[pid]) / clk_tck for pid in bb_pids)
        cpu_pct = (total_cpu_sec / wall_time) * 100.0

        self.log(f"Measured {cpu_pct:.4f}% single-core CPU over {wall_time:.2f}s")
        cpu_ok = (cpu_pct < 0.5)
        self.record_result("CPU_UTILIZATION_AUDIT", cpu_ok,
                           f"(Measured: {cpu_pct:.4f}% < Threshold: 0.5000%)")

        if os.path.exists(cpu_ring):
            os.remove(cpu_ring)

    # -------------------------------------------------------------------------
    # Main Suite Runner
    # -------------------------------------------------------------------------
    def run_all(self) -> bool:
        os.makedirs(TEST_TEMP_ROOT, exist_ok=True)
        print("========================================================")
        print(" BLACKBOX TELEMETRY ADVERSARIAL STRESS TEST SUITE (M6)")
        print("========================================================")

        self.test_mutex_conflict()
        self.test_rapid_kill_and_restart()
        self.test_high_frequency_stress()
        self.test_malformed_cli_arguments()
        self.test_permission_and_path_failures()
        self.test_cpu_utilization_audit()

        print("\n========================================================")
        print("                  STRESS TEST SUMMARY                   ")
        print("========================================================")
        print(f" Total Tests Run : {self.passed + self.failed}")
        print(f" Passed          : {self.passed}")
        print(f" Failed          : {self.failed}")
        success_rate = (self.passed / (self.passed + self.failed)) * 100.0 if (self.passed + self.failed) > 0 else 0
        print(f" Success Rate    : {success_rate:.1f}%")
        print("========================================================")

        return (self.failed == 0)

if __name__ == "__main__":
    runner = StressTestRunner()
    success = runner.run_all()
    sys.exit(0 if success else 1)
