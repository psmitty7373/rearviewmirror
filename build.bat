@echo off
setlocal

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
    goto done
)
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo Could not initialize MSVC environment.
    goto done
)

cd /d "%~dp0"
set CFG=%1
if "%CFG%"=="" set CFG=Release
set TGT=
if not "%2"=="" set TGT=--target %2
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=%CFG% || exit /b 1
cmake --build build %TGT% || exit /b 1
echo.
echo Build complete.

:done
pause
