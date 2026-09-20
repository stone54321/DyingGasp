# Blackbox Deployment & Post-Crash Analysis Guide

This guide provides field operators, hardware QA engineers, and IT administrators with step-by-step procedures to deploy Blackbox onto physical Windows 10/11 target machines suffering from silent reboots, sudden power trips, hard lockups, or transient GPU/PCIe faults, and how to recover and inspect telemetry following an incident.

---

## 1. System Requirements & Prerequisites

- **Operating System**: Windows 10 x64 (Version 1809 / Build 17763 or later) or Windows 11 x64.
- **Privileges**: Local Administrator rights required for service installation and Scheduled Task creation.
- **Hardware Compatibility**:
  - Any x86-64 system.
  - **NVIDIA GPU**: Telemetry queries NVIDIA Management Library (`nvml.dll`) dynamically for temperature, board power, graphics/memory clock frequencies, PCIe link generation, link width, and PCIe replay error counts.
  - **Non-NVIDIA / AMD / Intel / Headless Systems**: Fully supported without crashes. If `nvml.dll` or an NVIDIA GPU is absent, all GPU telemetry fields record standard sentinels (`0xFFFF` temperature, `0xFFFFFFFF` power/clocks/replay, `0xFF` PCIe link) while foreground process monitoring and CPU utilization continue uninterrupted.
- **Dependencies**: None. Binaries are statically linked (`-static -static-libgcc` / `/MT`) and contain zero external DLL dependencies beyond standard Windows system libraries.

---

## 2. Preparing Deployment Media (USB Drive or SMB Share)

1. Build or locate the compiled binaries in `bin\`.
2. Copy the following files and folders onto your USB flash drive (or network share directory):
   ```
   D:\Blackbox\
   ├── blackbox.exe            # Pre-crash telemetry collector daemon
   ├── blackbox-analyze.exe    # Post-mortem crash analyzer utility
   ├── install.bat             # Elevated Scheduled Task installation script
   ├── uninstall.bat           # Clean teardown script
   ├── run_tests_windows.bat   # Windows acceptance test harness
   └── tests\                  # Modular acceptance test scripts (.bat)
   ```

---

## 3. Target Machine Installation

1. Connect the USB drive or map the network share on the target Windows PC.
2. Open File Explorer, navigate to the `Blackbox` folder, right-click `install.bat`, and select **"Run as administrator"**.
3. The installer performs the following automated steps:
   - Validates elevated administrative credentials (`net session`).
   - Copies `blackbox.exe` and `blackbox-analyze.exe` to `C:\Program Files\Blackbox\`.
   - Creates directory `C:\ProgramData\blackbox\` with appropriate ACLs (`SYSTEM` and Administrators full control, standard users read/execute).
   - Configures an elevated Windows Scheduled Task named **`BlackboxLogger`** via `schtasks.exe`:
     ```cmd
     schtasks /create /tn "BlackboxLogger" /tr "\"C:\Program Files\Blackbox\blackbox.exe\"" /sc ONSTART /ru "SYSTEM" /rl HIGHEST /f
     ```
     *(Note: Blackbox does NOT modify the Windows registry directly).*
   - Starts the `BlackboxLogger` task immediately via `schtasks /run /tn "BlackboxLogger"`.

### Operational Verification
Open Command Prompt as Administrator and verify daemon execution:
```cmd
tasklist /fi "imagename eq blackbox.exe"
```
Verify that the 64 MB write-through ring buffer has been pre-allocated:
```cmd
dir "C:\ProgramData\blackbox\ring.bin"
```
The file size must be exactly `67,108,864 bytes` (64 MB). It is pre-allocated with `FILE_FLAG_WRITE_THROUGH` and will never change size during execution.

---

## 4. Post-Crash Triage & Analysis Procedure

When the target machine experiences a silent reboot, blue screen (BSOD), kernel panic, or sudden power cut:

### Step 4.1: Automatic Post-Boot Rotation
1. Power on the target machine and boot back into Windows.
2. Upon Windows boot, the `BlackboxLogger` scheduled task launches `blackbox.exe` automatically under the `SYSTEM` account.
3. At startup, the daemon inspects `C:\ProgramData\blackbox\ring.bin`:
   - Because the system crashed abruptly, `clean_shutdown == 0`.
   - The collector atomically renames (rotates) `ring.bin` to `C:\ProgramData\blackbox\ring-crash-<YYYYMMDD-HHMMSS>.bin` (preserving up to 3 newest crash files).
   - A fresh, pre-allocated `ring.bin` is created for active collection.
   - **Result**: The pre-crash telemetry tail leading up to the exact moment of failure is safely preserved in `ring-crash-*.bin`.

### Step 4.2: Running the Post-Mortem Analyzer
Open **Command Prompt as Administrator** and execute:

```cmd
"C:\Program Files\Blackbox\blackbox-analyze.exe"
```

#### Auto-Discovery Mode (Default):
When executed without arguments, `blackbox-analyze.exe` automatically scans `C:\ProgramData\blackbox\` and selects the **newest crash ring (`ring-crash-*.bin`) whose tail ends with a gap/crash**. If no crash rings exist, it inspects the active `ring.bin`.
The analyzer explicitly outputs:
```
Selected ring file for analysis: C:\ProgramData\blackbox\ring-crash-20260913-143022.bin
```

#### Manual File Override:
To inspect a specific historical crash ring or active buffer, provide the file path directly:
```cmd
"C:\Program Files\Blackbox\blackbox-analyze.exe" --ring="C:\ProgramData\blackbox\ring-crash-20260913-143022.bin"
```
or positionally:
```cmd
"C:\Program Files\Blackbox\blackbox-analyze.exe" "C:\ProgramData\blackbox\ring-crash-20260913-143022.bin" --gap=3
```

---

## 5. Interpreting Diagnostic Output

The analyzer report is divided into five diagnostic sections:

### 1. Ring Health Summary
Displays total ring capacity, total valid records recovered, buffer wrap counts, calibration ratios, nominal vs. actual sampling rate, and mapped executables.

### 2. Automated Anomaly Detection
The anomaly engine flags critical system failures:
- `[HANG]`: Polling interval between records exceeds `3x` the nominal period (e.g. tick took >300 ms at 10 Hz), indicating kernel starvation, DPC/ISR storms, or hardware lockup.
- `[POWER SPIKE]`: GPU board power fluctuated by >100 W (100,000 mW) between consecutive samples, indicative of transient power rail collapse or PSU over-current trip (OCP).
- `[PCIE DROP]`: PCIe link generation or link width degraded relative to the previous sample (e.g. Gen4 x16 degraded to Gen1 x4), indicating PCIe bus instability, slot debris, or riser cable failure.
- `[REPLAY JUMP]`: Hardware PCIe replay counter incremented, confirming physical layer transmission retries across the PCIe bus.
- `[TORN RECORD]`: A record partially committed during power-off is detected at the boundary and discarded without memory fault.

### 3. Telemetry Inspection Window
Displays a high-resolution tabular timeline of the last 200 records immediately preceding the crash or selected gap. Fields include:
- Monotonic sequence number (`SEQ`)
- Microsecond-accurate UTC wall-clock time (`WALL CLOCK (UTC)`)
- GPU power draw (`GPU PWR`), temperature (`TEMP`), PCIe link state (`PCIE`), and replay error count (`REPLAY`)
- SM and memory clock speeds (`SM CLK`, `MEM CLK`)
- Total system CPU load percentage (`CPU%`)
- Foreground application executable basename and PID (`FG PROCESS`)

### 4. Live Hardware Recovery Delta
Queries the live NVIDIA GPU state via NVML and compares the current hardware PCIe replay counter against the final record committed before reboot, reporting any bus degradation sustained across the reboot cycle.

### 5. Windows System Event Log Correlation
Queries `wevtapi.dll` for critical reboot events:
- **Event ID 41 (Kernel-Power)**: The system rebooted without cleanly shutting down first.
- **Event ID 1001 (BugCheck)**: System crash dump / blue screen details.
- **WHEA-Logger (Event IDs 17, 18, 19, 41)**: Hardware architecture error reports.
*(Note: If the Windows Event Log service is unreachable or unprivileged, the analyzer prints `Event Log: unavailable` and exits code 0 without interruption).*

---

## 6. Target Uninstallation

To cleanly remove Blackbox after hardware triage:
1. Open Command Prompt as Administrator.
2. Run:
   ```cmd
   "C:\Program Files\Blackbox\uninstall.bat"
   ```
3. The uninstaller will:
   - Stop and unregister the `BlackboxLogger` scheduled task (`schtasks /end` and `schtasks /delete`).
   - Terminate any running `blackbox.exe` daemon processes (`taskkill /f`).
   - Remove binary files from `C:\Program Files\Blackbox\`.
   - **Preserve Telemetry Data**: The persistent ring buffer `ring.bin` and crash files `ring-crash-*.bin` in `C:\ProgramData\blackbox\` are intentionally left on disk for post-mortem archiving. To delete them permanently:
     ```cmd
     rmdir /s /q "C:\ProgramData\blackbox"
     ```
