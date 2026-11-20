@echo off
rem Double-click this file (or run it from cmd/PowerShell) to build and play FocusBird.
rem Extra arguments are passed through, e.g.  run-focusbird.cmd --keyboard
setlocal
cd /d "%~dp0"
set FOCUSBIRD_PAUSE=1
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 "%~dp0run-focusbird.py" %*
) else (
    where python >nul 2>nul
    if errorlevel 1 (
        echo Python 3 is not installed. Install it with:  winget install Python.Python.3.12
        echo Then close and reopen this window and run this file again.
        pause
        exit /b 1
    )
    python "%~dp0run-focusbird.py" %*
)
exit /b %errorlevel%
