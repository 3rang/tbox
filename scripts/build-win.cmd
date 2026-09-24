@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem Copyright (c) 2026 Tarang Patel
rem
rem Build tbox for Windows from the command line (classic flow):
rem     scripts\build-win.cmd
rem Options: BUILD_TYPE (Release default), VCVARS_ARCH (x64 default)
rem
rem Produces in build\:
rem   tbox.exe             main CLI
rem   qr-login.exe         demo\ (terminal QR-login, always built)
rem   tests\test_*.exe     unit tests
rem then runs the ctest gate. Configure with credentials for the live QR
rem flow:  cmake -B build -DTG_API_ID=<id> -DTG_API_HASH=<hash>
setlocal
set "ROOT=%~dp0.."
set "BUILD_TYPE=Release"
if not "%1"=="" set "BUILD_TYPE=%1"

for /f "usebackq tokens=*" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath`) do set "VSROOT=%%i"
if "%VSROOT%"=="" (
    echo ERROR: Visual Studio Build Tools not found. Install MSVC via the VS Installer.
    exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul

if not exist "%ROOT%\build" mkdir "%ROOT%\build"
cmake -S "%ROOT%" -B "%ROOT%\build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
if errorlevel 1 exit /b 1
cmake --build "%ROOT%\build"
if errorlevel 1 exit /b 1

echo.
echo Running test gate (ctest)...
ctest --test-dir "%ROOT%\build" -C %BUILD_TYPE% --output-on-failure
if errorlevel 1 exit /b 1

echo.
echo Build OK. Binaries:
dir /b "%ROOT%\build\*.exe" 2>nul
if exist "%ROOT%\build\tests" dir /b "%ROOT%\build\tests\*.exe" 2>nul
echo.
endlocal