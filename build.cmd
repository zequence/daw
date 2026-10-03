@echo off
rem Configures and builds with MSVC + Ninja (bundled with Visual Studio / Build Tools).
rem Usage: build.cmd [Debug|Release]   (default: Release)
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if "%VSINSTALL%"=="" (
    echo Visual Studio C++ build tools not found.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist "%~dp0external\JUCE\CMakeLists.txt" git -C "%~dp0." submodule update --init || exit /b 1

cmake -S "%~dp0." -B "%~dp0build\%CONFIG%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build "%~dp0build\%CONFIG%" || exit /b 1

echo.
echo Built: %~dp0build\%CONFIG%\OrchestralDAW_artefacts\%CONFIG%\Orchestral DAW.exe
