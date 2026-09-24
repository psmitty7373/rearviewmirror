@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo Could not initialize MSVC environment. & exit /b 1)
cd /d "%~dp0"
set CFG=%1
if "%CFG%"=="" set CFG=Release
set TGT=
if not "%2"=="" set TGT=--target %2
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=%CFG% || exit /b 1
cmake --build build %TGT% || exit /b 1
echo.
echo Build complete.
