# Blackbox: High-Durability Pre-Crash Telemetry Ring-Logger & Crash Analyzer

Blackbox is a pure C11 / Win32 user-mode telemetry ring-logger daemon (`blackbox.exe`) and post-mortem crash analyzer (`blackbox-analyze.exe`) engineered for Windows 10/11 x64. It records CPU, foreground-process and optional NVIDIA GPU telemetry for post-crash inspection. Anomaly tags identify observations, not proven causes of a reboot or hardware failure.

---

## 1. Architectural Overview & Data Flow

Blackbox is architected around the principle that kernel crashes, PSU over-current trips (OCP), and instant power cuts invalidate user-mode buffers and filesystem caches. Blackbox uses pre-allocated circular storage with FILE_FLAG_WRITE_THROUGH and no explicit dynamic allocation in its sampling loop. This reduces buffering; it does not guarantee lossless recovery from power failure or atomic 64-byte writes.

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
|   |   - CPU usage: hardware/driver/storage dependent                             |   |
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
- **Sampling path**: No console printing or explicit heap allocation. The collector waits on a timer/stop event; a separate hidden-window thread handles session shutdown notifications.
- **CPU Footprint**: Depends on the driver, hardware and storage latency; measure on the target machine. No universal utilization bound is guaranteed.

---

## 2. Binary Format Specification

### 2.1 File Header (4096 Bytes, Little-Endian)
The first 4 KB (`0x0000..0x0FFF`) contains geometry, clock anchors and mutable shutdown/session metadata. The packed structures in `include/blackbox_format.h` are authoritative:

| Offset | Size | Type | Field Name | Description |
|---|---|---|---|---|
| `0x0000` | 8 B | u64 | magic | ASCII BLKBOX01 |
| `0x0008` | 4 B | u32 | version | 1 |
| `0x000C` | 4 B | u32 | header_size | 4096 |
| `0x0010` | 8 B | u64 | ring_bytes | Header plus all record slots |
| `0x0018` | 4 B | u32 | record_size | 64 |
| `0x001C` | 4 B | u32 | record_count | `(ring_bytes - 4096) / 64` |
| `0x0020` | 8 B | u64 | qpc_freq | QPC ticks per second |
| `0x0028` | 8 B | u64 | qpc_start_ticks | Initial raw QPC |
| `0x0030` | 8 B | u64 | qpc_start_100ns | Initial QPC in 100 ns units |
| `0x0038` | 8 B | u64 | tsc_start | Initial TSC |
| `0x0040` | 8 B | u64 | filetime_start | UTC FILETIME paired with QPC |
| `0x0048` | 4 B | u32 | nominal_hz | 1..100 Hz |
| `0x004C` | 4 B | u32 | daemon_pid | Current writer PID |
| `0x0050` | 8 B | u64 | reserved_flags | Reserved |
| `0x0058` | 4 B | u32 | clean_shutdown | In reserved_padding: 0 active/unclean, 1 clean |
| `0x005C` | 4 B | u32 | generation | In reserved_padding: clean-resume counter |
| `0x0060` | 4000 B | u8[] | reserved | Zero padding |

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
0x1C    8 B   u64   seq            Monotonic sequence counter (0, 1, 2, ...)
0x24    8 B   u64   tsc            Hardware TSC counter (__rdtsc)
0x2C    8 B   u64   qpc_100ns      High-resolution QPC time (100ns units)
0x34   12 B   u8[]  reserved1      Reserved / zero padding (64 bytes total)
```

#### Type 1: Foreground Process Transition Record
Persisted whenever the active foreground window process changes.
```
Offset  Size  Type  Field Name     Description
0x00    1 B   u8    type           1 = Process Transition Record
0x01    1 B   u8    flags          Status flags (bit 1: fg_query_ok)
0x02    1 B   u8    name_len       Length of executable basename string
0x03    1 B   u8    pad0           Zero padding
0x04    8 B   u64   seq            Monotonic sequence counter
0x0C    8 B   u64   tsc            Hardware TSC counter
0x14    8 B   u64   qpc_100ns      High-resolution QPC time (100ns units)
0x1C    4 B   u32   fg_pid         Foreground window process ID
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
Open a **Visual Studio x64 Native Tools Command Prompt** (for MSVC `cl.exe` with C11 support (Visual Studio 2019 16.7 or newer)) or a standard Command Prompt with MinGW `gcc.exe` in `PATH`:
```cmd
build.bat /release
```
- **MSVC Flags**: `/std:c11 /utf-8 /W4 /WX /O2 /MT /GS /GF /Gy /GL` (strictly 0 warnings with `/WX`).
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
This executes native regression tests followed by the legacy E2E suite. Python 3 is required.

```cmd
python tests/test_regressions.py -v
python tests/test_adversarial.py
```

The regression suite checks actual binaries, including collector wrap, forced kill,
byte-identical crash rotation, clean resume, session-end notifications, disk/timer
failure propagation, no-GPU sentinels, Unicode paths and analyzer output.
It stops only processes it starts. `build.bat` also builds `blackbox-test-core.exe`,
which the regression suite uses for controlled I/O/timer failures and no-GPU sampling.
The five `tests/test_*.bat` files are shortcuts to selected regression cases.

The legacy **178** E2E checks contain **174 Python oracle checks and only 4 executable
checks**. They must not be described as 178 independent checks of the C program.
The adversarial suite runs 48 analyzer cases natively on Windows or through Wine.
Linux shell/stress harnesses have not been revalidated by this Windows audit.

---

## 6. Anomaly Detection Engine

The analyzer runs automated anomaly detection rules across chronological samples:

| Anomaly Tag | Detection Criterion | Hardware / Diagnostic Meaning |
|---|---|---|
| `[HANG]` | Time between data samples > 3x nominal period | Sampling delay; possible scheduling, driver or storage stall. |
| `[POWER SPIKE]` | Absolute GPU power change > 100 W | Observed power change; normal workload transitions may also trigger it. |
| `[PCIE DROP]` | An available link generation or width decreases | Observed link change; power management can cause normal changes. |
| `[REPLAY JUMP]` | Available replay counter increases | Observed retransmission counter change; investigate with other evidence. |
| `[TORN RECORD]` | Truncated file, invalid record type or slot/sequence mismatch | Structural inconsistency; v1 has no checksum and cannot detect every torn write. |

Telemetry comparisons skip intervening service records. Wall times use retained
calibration records in sequence order; the header is the fallback anchor. If a
session's calibration has already been overwritten by the ring, historical clock
corrections cannot always be reconstructed. Process names are also unavailable
when the relevant transition has been overwritten. Missing data is not inferred.

---

## 7. Runtime behavior and limits

- Stop an interactive run with Ctrl+C or Ctrl+Break. Successful cleanup marks the
  header clean; timer or write failures return a nonzero exit code and leave it unclean.
- A hidden top-level window handles WM_ENDSESSION because a program loading user32
  does not receive console logoff/shutdown events. Windows can still forcibly
  terminate a process that cannot finish within its shutdown deadline.
- Crash rotation uses `ring-crash-YYYYMMDD-HHMMSS-mmm-NNN.bin` and never replaces a
  same-name file. The existing retention policy keeps up to three recent crash files.
  A failed rename stops collection instead of overwriting the evidence.
- An invalid existing header is preserved and rejected. A clean ring can resume
  only with the same size and sampling rate; use a new `--path` to change them.
- Invalid numbers, empty/overlong paths and unknown options fail explicitly.
  If the default directory is unwritable, select a writable `--path`.
- Running from the SYSTEM startup task does not capture another user's foreground
  desktop. Run interactively in that user's session when foreground tracking matters.
- The event-log report lists recent matching system events; it does **not** prove
  their temporal or causal connection to the selected ring. The live PCIe counter
  can reset at reboot or belong to different hardware; its delta is not a diagnosis.
- Write-through is not unbuffered I/O, a storage-controller guarantee, or a checksum.
  Recovery after a real power cut depends on the hardware and driver stack. A
  forced-process-termination test does not verify power-loss durability. No physical
  power-cut, BSOD, system reboot or service installation was performed in the audit.

Win32 behavior: [file caching](https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching),
[write-through flags](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/ns-fileapi-createfile2_extended_parameters),
[shutdown handler requirements](https://learn.microsoft.com/en-us/windows/console/registering-a-control-handler-function).
