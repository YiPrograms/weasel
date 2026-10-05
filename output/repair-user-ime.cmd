@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0repair-user-ime.ps1"
exit /b %errorlevel%
