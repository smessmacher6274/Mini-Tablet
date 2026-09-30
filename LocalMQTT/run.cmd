@echo off
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  echo Install the Python environment first; see README.md.
  pause
  exit /b 1
)
if exist "data\connection.json" (
  ".venv\Scripts\python.exe" server.py --lan
) else (
  ".venv\Scripts\python.exe" server.py
)
if errorlevel 1 pause
