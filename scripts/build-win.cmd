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
rem then runs the ctest gate. Credentials are NOT build options: the live QR
rem flow reads TG_API_ID / TG_API_HASH from the environment at run time.
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

rem If a previous configure used another generator (e.g. VS Code/CMake Tools
rem defaults to "Visual Studio 18 2026"), CMake refuses to reuse the dir.
rem Detect and wipe it so scripts always configure with NMake Makefiles.
if exist "%ROOT%\build\CMakeCache.txt" (
    findstr /i /c:"CMAKE_GENERATOR:INTERNAL=NMake Makefiles" "%ROOT%\build\CMakeCache.txt" >nul 2>&1
    if errorlevel 1 (
        echo [build-win] stale build/ from another generator - wiping...
        rmdir /s /q "%ROOT%\build"
    )
)
if not exist "%ROOT%\build" mkdir "%ROOT%\build"

rem Configure with tests unless the user asked for them off (build-win always
rem configures with whatever the cache already holds; to switch, re-configure:
rem   cmake -S "%ROOT%" -B "%ROOT%\build" -DTBOX_BUILD_TESTS=OFF
cmake -S "%ROOT%" -B "%ROOT%\build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
if errorlevel 1 exit /b 1
cmake --build "%ROOT%\build"
if errorlevel 1 exit /b 1

rem Test gate: with no tests to run ctest exits nonzero, so honour
rem -DTBOX_BUILD_TESTS=OFF and skip the gate instead.
findstr /i /c:"TBOX_BUILD_TESTS:BOOL=OFF" "%ROOT%\build\CMakeCache.txt" >nul 2>&1
if not errorlevel 1 goto notests
echo.
echo Running test gate (ctest)...
ctest --test-dir "%ROOT%\build" -C %BUILD_TYPE% --output-on-failure
if errorlevel 1 exit /b 1
goto gate_done
:notests
echo.
echo Tests disabled - skipping ctest gate.
:gate_done

echo.
echo Build OK. Binaries:
dir /b "%ROOT%\build\*.exe" 2>nul
if exist "%ROOT%\build\tests" dir /b "%ROOT%\build\tests\*.exe" 2>nul
echo.
endlocal