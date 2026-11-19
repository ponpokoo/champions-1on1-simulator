@echo off
setlocal
cd /d "%~dp0"

set "PY_CMD="
where py >nul 2>&1
if not errorlevel 1 (
  py -3 -c "import sys" >nul 2>&1
  if not errorlevel 1 set "PY_CMD=py -3"
)
if not defined PY_CMD (
  where python >nul 2>&1
  if not errorlevel 1 (
    python -c "import sys" >nul 2>&1
    if not errorlevel 1 set "PY_CMD=python"
  )
)
if not defined PY_CMD (
  echo [Error] Python 3 was not found in PATH.
  echo Install Python 3 from https://www.python.org/downloads/ and try again.
  pause
  exit /b 1
)

where gcc >nul 2>&1
if errorlevel 1 (
  echo [Error] gcc was not found in PATH.
  echo A C compiler ^(e.g. WinLibs / MSYS2 GCC^) is required to run simulations.
  echo Tip: You can still open viewer\index.html directly in your browser to view the bundled sample.
  pause
  exit /b 1
)

echo Starting Champions 1on1 Studio...
echo Press Ctrl+C in this window to stop the server.
%PY_CMD% viewer\server.py %*
if errorlevel 1 (
  echo.
  echo [Error] Server exited with an error. Check the log above.
  pause
  exit /b 1
)
