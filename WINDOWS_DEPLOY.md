# Windows deployment

Build from the repository root with `build.bat /release`. To verify before installing,
run `run_tests_windows.bat` and `python tests/test_adversarial.py` (Python 3 required).
Keep the complete source tree when running tests; copying only the two production
executables is sufficient for collection and analysis.

## Interactive use

```cmd
bin\blackbox.exe --path="C:\blackbox-data\ring.bin" --size-mb=64 --hz=10
```

Choose a writable data directory. Stop with Ctrl+C, then inspect:

```cmd
bin\blackbox-analyze.exe "C:\blackbox-data\ring.bin" --no-eventlog
```

Foreground process tracking requires running in the user's desktop session.
Only one collector may run at a time. No GPU is required; unavailable sensors
are represented as N/A. A clean ring resumes only with the same size/rate.
Use a new path when changing configuration; do not delete evidence to bypass errors.

## Optional boot task

`install.bat` installs binaries under `%ProgramFiles%\Blackbox` and registers the
`BlackboxLogger` task at system startup as SYSTEM. It requires Administrator rights.
This mode records CPU/GPU telemetry but cannot observe another user's foreground
desktop in their interactive session. The installer and uninstall scripts stop
running collectors, so preserve needed logs before an update.

The installer is a deployment tool, not part of the automated tests. Installation,
real reboot/shutdown delivery, battery behavior and long-running scheduled-task
operation were not exercised during the code audit. Review Task Scheduler settings
(including execution-time and battery limits) for a continuously running deployment.

## After an incident

At restart an unclean `ring.bin` is renamed to
`ring-crash-YYYYMMDD-HHMMSS-mmm-NNN.bin`; three recent crash archives are retained.
Back up an important archive before more incidents occur. If rotation fails,
collection stops with an error rather than overwriting the file.

```cmd
"C:\Program Files\Blackbox\blackbox-analyze.exe" "%ProgramData%\blackbox"
```

Passing the directory explicitly avoids accidentally selecting `ring.bin` in the
current directory. Without a path, discovery checks the current directory, then
ProgramData, then the executable directory; each search prefers a crash archive.

An unclean header can mean a crash, forced termination, or a still-running writer.
A continuous sequence does not prove a clean shutdown. Anomaly labels are clues,
not proof of a faulty power supply, GPU or PCIe link. See README for the actual
binary format, durability limits and event-log limitations.
