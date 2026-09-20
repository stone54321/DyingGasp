# Blackbox: High-Durability Pre-Crash Telemetry Ring-Logger & Crash Analyzer

Blackbox is a pure C11 / Win32 user-mode telemetry ring-logger daemon (`blackbox.exe`) and post-mortem crash analyzer (`blackbox-analyze.exe`) engineered for Windows 10/11 x64. It diagnoses silent PC reboots, kernel panics, transient power cuts, GPU throttling, and PCIe degradation without requiring kernel drivers.

---

## 1. Architectural Overview & Data Flow

Blackbox is architected around the principle that kernel crashes, PSU over-current trips (OCP), and instant power cuts invalidate user-mode buffers and filesystem caches. To achieve forensic persistence, Blackbox employs write-through pre-allocated circular disk storage with zero memory allocations in the hot path.

```
+-----------------------------------------------------------------------------------------+
|                                    WINDOWS 10 / 11 TARGET                               |
|                                                                                         |
|   +-----------------------+                         +-------------------------------+   |
|   |   NVIDIA GPU Driver   |                         |  Active Desktop Window (HWND) |   |
|   |       nvml.dll        |                         |     GetForegroundWindow       |   |
|   +-----------+-----------+                         +---------------+---------------+   |
|               | Dynamic Load                                        | Polled on tick    |
|               v                                                     v                   |
|   +---------------------------------------------------------------------------------+   |
|   |                        blackbox.exe (Collector Daemon)                          |   |
|   |   - Mutex: "Global\BlackboxLogger.SingleInstance"                               |   |
|   |   - Waitable Timer: 10 Hz default (1..100 Hz configurable)                      |   |
|   |   - CPU Sampling: GetSystemTimes idle/total delta (0..100%)                     |   |
|   |   - Process Tracker: PID -> QueryFullProcessImageNameW on delta (Type 1 record) |   |
|   |   - Dynamic NVML: Temp, Power, Clocks, PCIe Gen/Width, Replay Counter           |   |
|   |   - Hot Loop: Pure Win32, Zero malloc/HeapAlloc, Zero printf, Zero locks        |   |
|   |   - Hot Loop CPU Usage: < 0.5% single core at 10 Hz                             |   |
|   |   - SetConsoleCtrlHandler (clean_shutdown = 1 on clean exit, 0 on crash)       |   |
|   +----------------------------------------+----------------------------------------+   |
|                                            | SetFilePointerEx + WriteFile               |
|                                            | FILE_FLAG_WRITE_THROUGH | FILE_SHARE_READ  |
|                                            v                                            |
|   +---------------------------------------------------------------------------------+   |
|   |                     ring.bin (Pre-allocated File: 64 MB default)                |   |
|   |   [4 KB Header: magic, version, calibration triple, clean_shutdown, generation] |   |
|   |   [Slot 0: 64B Record] [Slot 1: 64B Record] ... [Slot N-1: 64B Record]          |   |
|   +----------------------------------------+----------------------------------------+   |
|                                            |                                            |
|                    Post-Crash Reboot: Auto-Rotate if clean_shutdown == 0                |
|                    ==> Renamed to ring-crash-<YYYYMMDD-HHMMSS>.bin                      |
|                                            |                                            |
|                                            v                                            |
|   +---------------------------------------------------------------------------------+   |
|   |                    blackbox-analyze.exe (Post-Mortem Analyzer)                  |   |
|   |   - Auto-Discovery: selects newest crash ring with gap/crash tail               |   |
|   |   - CLI Syntax: blackbox-analyze.exe [ring_path] [--ring=<path>] [--gap=N]      |   |
|   |   - Ring Health Summary: total records, wrap count, actual vs nominal rate      |   |
|   |   - Tabular Telemetry Inspection: last 200 records before selected gap / tail   |   |
|   |   - Automated Anomaly Engine: HANG, POWER SPIKE, PCIE DROP, REPLAY JUMP, TORN   |   |
|   |   - Live NVML Hardware Query: compares live PCIe replay counter against ring    |   |
|   |   - Windows System Event Log: EvtQuery for Kernel-Power 41, BugCheck 1001, WHEA |   |
|   +---------------------------------------------------------------------------------+   |
+-----------------------------------------------------------------------------------------+
```

### Core Hot-Loop Guarantees
- **Zero Dynamic Memory Allocation**: All telemetry buffers and structs are stack-allocated; zero calls to `malloc`, `free`, `HeapAlloc`, or C++ `new` in the sampling path.
- **Zero Console I/O & Locks**: Zero `printf`, `wprintf`, or synchronization primitives in the hot loop.
- **CPU Footprint**: Well below 0.5% single CPU core utilization at 10 Hz.

---

## 2. Binary Format Specification

### 2.1 File Header (4096 Bytes, Little-Endian)
The first 4 KB (`0x0000..0x0FFF`) contains immutable metadata, geometric constants, and system clock calibrations:

| Offset | Size | Type | Field Name | Description |
|---|---|---|---|---|
| `0x0000` | 8 B | `u64` / `char[8]` | `magic` | ASCII `"BLKBOX01"` (`0x3130584F424B4C42`) |
| `0x0008` | 4 B | `u32` | `version` | Header format version (`1`) |
| `0x000C` | 4 B | `u32` | `header_size` | Header size in bytes (`4096`) |
| `0x0010` | 8 B | `u64` | `ring_bytes` | Total pre-allocated file size (e.g. `67,108,864`) |
| `0x0018` | 4 B | `u32` | `record_size` | Size of each telemetry record in bytes (`64`) |
| `0x001C` | 4 B | `u32` | `nominal_hz` | Collector sampling frequency in Hz (1..100, default `10`) |
| `0x0020` | 8 B | `u64` | `record_count` | Total physical record slots: `(ring_bytes - 4096) / 64` |
| `0x0028` | 8 B | `u64` | `cal_tsc` | TSC counter reading (`__rdtsc()`) at calibration instant |
| `0x0030` | 8 B | `u64` | `cal_qpc_100ns` | QPC timestamp scaled to 100ns units at calibration |
| `0x0038` | 8 B | `u64` | `cal_filetime` | Win32 `FILETIME` UTC timestamp at calibration |
| `0x0040` | 8 B | `u64` | `tsc_freq_hz` | Estimated hardware TSC frequency in Hertz |
| `0x0048` | 8 B | `u64` | `start_time` | Win32 `FILETIME` timestamp when collector initialized |
| `0x0050` | 8 B | `u64` | `reserved_cal` | Reserved for clock skew correction |
| `0x0058` | 4 B | `u32` | `clean_shutdown` | `0` = running/crash/kill, `1` = clean shutdown |
| `0x005C` | 4 B | `u32` | `generation` | Collector session generation counter |
| `0x0060..0x0FFF`| 3904 B | `u8[]` | `reserved` | Zero-padded to fill exactly 4096 bytes |

### 2.2 Record Format (64 Bytes, Little-Endian)
Records are persisted in 64-byte slots starting at byte offset `0x00001000` (4096).
Physical file offset formula:
```c
Offset = 4096 + (seq % record_count) * 64;
```

#### Type 0: Periodic Telemetry Record
Persisted on each timer interval (10 Hz nominal).
```
Offset  Size  Type  Field Name     Description
0x00    1 B   u8    type           0 = Telemetry Data Record
0x01    1 B   u8    pcie_gen       Current PCIe link gen (1..5, or 0xFF sentinel)
0x02    1 B   u8    pcie_width     Current PCIe link width (1..16, or 0xFF sentinel)
0x03    1 B   u8    cpu_pct        Total system CPU usage percent (0..100)
0x04    2 B   u16   gpu_temp_c     GPU die temperature in °C (or 0xFFFF sentinel)
0x06    2 B   u16   flags          Bit 0: nvml_available, Bit 1: fg_ok, Bit 2: timer_late
0x08    4 B   u32   gpu_power_mw   GPU board power in milliwatts (or 0xFFFFFFFF)
0x0C    4 B   u32   pcie_replay    PCIe replay counter (or 0xFFFFFFFF)
0x10    4 B   u32   sm_clock_mhz   GPU Streaming Multiprocessor clock in MHz
0x14    4 B   u32   mem_clock_mhz  GPU Memory clock in MHz
0x18    4 B   u32   fg_pid         Foreground window process ID
0x1C    4 B   u32   reserved0      Zero padding / alignment
0x20    8 B   u64   seq            Monotonic sequence counter (0, 1, 2, ...)
0x28    8 B   u64   tsc            Hardware TSC counter (__rdtsc)
0x30    8 B   u64   qpc_100ns      High-resolution QPC time (100ns units)
0x38   12 B   u8[]  reserved1      Reserved / zero padding (64 bytes total)
```

#### Type 1: Foreground Process Transition Record
Persisted whenever the active foreground window process changes.
```
Offset  Size  Type  Field Name     Description
0x00    1 B   u8    type           1 = Process Transition Record
0x01    1 B   u8    flags          Status flags (bit 1: fg_query_ok)
0x02    1 B   u8    name_len       Length of executable basename string
0x03    1 B   u8    pad0           Zero padding
0x04    4 B   u32   fg_pid         Foreground window process ID
0x08    8 B   u64   seq            Monotonic sequence counter
0x10    8 B   u64   tsc            Hardware TSC counter
0x18    8 B   u64   qpc_100ns      High-resolution QPC time (100ns units)
0x20   32 B   char  name[32]       Basename of executable (NUL-terminated, up to 31 chars)
```

#### Type 2: Calibration Synchronization Service Record
Persisted at daemon startup and periodically every 3600 seconds.
```
Offset  Size  Type  Field Name     Description
0x00    1 B   u8    type           2 = Calibration Resync Record
0x01    7 B   u8[]  reserved0      Zero padding
0x08    8 B   u64   seq            Monotonic sequence counter
0x10    8 B   u64   tsc            Hardware TSC counter
0x18    8 B   u64   qpc_100ns      High-resolution QPC time (100ns units)
0x20    8 B   u64   filetime       Win32 FILETIME UTC timestamp
0x28   24 B   u8[]  reserved1      Reserved / zero padding (64 bytes total)
```

#### Type 3: Heartbeat Service Record
Persisted every 10 seconds.
```
Offset  Size  Type  Field Name     Description
0x00    1 B   u8    type           3 = Heartbeat Record
0x01    1 B   u8    cpu_pct        Total system CPU usage percent (0..100)
0x02    2 B   u16   flags          Status flags
0x04    4 B   u32   fg_pid         Foreground process ID
0x08    8 B   u64   seq            Monotonic sequence counter
0x10    8 B   u64   tsc            Hardware TSC counter
0x18    8 B   u64   qpc_100ns      High-resolution QPC time (100ns units)
0x20    4 B   u32   uptime_sec     Collector daemon uptime in seconds
0x24   28 B   u8[]  reserved1      Reserved / zero padding (64 bytes total)
```

---

## 3. Command-Line Reference

### 3.1 `blackbox.exe` (Collector Daemon)
```
Usage: blackbox.exe [OPTIONS] [ring_path]

Options:
  --path=<path>, --ring-path=<path>  Set path to ring buffer file
                                     (Default: %ProgramData%\blackbox\ring.bin)
  --hz=<1..100>                      Sampling frequency in Hertz (Default: 10)
  --size-mb=<N>, --ring-size-mb=<N>  Ring file size in megabytes (Default: 64)
  --kb=<N>, --ring-kb=<N>            Ring file size in kilobytes
  -h, --help                         Display help and exit
```

### 3.2 `blackbox-analyze.exe` (Post-Mortem Analyzer)
```
Usage: blackbox-analyze.exe [ring_path] [--ring=<path>] [OPTIONS]

Arguments & Options:
  [ring_path], --ring=<path>   Path to ring buffer file or directory to inspect.
                               If omitted, auto-discovery automatically scans:
                                 1. Current working directory (.)
                                 2. %ProgramData%\blackbox\
                                 3. Executable directory
                               Default target: the newest crash ring (ring-crash-*.bin)
                               whose tail ends with a gap; if none, defaults to ring.bin.
  --gap=N                      Select 1-based gap index to analyze (Default: latest gap)
  --no-eventlog                Skip Windows System Event Log query
  -h, --help                   Display help and exit
```

#### Auto-Discovery Output Example:
```
Selected ring file for analysis: C:\ProgramData\blackbox\ring-crash-20260913-143022.bin
```

---

## 4. Build Instructions

### 4.1 Linux Host Cross-Compilation (Recommended)
Prerequisites: `cmake` and `x86_64-w64-mingw32-gcc` (MinGW-w64).
```bash
# Arch Linux / CachyOS:
sudo pacman -S cmake mingw-w64-gcc

# Ubuntu / Debian:
sudo apt install cmake gcc-mingw-w64-x86-64

# Compile Release binaries:
./build.sh --release
```
Artifacts are emitted directly into `bin/`:
- `bin/blackbox.exe`
- `bin/blackbox-analyze.exe`

### 4.2 Windows Native Build
Open a **Visual Studio x64 Native Tools Command Prompt** (for MSVC `cl.exe`) or a standard Command Prompt with MinGW `gcc.exe` in `PATH`:
```cmd
build.bat /release
```
- **MSVC Flags**: `/W4 /WX /O2 /MT /GS /GF /Gy /GL` (strictly 0 warnings with `/WX`).
- **MinGW Flags**: `-std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -static -static-libgcc`.

---

## 5. Automated Acceptance Testing

### 5.1 Wine / Linux Acceptance Testing
Run the automated test suite under Proton or Wine on Linux:
```bash
# Standalone scenario tests:
./tests/test_wrap.sh    # Circular buffer wrap and monotonic sequence continuity
./tests/test_kill.sh    # Forced termination & post-restart crash rotation recovery
./tests/test_nogpu.sh   # Graceful fallback & sentinel logging without NVML
./tests/test_torn.sh    # Corrupt/truncated boundary record detection & discard
./tests/test_rotate.sh  # Crash detection, atomic rotation & clean new ring

# Complete 178-test E2E Test Runner:
python3 tests/e2e_runner.py --analyzer=bin/blackbox-analyze.exe --verbose
```

### 5.2 Windows Native Acceptance Testing
Run the acceptance harness directly in Windows `cmd.exe`:
```cmd
run_tests_windows.bat
```
This executes all 5 tests natively:
1. `test_wrap.bat`: Capacity overflow & circular wraparound verification.
2. `test_kill.bat`: Forced kill (`taskkill /f`) and post-restart crash recovery.
3. `test_nogpu.bat`: Execution without NVML, confirming sentinel logging.
4. `test_torn.bat`: 30-byte truncated record boundary validation & recovery.
5. `test_rotate.bat`: Crash detection, atomic rotation to `ring-crash-*.bin`, and fresh `ring.bin` creation.

---

## 6. Anomaly Detection Engine

The analyzer runs automated anomaly detection rules across chronological samples:

| Anomaly Tag | Detection Criterion | Hardware / Diagnostic Meaning |
|---|---|---|
| `[HANG]` | Delta-t > 3x nominal period | Kernel freeze, high-priority DPC/ISR storm, or scheduler starvation. |
| `[POWER SPIKE]` | \|Delta-P_GPU\| > 100 W (100,000 mW) | Transient voltage surge, VRM collapse, or PSU trip (OCP/UVP). |
| `[PCIE DROP]` | Link Gen or Width decreased | Physical PCIe bus degradation, damaged traces, or riser cable failure. |
| `[REPLAY JUMP]` | PCIe replay counter increased (Delta > 0) | Physical layer PCIe link packet transmission errors requiring replay. |
| `[TORN RECORD]` | Corrupted magic/type/seq at boundary | Abrupt power interruption mid-record; boundary discarded safely. |

---

## 7. Manual Acceptance Test: Real Hardware Power-Cut

To verify absolute zero-loss crash durability on physical Windows hardware:

1. **Deploy Blackbox**: Run `install.bat` as Administrator on a physical Windows 10/11 test machine with an active workload (e.g. 3D game, GPU benchmark, or FurMark).
2. **Verify Execution**: Ensure `blackbox.exe` is running at 10 Hz and writing to `C:\ProgramData\blackbox\ring.bin`.
3. **Execute Power-Cut**: While telemetry is actively collecting under load, **physically pull the AC power cord** directly from the power supply unit (PSU). Do NOT use software shutdown.
4. **Boot Target Machine**: Reconnect the power cord and boot back into Windows.
5. **Observe Automatic Rotation**:
   - The `BlackboxLogger` scheduled task starts `blackbox.exe` on boot.
   - The collector detects `clean_shutdown == 0` from the power cut.
   - The collector atomically renames `ring.bin` to `ring-crash-<YYYYMMDD-HHMMSS>.bin`.
   - A fresh `ring.bin` is pre-allocated for the new session.
6. **Run Analyzer**: Open Command Prompt as Administrator and run:
   ```cmd
   "C:\Program Files\Blackbox\blackbox-analyze.exe"
   ```
7. **Expected Verification Result**:
   - The analyzer automatically discovers and selects the rotated `ring-crash-*.bin`.
   - The pre-crash telemetry tail is displayed in tabular format up to within **one single polling period (100 ms)** of the power cord disconnection.
   - If power was severed while writing a record slot, that partial record is cleanly identified and dropped as `[TORN RECORD]` without faulting.
   - The live NVML PCIe replay delta and Event Log correlation (Event 41: Kernel-Power) are presented accurately.
