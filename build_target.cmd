@echo off
rem Build one target: build_target.cmd <target> [Release|Debug]
setlocal
set CONFIG=%2
if "%CONFIG%"=="" set CONFIG=Release
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64 >nul || exit /b 1
cmake --build "%~dp0build" --config %CONFIG% --target %1
