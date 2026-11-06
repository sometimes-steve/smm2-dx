@echo off
rem smm2-dx Build Helper launcher: runs build-helper.ps1 without needing a script policy change.
rem Options: build-helper.cmd -Package, -Clean, -Run, -Jobs n ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-helper.ps1" %*
