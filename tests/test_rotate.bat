@echo off
setlocal
python "%~dp0test_regressions.py" -v Regressions.test_multiple_rapid_crashes_preserved Regressions.test_failed_rotation_preserves_original
exit /b %errorlevel%
