@echo off
setlocal
set "STYLOS_WINTAB_DEVICE=Wacom"
pushd "%~dp0"
start "" "Stylos.exe"
popd
