"""Black-box regressions against the actual Windows binaries (no oracle-only passes).

Run on Windows: python tests/test_regressions.py [-v]
Every collector is launched in an isolated directory and stopped by its own PID.
No installation, scheduled tasks or termination of other collectors is performed.
"""
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.generate_test_ring import (
    build_header, pack_type0_data, pack_type1_process,
    pack_type2_cal, pack_type3_heartbeat,
)

BIN = Path(os.environ.get('BLACKBOX_TEST_BIN', ROOT / 'bin')).resolve()


def data(seq, tick=None, **kwargs):
    return pack_type0_data(seq, 100_000_000 + (seq if tick is None else tick) * 1_000_000,
                           3_000_000_000 + seq * 300_000_000, **kwargs)


@unittest.skipUnless(sys.platform == 'win32', 'Native Windows binary tests')
class Regressions(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='blackbox-regression-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def ring(self, records, slots=16, clean=True, name='ring.bin'):
        buf = bytearray(build_header(4096 + slots * 64))
        struct.pack_into('<I', buf, 88, int(clean))
        buf.extend(bytes(slots * 64))
        for seq, record in records:
            offset = 4096 + (seq % slots) * 64
            buf[offset:offset + 64] = record
        path = self.directory / name
        path.write_bytes(buf)
        return path

    def analyze(self, path, *args):
        return subprocess.run([str(BIN / 'blackbox-analyze.exe'), str(path),
                               '--no-eventlog', *args], capture_output=True,
                              encoding='utf-8', errors='replace', timeout=10)

    def spawn(self, path, *args):
        process = subprocess.Popen([str(BIN / 'blackbox.exe'), '--path=' + str(path), *args],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)
        self.addCleanup(cleanup)
        return process

    def wait_records(self, process, path, minimum=4):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process.poll() is not None:
                self.fail('Collector exited early: ' + repr(process.communicate()))
            if path.exists():
                raw = path.read_bytes()
                nonempty = sum(any(raw[i:i+64]) for i in range(4096, len(raw), 64))
                if (len(raw) >= 4096 and struct.unpack_from('<I', raw, 76)[0] == process.pid
                        and nonempty >= minimum):
                    return raw
            time.sleep(.02)
        self.fail('Collector did not persist samples')

    def test_service_records_do_not_hide_gpu_anomalies(self):
        services = [pack_type1_process(1, 101_000_000, 1, 55, 'game.exe'),
                    pack_type2_cal(1, 101_000_000, 1, 133700000001000000),
                    pack_type3_heartbeat(1, 101_000_000, 1, 10)]
        for record in services:
            with self.subTest(type=record[0]):
                path = self.ring([(0, data(0)), (1, record),
                                  (2, data(2, gpu_power_mw=350000, pcie_gen=2, pcie_replay=7))])
                result = self.analyze(path)
                self.assertEqual(result.returncode, 0, result.stderr)
                for tag in ('[POWER SPIKE]', '[PCIE DROP]', '[REPLAY JUMP]'):
                    self.assertIn(tag, result.stdout)

    def test_rate_counts_telemetry_intervals(self):
        path = self.ring([(0, data(0, tick=0)),
                          (1, pack_type3_heartbeat(1, 100_000_000, 1, 10)),
                          (2, data(2, tick=1))])
        result = self.analyze(path)
        self.assertRegex(result.stdout, r'Actual Sampling Rate\s*: 10\.00 Hz')

    def test_heartbeat_does_not_hide_polling_gap(self):
        path = self.ring([(0, data(0)),
                          (1, pack_type3_heartbeat(1, 102_000_000, 1, 10)),
                          (2, data(2, tick=4))])
        self.assertIn('[HANG]', self.analyze(path).stdout)

    def test_pcie_drop_with_other_sensor_unavailable(self):
        path = self.ring([(0, data(0, pcie_width=255)),
                          (1, data(1, pcie_width=255, pcie_gen=2))])
        self.assertIn('[PCIE DROP]', self.analyze(path).stdout)

    def test_unclean_continuous_tail_not_reported_clean(self):
        result = self.analyze(self.ring([(0, data(0)), (1, data(1))], clean=False))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('(clean tail)', result.stdout)
        self.assertIn('unclean', result.stdout)

    def test_unicode_path(self):
        path = self.ring([(0, data(0))], name='журнал Қазақстан.bin')
        result = self.analyze(path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Total Valid Records   : 1', result.stdout)

    def test_calibration_record_updates_wall_clock(self):
        path = self.ring([(0, data(0)),
                          (1, pack_type2_cal(1, 101_000_000, 1, 133700036001000000)),
                          (2, data(2))])
        out = self.analyze(path).stdout
        times = re.findall(r'(?:First|Last) Record Time\s*: (.*?) \(Seq', out)
        self.assertEqual(len(times), 2)
        from datetime import datetime
        delta = datetime.strptime(times[1], '%Y-%m-%d %H:%M:%S.%f UTC') - datetime.strptime(times[0], '%Y-%m-%d %H:%M:%S.%f UTC')
        self.assertAlmostEqual(delta.total_seconds(), 3600.2, places=2)

    def test_process_names_follow_timeline_after_wrap(self):
        path = self.ring([(14, pack_type1_process(14, 114_000_000, 1, 1337, 'old.exe')),
                          (15, data(15)),
                          (16, pack_type1_process(16, 116_000_000, 1, 1337, 'new.exe')),
                          (17, data(17))])
        out = self.analyze(path).stdout
        self.assertRegex(out, r'\|\s+15\s+\|[^\n]*old\.exe')
        self.assertRegex(out, r'\|\s+17\s+\|[^\n]*new\.exe')

    def test_no_gpu_sentinels_in_analyzer(self):
        path = self.ring([(0, data(0, pcie_gen=255, pcie_width=255, gpu_temp_c=65535,
                                  gpu_power_mw=0xFFFFFFFF, pcie_replay=0xFFFFFFFF,
                                  sm_clock_mhz=0xFFFFFFFF, mem_clock_mhz=0xFFFFFFFF, flags=0))])
        result = self.analyze(path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreaterEqual(result.stdout.count('N/A'), 6)
        self.assertNotIn('[POWER SPIKE]', result.stdout)

    def test_no_gpu_collector_and_io_failure_paths(self):
        result = subprocess.run([str(BIN / 'blackbox-test-core.exe')],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PASS:', result.stdout)

    def test_multiple_rapid_crashes_preserved(self):
        path = self.directory / 'ring.bin'
        snapshots = []
        for i in range(3):
            process = self.spawn(path, '--kb=5', '--hz=100')
            self.wait_records(process, path)
            process.kill()
            process.communicate(timeout=3)
            snapshots.append(path.read_bytes())
        archives = [p.read_bytes() for p in self.directory.glob('ring-crash-*.bin')]
        self.assertEqual(len(archives), 2)
        for original in snapshots[:2]:
            self.assertIn(original, archives)

    def test_session_end_notification_flushes_clean_header(self):
        import ctypes
        from ctypes import wintypes
        path = self.directory / 'ring.bin'
        process = self.spawn(path, '--kb=5')
        self.wait_records(process, path)
        user32 = ctypes.windll.user32
        user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
        user32.FindWindowW.restype = wintypes.HWND
        window = user32.FindWindowW('BlackboxLogger.ShutdownListener', None)
        self.assertTrue(window)
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
        self.assertEqual(owner.value, process.pid)
        send = user32.SendMessageW
        send.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        send.restype = ctypes.c_ssize_t
        # Deliver only to this test window. This does NOT shut down Windows.
        send(window, 0x16, 0, 0)  # canceled WM_ENDSESSION
        self.assertIsNone(process.poll())
        send(window, 0x16, 1, 0)
        process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0)
        self.assertEqual(struct.unpack_from('<I', path.read_bytes(), 88)[0], 1)

    def test_failed_rotation_preserves_original(self):
        import ctypes
        from ctypes import wintypes
        path = self.ring([(0, data(0))], clean=False)
        original = path.read_bytes()
        create = ctypes.windll.kernel32.CreateFileW
        create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                           wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        create.restype = wintypes.HANDLE
        close = ctypes.windll.kernel32.CloseHandle
        close.argtypes = [wintypes.HANDLE]
        # Allow reading and writing, but deny rename/delete while held.
        handle = create(str(path), 0x80000000, 3, None, 3, 0, None)
        self.assertNotEqual(handle, wintypes.HANDLE(-1).value)
        try:
            process = self.spawn(path, '--kb=5')
            process.communicate(timeout=3)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(path.read_bytes(), original)
        finally:
            close(handle)

    def test_invalid_analyzer_arguments_fail(self):
        path = self.ring([(0, data(0))])
        for arg in ('--gap=abc', '--gap=0', '--gap=-1', '--gap=1x',
                    '--gap=999999999999999999999', '--ring=', '--bogus'):
            with self.subTest(arg=arg):
                result = self.analyze(path, arg)
                self.assertNotEqual(result.returncode, 0)

    def test_invalid_collector_arguments_do_not_create_files(self):
        path = self.directory / 'invalid.bin'
        for arg in ('--hz=0', '--hz=101', '--hz=ten', '--kb=-1', '--kb=4',
                    '--size-mb=999999999999999999999', '--bogus'):
            with self.subTest(arg=arg):
                process = self.spawn(path, arg)
                process.communicate(timeout=3)
                self.assertNotEqual(process.returncode, 0)
                self.assertFalse(path.exists())

    def test_inconsistent_geometry_rejected(self):
        path = self.ring([(0, data(0))])
        raw = bytearray(path.read_bytes())
        struct.pack_into('<I', raw, 28, 0xFFFFFFFF)
        path.write_bytes(raw)
        result = self.analyze(path)
        self.assertNotEqual(result.returncode, 0)

    def test_truncation_keeps_original_slot_modulo(self):
        path = self.ring([(i, data(i)) for i in range(16, 32)])
        path.write_bytes(path.read_bytes()[:-30])
        result = self.analyze(path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Total Valid Records   : 15', result.stdout)
        self.assertIn('[TORN RECORD]', result.stdout)

    def test_clean_resume_rejects_resize_without_modifying_evidence(self):
        path = self.ring([(i, data(i)) for i in range(16)])
        original = path.read_bytes()
        process = self.spawn(path, '--kb=8')
        process.communicate(timeout=3)
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(path.read_bytes(), original)

    def test_invalid_existing_file_preserved(self):
        path = self.directory / 'evidence.bin'
        original = b'not a valid header, do not overwrite'
        path.write_bytes(original)
        process = self.spawn(path, '--kb=8')
        process.communicate(timeout=3)
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(path.read_bytes(), original)

    def test_collector_kill_rotate_wrap_and_clean_resume(self):
        path = self.directory / 'ring.bin'
        process = self.spawn(path, '--kb=5', '--hz=100')
        self.wait_records(process, path, 16)
        time.sleep(.2)
        process.kill()
        process.communicate(timeout=3)
        before_crash = path.read_bytes()
        self.assertEqual(struct.unpack_from('<I', before_crash, 88)[0], 0)
        result = self.analyze(path)
        self.assertEqual(result.returncode, 0, result.stderr)
        match = re.search(r'Buffer Wrap Count\s*: (\d+)', result.stdout)
        self.assertGreater(int(match[1]), 0)
        process2 = self.spawn(path, '--kb=5', '--hz=100')
        deadline = time.monotonic() + 5
        while not list(self.directory.glob('ring-crash-*.bin')) and time.monotonic() < deadline:
            time.sleep(.02)
        archives = list(self.directory.glob('ring-crash-*.bin'))
        self.assertEqual(len(archives), 1)
        self.assertEqual(archives[0].read_bytes(), before_crash)
        self.wait_records(process2, path)
        process2.send_signal(signal.CTRL_BREAK_EVENT)
        process2.communicate(timeout=5)
        self.assertEqual(process2.returncode, 0)
        clean = path.read_bytes()
        self.assertEqual(struct.unpack_from('<I', clean, 88)[0], 1)
        process3 = self.spawn(path, '--kb=5', '--hz=100')
        self.wait_records(process3, path)
        time.sleep(.1)
        process3.send_signal(signal.CTRL_BREAK_EVENT)
        process3.communicate(timeout=5)
        self.assertEqual(process3.returncode, 0)
        self.assertEqual(struct.unpack_from('<I', path.read_bytes(), 92)[0], 1)
        self.assertEqual(len(list(self.directory.glob('ring-crash-*.bin'))), 1)

    def test_60hz_timer_does_not_run_at_62_5hz(self):
        path = self.directory / 'ring.bin'
        process = self.spawn(path, '--kb=16', '--hz=60')
        self.wait_records(process, path, 70)
        process.send_signal(signal.CTRL_BREAK_EVENT)
        process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0)
        out = self.analyze(path).stdout
        rate = float(re.search(r'Actual Sampling Rate\s*: ([\d.]+)', out)[1])
        self.assertGreater(rate, 30)  # Allow slow CI machines to miss deadlines.
        self.assertLess(rate, 61)


if __name__ == '__main__':
    unittest.main()
