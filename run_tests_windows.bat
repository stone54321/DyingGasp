@echo off
setlocal
python "%~dp0tests\test_regressions.py" -v
if errorlevel 1 exit /b 1
python "%~dp0tests\e2e_runner.py" --analyzer="%~dp0bin\blackbox-analyze.exe" --json-out="%~dp0tests\test_report.json"
exit /b %errorlevel%
