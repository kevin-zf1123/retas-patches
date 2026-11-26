@echo off
setlocal
if "%~1"=="" (
    powershell.exe -NoProfile -STA -ExecutionPolicy RemoteSigned -File "%~dp0MotionSetup.ps1" -Action Restore
) else (
    powershell.exe -NoProfile -STA -ExecutionPolicy RemoteSigned -File "%~dp0MotionSetup.ps1" -Action Restore -StylosDir "%~1"
)
if errorlevel 1 (
    pause
    exit /b 1
)
pause
