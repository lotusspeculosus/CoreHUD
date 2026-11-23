@echo off
rem Builds RigSense.exe (GUI) and rigsense-cli.exe (console) with MSVC.
setlocal
cd /d "%~dp0"
if defined VSCMD_VER goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%%VSWHERE%%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (echo Visual Studio C++ build tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
:build
if not exist build mkdir build
if not exist bin mkdir bin
set CFLAGS=/nologo /std:c++17 /O2 /MT /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Fobuild\
set LIBS=user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib wbemuuid.lib crypt32.lib dxgi.lib pdh.lib setupapi.lib hid.lib winhttp.lib
cl %CFLAGS% src\cli.cpp src\sensors.cpp src\gpu_generic.cpp src\pinsources.cpp src\nvapi_gpu.cpp src\nvml_gpu.cpp /Fe:bin\rigsense-cli.exe /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
cl %CFLAGS% src\app.cpp src\monitor.cpp src\pinsources.cpp src\panel.cpp src\sensors.cpp src\gpu_generic.cpp src\components.cpp src\nvapi_gpu.cpp src\nvml_gpu.cpp /Fe:bin\RigSense.exe /link /SUBSYSTEM:WINDOWS d2d1.lib dwrite.lib dwmapi.lib windowscodecs.lib comdlg32.lib %LIBS% || exit /b 1
echo.
echo Built bin\RigSense.exe and bin\rigsense-cli.exe
