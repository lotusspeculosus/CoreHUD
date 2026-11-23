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
set "TFLAGS=/nologo /std:c++17 /O2 /MT /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Fobuild\tests\"
set "MONSRC=src\monitor.cpp src\pinsources.cpp src\nvapi_gpu.cpp src\nvml_gpu.cpp src\wear.cpp src\blackbox.cpp"
cl %TFLAGS% tests\test_panel.cpp src\panel.cpp %MONSRC% /Fe:build\tests\test_panel.exe /link user32.lib crypt32.lib setupapi.lib hid.lib advapi32.lib >nul || (echo BUILD FAILED & exit /b 1)
cl %TFLAGS% tests\test_pins.cpp src\pinsources.cpp /Fe:build\tests\test_pins.exe /link setupapi.lib hid.lib advapi32.lib >nul || (echo BUILD FAILED & exit /b 1)
build\tests\test_pins.exe || exit /b 1
cl %TFLAGS% tests\test_update.cpp src\updater.cpp %MONSRC% /Fe:build\tests\test_update.exe /link winhttp.lib bcrypt.lib setupapi.lib hid.lib advapi32.lib user32.lib >nul || (echo BUILD FAILED & exit /b 1)
build\tests\test_update.exe || exit /b 1
cl %TFLAGS% tests\test_history.cpp src\wear.cpp src\blackbox.cpp /Fe:build\tests\test_history.exe /link advapi32.lib >nul || (echo BUILD FAILED & exit /b 1)
build\tests\test_history.exe || exit /b 1
cl %TFLAGS% tests\test_connector.cpp src\connector.cpp /Fe:build\tests\test_connector.exe >nul || (echo BUILD FAILED & exit /b 1)
build\tests\test_connector.exe || exit /b 1
build\tests\test_panel.exe
