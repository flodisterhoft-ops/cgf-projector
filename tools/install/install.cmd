@echo off
rem Installs CGF Projector for this Windows user (see install.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
