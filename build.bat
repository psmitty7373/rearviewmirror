@echo off
setlocal

set "CFG="
set "TGT="
set "FEATURES="
set "SIGNING="
set "NO_PAUSE="

:args
if "%1"=="" goto configure
if "%~1"=="" (
    if not defined CFG set "CFG=Release"
    shift
    goto args
)
if /i "%~1"=="--help" goto usage
if /i "%~1"=="--remote-control" (
    set "FEATURES=-DRVM_STREAMING=ON -DRVM_REMOTE_CONTROL=ON"
    shift
    goto args
)
if /i "%~1"=="--streaming-only" (
    set "FEATURES=-DRVM_STREAMING=ON -DRVM_REMOTE_CONTROL=OFF"
    shift
    goto args
)
if /i "%~1"=="--mirrors-only" (
    set "FEATURES=-DRVM_STREAMING=OFF -DRVM_REMOTE_CONTROL=OFF"
    shift
    goto args
)
if /i "%~1"=="--no-sign" (
    set "SIGNING=-DRVM_SIGN=OFF"
    shift
    goto args
)
if /i "%~1"=="--no-pause" (
    set "NO_PAUSE=1"
    shift
    goto args
)
set "ARG=%~1"
if "%ARG:~0,1%"=="-" (
    echo Unknown option: %~1
    exit /b 1
)
if not defined CFG (
    set "CFG=%~1"
) else if not defined TGT (
    set "TGT=%~1"
) else (
    echo Unexpected argument: %~1
    exit /b 1
)
shift
goto args

:configure
if not defined CFG set "CFG=Release"

rem Find the newest Visual Studio (any edition, or Build Tools) with the C++
rem tools, falling back to the 2022 Build Tools' usual place.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo Could not find the Visual Studio C++ build tools.
    exit /b 1
)
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo Could not initialize MSVC environment.
    exit /b 1
)

cd /d "%~dp0" || exit /b 1
cmake -G Ninja -S . -B build "-DCMAKE_BUILD_TYPE=%CFG%" %FEATURES% %SIGNING% || exit /b 1
if defined TGT (
    cmake --build build --target "%TGT%" || exit /b 1
) else (
    cmake --build build || exit /b 1
)
echo.
echo Build complete.

if not defined NO_PAUSE pause
exit /b 0

:usage
echo Usage: build.bat [Release^|Debug] [target] [options]
echo.
echo   --remote-control  Build streaming and full-desktop control
echo   --streaming-only  Build streaming without remote control
echo   --mirrors-only    Build local mirrors without networking or the client
echo   --no-sign         Disable executable signing
echo   --no-pause        Exit without waiting for a key
echo.
echo Without a feature option, keep the previous CMake selection.
echo A fresh build defaults to streaming without remote control.
exit /b 0
