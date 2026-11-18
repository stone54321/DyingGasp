@echo off
setlocal
python "%~dp0test_regressions.py" -v Regressions.test_truncation_keeps_original_slot_modulo
exit /b %errorlevel%
