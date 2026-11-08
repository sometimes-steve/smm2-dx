@echo off
rem smm2-dx Build Helper launcher. All it does is fetch the Python that recomp\tools\build.py runs on (pinned to a
rem sha256, into dist\smm2-dx_data\work, nothing is installed), then hand over to build.py.
rem Options: build-helper.cmd --clean, --run, --jobs n, --gen ...  (see the top of recomp\tools\build.py)
setlocal EnableExtensions
set "ROOT=%~dp0"
set "WORK=%ROOT%dist\smm2-dx_data\work"
set "PYDIR=%WORK%\python-windows-x86_64"

rem (a Python set up before the OpenCourse World account lookup was added has no pip left to add its packages: unpacked again)
if exist "%PYDIR%\README-LICENSES.txt" (
    findstr /c:"OpenCourse World account lookup" "%PYDIR%\README-LICENSES.txt" >nul 2>&1
    if not errorlevel 1 goto run
)

set "NAME=cpython-3.12.14+20260924-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"
set "URLNAME=cpython-3.12.14%%2B20260924-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"
set "SHA=c5bf8edfe858c1df9891be498b5bbc8761d383df5b9790658b088fea4870433a"
set "FILE=%WORK%\dl\%NAME%"
set "SYS=%SystemRoot%\System32"

if not exist "%WORK%\dl" mkdir "%WORK%\dl"
call :verify
if not errorlevel 1 goto unpack
echo Downloading Python 3.12 ...
if exist "%FILE%" del "%FILE%"
"%SYS%\curl.exe" -fL -sS --proto =https --retry 5 --retry-delay 2 --retry-all-errors -o "%FILE%" "https://github.com/astral-sh/python-build-standalone/releases/download/20260924/%URLNAME%"
if errorlevel 1 (echo Download failed: save the file as "%FILE%" and run this again. & exit /b 1)
call :verify
if errorlevel 1 (echo The downloaded Python does not match its pinned sha256. & del "%FILE%" & exit /b 1)

:unpack
if exist "%PYDIR%" rmdir /s /q "%PYDIR%"
mkdir "%PYDIR%"
"%SYS%\tar.exe" -xf "%FILE%" -C "%PYDIR%" --strip-components=1
if errorlevel 1 (echo Could not unpack Python. & exit /b 1)

:run
"%PYDIR%\python.exe" "%ROOT%recomp\tools\build.py" %*
exit /b %errorlevel%

:verify
if not exist "%FILE%" exit /b 1
set "GOT="
for /f "skip=1 tokens=* delims=" %%h in ('certutil -hashfile "%FILE%" SHA256') do if not defined GOT set "GOT=%%h"
set "GOT=%GOT: =%"
if /i "%GOT%"=="%SHA%" (exit /b 0) else (exit /b 1)
