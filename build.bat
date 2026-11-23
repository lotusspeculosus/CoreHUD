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

rem --- build stamp: git commit (+ if there are uncommitted changes) and date -> build\buildinfo.h ---
set "GITREV=nogit"
for /f %%i in ('git rev-parse --short HEAD 2^>nul') do set "GITREV=%%i"
git diff --quiet HEAD -- src 2>nul || if not "%GITREV%"=="nogit" set "GITREV=%GITREV%+"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd"') do set "BDATE=%%i"
> build\buildinfo.h echo #define RIGSENSE_BUILD_REV "%GITREV%"
>> build\buildinfo.h echo #define RIGSENSE_BUILD_DATE "%BDATE%"

set CFLAGS=/nologo /std:c++17 /O2 /MT /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Ibuild /Fobuild\
set LIBS=user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib wbemuuid.lib crypt32.lib dxgi.lib pdh.lib setupapi.lib hid.lib winhttp.lib bcrypt.lib
cl %CFLAGS% src\cli.cpp src\sensors.cpp src\gpu_generic.cpp src\pinsources.cpp src\nvapi_gpu.cpp src\nvml_gpu.cpp /Fe:bin\rigsense-cli.exe /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
rc /nologo /Isrc /fo build\app.res src\app.rc || exit /b 1
cl %CFLAGS% src\app.cpp src\monitor.cpp src\pinsources.cpp src\panel.cpp src\sensors.cpp src\gpu_generic.cpp src\components.cpp src\updater.cpp src\nvapi_gpu.cpp src\nvml_gpu.cpp build\app.res /Fe:bin\RigSense.exe /link /SUBSYSTEM:WINDOWS d2d1.lib dwrite.lib dwmapi.lib windowscodecs.lib comdlg32.lib %LIBS% || exit /b 1
echo.
echo Built bin\RigSense.exe and bin\rigsense-cli.exe (%GITREV%, %BDATE%)
