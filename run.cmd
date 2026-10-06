@echo off
rem Builds (if needed) and starts the app.
rem Usage: run.cmd [Debug|Release]   (default: Debug)
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

call "%~dp0build.cmd" %CONFIG% || (
    echo Build failed.
    pause
    exit /b 1
)

start "" "%~dp0build\%CONFIG%\OrchestralDAW_artefacts\%CONFIG%\Daw+.exe"
