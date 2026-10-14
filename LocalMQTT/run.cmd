@echo off
setlocal
set PYTHONUTF8=1
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  echo Install the Python environment first; see README.md.
  pause
  exit /b 1
)
echo Starting Note Tablet server...
echo Logs appear here and are saved to data\server.log
echo Authentication mode is shown in the startup logs.
echo.
if exist "data\connection.json" (
  ".venv\Scripts\python.exe" -u server.py --lan --log-file data\server.log
) else (
  ".venv\Scripts\python.exe" -u server.py --log-file data\server.log
)
if errorlevel 1 echo Server stopped with an error. Check data\server.log and the output above.
pause
