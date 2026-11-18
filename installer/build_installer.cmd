@echo off
rem Builds the tool, then the installer and the portable zip: release\tekken-studio-<version>-setup.exe and
rem release\tekken-studio-<version>-portable.zip. Needs Inno Setup 6 (winget install JRSoftware.InnoSetup).
setlocal
cd /d "%~dp0.."
call "%CD%\build.cmd" || exit /b 1
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo Inno Setup 6 isn't installed. Run: winget install JRSoftware.InnoSetup
    exit /b 1
)
"%ISCC%" /Q installer\tekken8mvm.iss || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_portable.ps1 || exit /b 1
echo Release files: release\
dir /b release\tekken-studio-*
