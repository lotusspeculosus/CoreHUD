@echo off
rem Builds and runs the unit tests.
setlocal
cd /d "%~dp0\.."
if defined VSCMD_VER goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%%VSWHERE%%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
:build
if not exist build\tests mkdir build\tests
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Fobuild\tests\ tests\test_panel.cpp src\panel.cpp src\monitor.cpp src\nvapi_gpu.cpp src\nvml_gpu.cpp /Fe:build\tests\test_panel.exe /link user32.lib crypt32.lib >nul || (echo BUILD FAILED & exit /b 1)
build\tests\test_panel.exe
