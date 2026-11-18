@echo off
setlocal
python "%~dp0test_regressions.py" -v Regressions.test_collector_kill_rotate_wrap_and_clean_resume
exit /b %errorlevel%
