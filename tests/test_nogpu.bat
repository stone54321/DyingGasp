@echo off
setlocal
python "%~dp0test_regressions.py" -v Regressions.test_no_gpu_sentinels_in_analyzer Regressions.test_no_gpu_collector_and_io_failure_paths
exit /b %errorlevel%
