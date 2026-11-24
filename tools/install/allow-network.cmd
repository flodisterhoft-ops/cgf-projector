@echo off
rem Lets the church network reach CGF Projector without Windows Firewall asking
rem after every update (see allow-network.ps1). Asks for administrator rights.
powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','\"%~dp0allow-network.ps1\"'"
