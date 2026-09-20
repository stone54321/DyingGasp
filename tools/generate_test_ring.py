#!/usr/bin/env python3
"""
tools/generate_test_ring.py - Deterministic Synthetic Ring Buffer Fixture Generator

Generates bit-exact ring buffer binary files (.bin) matching the Blackbox 4 KB header
and 64-byte record specifications (C11/Win32 little-endian).

Supports:
- Clean continuous rings (clean tail, NO GAP DETECTED)
- Circular wraps (seq exceeding ring slot capacity)
- Single and multiple gaps (--gap=N testing)
- Anomalies: HANG (>3x nominal period), POWER SPIKE (>100W delta),
             PCIE DROP (gen/width decreased), REPLAY JUMP (counter incremented)
- Torn / corrupted boundary records (truncated final record by N bytes)
- Sentinels for missing GPU / non-NVIDIA (0xFF, 0xFFFF, 0xFFFFFFFF)
- Type 1 process transition records (char name[32])
- Type 2 calibration records
- Type 3 heartbeat records
- Telemetry status flags (bit0 nvml, bit1 fg_ok, bit2 timer_late)
"""

import argparse
import math
import os
import struct
import sys
from typing import Dict, List, Optional, Tuple, Union

# Format Constants
BLACKBOX_MAGIC = b"BLKBOX01"  # 0x3130584F424B4C42ULL in Little-Endian
BLACKBOX_VERSION = 1
HEADER_SIZE = 4096
RECORD_SIZE = 64
DEFAULT_RING_BYTES = 64 * 1024 * 1024  # 64 MB
DEFAULT_QPC_FREQ = 10_000_000  # 10 MHz (1 count = 100 ns)
DEFAULT_TSC_FREQ = 3_000_000_000  # 3.0 GHz nominal
DEFAULT_NOMINAL_HZ = 10  # 10 Hz (100 ms period)

# Sentinels
SENTINEL_U8 = 0xFF
SENTINEL_U16 = 0xFFFF
SENTINEL_U32 = 0xFFFFFFFF
SENTINEL_U64 = 0xFFFFFFFFFFFFFFFF

# Record Types
RECORD_TYPE_DATA = 0
RECORD_TYPE_PROCESS = 1
RECORD_TYPE_CALIBRATION = 2
RECORD_TYPE_HEARTBEAT = 3

# Telemetry Flags
FLAG_NVML_AVAILABLE = 1 << 0  # bit 0: NVML loaded and GPU active
FLAG_FG_QUERY_OK = 1 << 1     # bit 1: Foreground process query succeeded
FLAG_TIMER_LATE = 1 << 2      # bit 2: Timer tick arrived > 2x nominal late

# Struct Format Strings (all Little-Endian)
HEADER_STRUCT_FMT = "<8sIIQIIQQQQQIIQ4008s"
TYPE0_DATA_FMT = "<BBBBHHIIIIIQQQ12s"
TYPE1_PROCESS_FMT = "<BBBBQQQI32s"
TYPE2_CAL_FMT = "<B7sQQQQ24s"
TYPE3_HEARTBEAT_FMT = "<BBHIQQQI28s"


def build_header(
    ring_bytes: int,
    nominal_hz: int = DEFAULT_NOMINAL_HZ,
    qpc_freq: int = DEFAULT_QPC_FREQ,
    qpc_start_ticks: int = 100_000_000,
    tsc_start: int = 3_000_000_000,
    filetime_start: int = 133700000000000000,  # ~2024-2026 UTC
    daemon_pid: int = 4242,
    reserved_flags: int = 0,
) -> bytes:
    """Builds an exact 4096-byte little-endian header."""
    record_count = (ring_bytes - HEADER_SIZE) // RECORD_SIZE
    qpc_start_100ns = (qpc_start_ticks * 10_000_000) // qpc_freq

    hdr = struct.pack(
        HEADER_STRUCT_FMT,
        BLACKBOX_MAGIC,
        BLACKBOX_VERSION,
        HEADER_SIZE,
        ring_bytes,
        RECORD_SIZE,
        record_count,
        qpc_freq,
        qpc_start_ticks,
        qpc_start_100ns,
        tsc_start,
        filetime_start,
        nominal_hz,
        daemon_pid,
        reserved_flags,
        b"\x00" * 4008,
    )
    assert len(hdr) == HEADER_SIZE, f"Header must be {HEADER_SIZE} bytes, got {len(hdr)}"
    return hdr


def pack_type0_data(
    seq: int,
    qpc_100ns: int,
    tsc: int,
    pcie_gen: int = 4,
    pcie_width: int = 16,
    cpu_pct: int = 15,
    gpu_temp_c: int = 65,
    flags: int = (FLAG_NVML_AVAILABLE | FLAG_FG_QUERY_OK),
    gpu_power_mw: int = 180_000,
    pcie_replay: int = 0,
    sm_clock_mhz: int = 2100,
    mem_clock_mhz: int = 9500,
    fg_pid: int = 1337,
) -> bytes:
    """Packs a 64-byte Type 0 periodic telemetry data record."""
    rec = struct.pack(
        TYPE0_DATA_FMT,
        RECORD_TYPE_DATA,
        pcie_gen & 0xFF,
        pcie_width & 0xFF,
        cpu_pct & 0xFF,
        gpu_temp_c & 0xFFFF,
        flags & 0xFFFF,
        gpu_power_mw & 0xFFFFFFFF,
        pcie_replay & 0xFFFFFFFF,
        sm_clock_mhz & 0xFFFFFFFF,
        mem_clock_mhz & 0xFFFFFFFF,
        fg_pid & 0xFFFFFFFF,
        seq & 0xFFFFFFFFFFFFFFFF,
        tsc & 0xFFFFFFFFFFFFFFFF,
        qpc_100ns & 0xFFFFFFFFFFFFFFFF,
        b"\x00" * 12,
    )
    assert len(rec) == RECORD_SIZE, f"Record must be {RECORD_SIZE} bytes, got {len(rec)}"
    return rec


def pack_type1_process(
    seq: int,
    qpc_100ns: int,
    tsc: int,
    fg_pid: int,
    name: str,
    flags: int = (FLAG_NVML_AVAILABLE | FLAG_FG_QUERY_OK),
) -> bytes:
    """Packs a 64-byte Type 1 process transition record."""
    name_bytes = name.encode("utf-8")[:31]  # truncated to max 31 chars + NUL
    name_len = len(name_bytes)
    name_buf = name_bytes.ljust(32, b"\x00")

    rec = struct.pack(
        TYPE1_PROCESS_FMT,
        RECORD_TYPE_PROCESS,
        flags & 0xFF,
        name_len & 0xFF,
        0,  # pad0
        seq & 0xFFFFFFFFFFFFFFFF,
        tsc & 0xFFFFFFFFFFFFFFFF,
        qpc_100ns & 0xFFFFFFFFFFFFFFFF,
        fg_pid & 0xFFFFFFFF,
        name_buf,
    )
    assert len(rec) == RECORD_SIZE, f"Record must be {RECORD_SIZE} bytes, got {len(rec)}"
    return rec


def pack_type2_cal(
    seq: int,
    qpc_100ns: int,
    tsc: int,
    filetime: int,
) -> bytes:
    """Packs a 64-byte Type 2 clock calibration record."""
    rec = struct.pack(
        TYPE2_CAL_FMT,
        RECORD_TYPE_CALIBRATION,
        b"\x00" * 7,
        seq & 0xFFFFFFFFFFFFFFFF,
        tsc & 0xFFFFFFFFFFFFFFFF,
        qpc_100ns & 0xFFFFFFFFFFFFFFFF,
        filetime & 0xFFFFFFFFFFFFFFFF,
        b"\x00" * 24,
    )
    assert len(rec) == RECORD_SIZE, f"Record must be {RECORD_SIZE} bytes, got {len(rec)}"
    return rec


def pack_type3_heartbeat(
    seq: int,
    qpc_100ns: int,
    tsc: int,
    uptime_sec: int,
    cpu_pct: int = 5,
    fg_pid: int = 1337,
    flags: int = (FLAG_NVML_AVAILABLE | FLAG_FG_QUERY_OK),
) -> bytes:
    """Packs a 64-byte Type 3 daemon heartbeat record."""
    rec = struct.pack(
        TYPE3_HEARTBEAT_FMT,
        RECORD_TYPE_HEARTBEAT,
        cpu_pct & 0xFF,
        flags & 0xFFFF,
        fg_pid & 0xFFFFFFFF,
        seq & 0xFFFFFFFFFFFFFFFF,
        tsc & 0xFFFFFFFFFFFFFFFF,
        qpc_100ns & 0xFFFFFFFFFFFFFFFF,
        uptime_sec & 0xFFFFFFFF,
        b"\x00" * 28,
    )
    assert len(rec) == RECORD_SIZE, f"Record must be {RECORD_SIZE} bytes, got {len(rec)}"
    return rec


class RingBufferGenerator:
    """Builds synthetic ring buffers in memory and writes them to disk."""

    def __init__(
        self,
        ring_bytes: int = 64 * 1024,  # Default 64 KB for fast tests (960 records)
        nominal_hz: int = DEFAULT_NOMINAL_HZ,
        qpc_freq: int = DEFAULT_QPC_FREQ,
        base_filetime: int = 133700000000000000,
    ):
        self.ring_bytes = ring_bytes
        self.nominal_hz = nominal_hz
        self.qpc_freq = qpc_freq
        self.base_filetime = base_filetime
        self.record_count = (ring_bytes - HEADER_SIZE) // RECORD_SIZE

        self.step_100ns = 10_000_000 // nominal_hz
        self.step_tsc = (DEFAULT_TSC_FREQ // nominal_hz)

        self.qpc_start = 100_000_000
        self.tsc_start = 3_000_000_000

        # In-memory storage: slot_index -> packed 64-byte record
        self.slots: Dict[int, bytes] = {}

    def get_slot_for_seq(self, seq: int) -> int:
        return seq % self.record_count

    def put_record(self, seq: int, record_bytes: bytes):
        assert len(record_bytes) == RECORD_SIZE
        slot = self.get_slot_for_seq(seq)
        self.slots[slot] = record_bytes

    def generate_scenario(
        self,
        scenario: str,
        total_records: Optional[int] = None,
        custom_params: Optional[dict] = None,
    ):
        """Generates a predefined scenario with deterministic anomaly injection."""
        params = custom_params or {}

        if total_records is None:
            if scenario in ("wrap", "realworld_gaming", "realworld_thermal"):
                total_records = self.record_count + (self.record_count // 2)  # 1.5x wrap
            else:
                total_records = min(self.record_count, 300)

        curr_qpc = self.qpc_start
        curr_tsc = self.tsc_start
        curr_seq = 0

        # Baseline telemetry metrics
        power_mw = 160_000  # 160 W
        temp_c = 62
        gen = 4
        width = 16
        replay = 0
        cpu_pct = 20
        fg_pid = 4000
        fg_name = "game.exe"
        flags = FLAG_NVML_AVAILABLE | FLAG_FG_QUERY_OK

        is_no_gpu = (scenario == "nogpu") or params.get("no_gpu", False)
        if is_no_gpu:
            flags &= ~FLAG_NVML_AVAILABLE
            power_mw = SENTINEL_U32
            temp_c = SENTINEL_U16
            gen = SENTINEL_U8
            width = SENTINEL_U8
            replay = SENTINEL_U32

        rec_idx = 0
        # Optional initial process transition
        if params.get("with_proc_change", True) and not is_no_gpu and (total_records is None or total_records > 0):
            rec = pack_type1_process(
                seq=curr_seq,
                qpc_100ns=curr_qpc,
                tsc=curr_tsc,
                fg_pid=fg_pid,
                name=fg_name,
                flags=flags,
            )
            self.put_record(curr_seq, rec)
            curr_seq += 1
            curr_qpc += self.step_100ns
            curr_tsc += self.step_tsc
            rec_idx += 1

        # Gap definition lists for multigap
        # Format: list of (at_seq, records_lost, time_jump_multiplier)
        gaps_to_inject = []
        if scenario == "gap":
            gaps_to_inject.append((50, 20, 10))  # at seq 50, lost 20 records, 10x dt
        elif scenario == "multigap":
            gaps_to_inject.append((40, 15, 5))
            gaps_to_inject.append((120, 30, 20))
            if total_records is None or total_records > 250:
                gaps_to_inject.append((220, 10, 8))

        # Main generation loop
        while rec_idx < total_records:
            # Check for gap injection
            injected_gap = False
            for gap_at, lost_cnt, dt_mult in gaps_to_inject:
                if rec_idx == gap_at:
                    curr_seq += lost_cnt  # jump sequence
                    curr_qpc += self.step_100ns * dt_mult
                    curr_tsc += self.step_tsc * dt_mult
                    injected_gap = True
                    break

            # Local modifications based on scenario and rec_idx
            rec_power = power_mw
            rec_temp = temp_c
            rec_gen = gen
            rec_width = width
            rec_replay = replay
            rec_cpu = cpu_pct
            rec_flags = flags
            rec_pid = fg_pid

            # Scenario 1: HANG (dt > 3x nominal period)
            if scenario in ("hang", "all_anomalies") and rec_idx == 45:
                # 400 ms jump at 10 Hz (4x period > 3x)
                curr_qpc += self.step_100ns * 4
                curr_tsc += self.step_tsc * 4

            # Scenario 2: POWER SPIKE (GPU power delta > 100 W = 100,000 mW)
            if scenario in ("powerspike", "all_anomalies") and rec_idx == 60 and not is_no_gpu:
                rec_power = power_mw + 125_000  # +125 W jump

            # Scenario 3: PCIE DROP (gen or width decreased)
            if scenario in ("pciedrop", "all_anomalies") and rec_idx == 75 and not is_no_gpu:
                rec_gen = 3  # Gen 4 -> Gen 3
                rec_width = 8  # x16 -> x8

            # Scenario 4: REPLAY JUMP (counter incremented)
            if scenario in ("replayjump", "all_anomalies") and rec_idx == 90 and not is_no_gpu:
                rec_replay = replay + 77  # +77 replays

            # Scenario: Timer Late Flag
            if params.get("timer_late_at") == rec_idx or (scenario == "timer_late" and rec_idx == 55):
                rec_flags |= FLAG_TIMER_LATE
                curr_qpc += self.step_100ns * 3  # Late tick arrived

            # Periodic service records
            if params.get("with_calib") and rec_idx == 100:
                rec = pack_type2_cal(
                    seq=curr_seq,
                    qpc_100ns=curr_qpc,
                    tsc=curr_tsc,
                    filetime=self.base_filetime + (curr_qpc - self.qpc_start),
                )
                self.put_record(curr_seq, rec)
                curr_seq += 1
                curr_qpc += self.step_100ns
                curr_tsc += self.step_tsc
                rec_idx += 1
                continue

            if params.get("with_heartbeat") and (rec_idx % 100 == 0) and rec_idx > 0:
                uptime = rec_idx // self.nominal_hz
                rec = pack_type3_heartbeat(
                    seq=curr_seq,
                    qpc_100ns=curr_qpc,
                    tsc=curr_tsc,
                    uptime_sec=uptime,
                    cpu_pct=rec_cpu,
                    fg_pid=rec_pid,
                    flags=rec_flags,
                )
                self.put_record(curr_seq, rec)
                curr_seq += 1
                curr_qpc += self.step_100ns
                curr_tsc += self.step_tsc
                rec_idx += 1
                continue

            # Real-world scenario 3: Gaming workload with process switching
            if scenario == "realworld_gaming":
                if rec_idx == 40:
                    rec = pack_type1_process(curr_seq, curr_qpc, curr_tsc, 5510, "Discord.exe", rec_flags)
                    self.put_record(curr_seq, rec)
                    curr_seq += 1
                    rec_idx += 1
                    fg_pid = 5510
                    continue
                elif rec_idx == 80:
                    rec = pack_type1_process(curr_seq, curr_qpc, curr_tsc, 8412, "Cyberpunk2077.exe", rec_flags)
                    self.put_record(curr_seq, rec)
                    curr_seq += 1
                    rec_idx += 1
                    fg_pid = 8412
                    continue
                elif rec_idx > 80:
                    rec_power = 240_000 + (rec_idx % 30) * 1000
                    rec_temp = 72 + (rec_idx % 5)
                    rec_cpu = 45

            # Real-world scenario 4: GPU thermal cascade
            if scenario == "realworld_thermal" and not is_no_gpu:
                rec_temp = min(105, 65 + (rec_idx // 10))
                if rec_temp >= 90:
                    rec_power = max(100_000, 250_000 - (rec_idx * 500))  # throttling
                if rec_temp >= 98 and rec_idx > 80:
                    rec_gen = 2
                    rec_replay += 15

            # Pack standard type 0 record
            rec = pack_type0_data(
                seq=curr_seq,
                qpc_100ns=curr_qpc,
                tsc=curr_tsc,
                pcie_gen=rec_gen,
                pcie_width=rec_width,
                cpu_pct=rec_cpu,
                gpu_temp_c=rec_temp,
                flags=rec_flags,
                gpu_power_mw=rec_power,
                pcie_replay=rec_replay,
                sm_clock_mhz=2100 if not is_no_gpu else SENTINEL_U32,
                mem_clock_mhz=9500 if not is_no_gpu else SENTINEL_U32,
                fg_pid=rec_pid,
            )
            self.put_record(curr_seq, rec)

            curr_seq += 1
            curr_qpc += self.step_100ns
            curr_tsc += self.step_tsc
            rec_idx += 1

        self.last_seq = curr_seq - 1
        return self.last_seq

    def write_to_file(self, output_path: str, truncate_bytes: int = 0):
        """Assembles 4 KB header and all slots, then writes to output_path."""
        os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
        header = build_header(
            ring_bytes=self.ring_bytes,
            nominal_hz=self.nominal_hz,
            qpc_freq=self.qpc_freq,
        )

        data_region = bytearray(self.record_count * RECORD_SIZE)
        for slot_idx, rec_data in self.slots.items():
            if 0 <= slot_idx < self.record_count:
                offset = slot_idx * RECORD_SIZE
                data_region[offset : offset + RECORD_SIZE] = rec_data

        full_image = header + data_region

        if truncate_bytes > 0:
            # Physical truncation at file boundary
            full_image = full_image[:-truncate_bytes]

        with open(output_path, "wb") as f:
            f.write(full_image)

        return len(full_image)


def generate_ring_file(
    output_path: str,
    scenario: str = "clean",
    ring_kb: int = 64,
    records: Optional[int] = None,
    nominal_hz: int = 10,
    torn_bytes: int = 0,
    with_proc_change: bool = True,
    with_calib: bool = False,
    with_heartbeat: bool = False,
    no_gpu: bool = False,
    custom_params: Optional[dict] = None,
) -> Tuple[int, int]:
    """Top-level helper to generate a synthetic ring file."""
    ring_bytes = ring_kb * 1024
    gen = RingBufferGenerator(ring_bytes=ring_bytes, nominal_hz=nominal_hz)

    params = custom_params or {}
    params.update(
        {
            "with_proc_change": with_proc_change,
            "with_calib": with_calib,
            "with_heartbeat": with_heartbeat,
            "no_gpu": no_gpu,
        }
    )

    if scenario == "torn" and torn_bytes == 0:
        torn_bytes = 30

    last_seq = gen.generate_scenario(scenario=scenario, total_records=records, custom_params=params)
    written_size = gen.write_to_file(output_path, truncate_bytes=torn_bytes)
    return last_seq, written_size


def parse_args():
    parser = argparse.ArgumentParser(
        description="Deterministic Synthetic Ring Buffer Generator for Blackbox Telemetry"
    )
    parser.add_argument(
        "--output",
        "-o",
        default="ring.bin",
        help="Output binary file path (default: ring.bin)",
    )
    parser.add_argument(
        "--scenario",
        "-s",
        choices=[
            "clean",
            "wrap",
            "gap",
            "multigap",
            "hang",
            "powerspike",
            "pciedrop",
            "replayjump",
            "torn",
            "nogpu",
            "timer_late",
            "all_anomalies",
            "realworld_gaming",
            "realworld_thermal",
        ],
        default="clean",
        help="Predefined test scenario to generate (default: clean)",
    )
    parser.add_argument(
        "--ring-kb",
        type=int,
        default=64,
        help="Total ring file size in KB (default: 64 KB = 960 record slots)",
    )
    parser.add_argument(
        "--ring-mb",
        type=int,
        default=0,
        help="Total ring file size in MB (overrides --ring-kb if > 0)",
    )
    parser.add_argument(
        "--records",
        "-n",
        type=int,
        default=None,
        help="Number of records to generate (default: derived from scenario and capacity)",
    )
    parser.add_argument(
        "--hz",
        type=int,
        default=DEFAULT_NOMINAL_HZ,
        help="Nominal sampling rate in Hz (default: 10)",
    )
    parser.add_argument(
        "--torn-bytes",
        type=int,
        default=0,
        help="Bytes to truncate off the final record/file (e.g. 30)",
    )
    parser.add_argument(
        "--with-calib",
        action="store_true",
        help="Inject Type 2 clock calibration record",
    )
    parser.add_argument(
        "--with-heartbeat",
        action="store_true",
        help="Inject Type 3 daemon heartbeat record",
    )
    parser.add_argument(
        "--no-proc-change",
        action="store_true",
        help="Disable automatic Type 1 process transition records",
    )
    parser.add_argument(
        "--no-gpu",
        action="store_true",
        help="Simulate absent GPU / sentinel logging",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    ring_kb = args.ring_kb
    if args.ring_mb > 0:
        ring_kb = args.ring_mb * 1024

    torn = args.torn_bytes
    if args.scenario == "torn" and torn == 0:
        torn = 30

    last_seq, written_size = generate_ring_file(
        output_path=args.output,
        scenario=args.scenario,
        ring_kb=ring_kb,
        records=args.records,
        nominal_hz=args.hz,
        torn_bytes=torn,
        with_proc_change=not args.no_proc_change,
        with_calib=args.with_calib,
        with_heartbeat=args.with_heartbeat,
        no_gpu=args.no_gpu,
    )

    print(
        f"[+] Generated '{args.scenario}' ring fixture -> {args.output} "
        f"({written_size} bytes, last_seq={last_seq}, ring_kb={ring_kb})"
    )


if __name__ == "__main__":
    main()
