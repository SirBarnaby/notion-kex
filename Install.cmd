@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Setup.ps1" -Action Install
if errorlevel 1 pause
pause