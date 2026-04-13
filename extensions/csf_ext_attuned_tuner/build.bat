@echo off
REM -- Build csf_ext_attuned_tuner.dll --
REM
REM Standalone CSF extension DLL that adds the Tuner / Attuned status pair
REM on top of csf_core.dll.  Depends on csf_core.dll being loaded first
REM (everything is resolved at runtime via GetProcAddress, so there is no
REM build-time dependency -- only csf_core_api.h needs to be on the include
REM path).
REM
REM Usage:
REM   build.bat            (build DLL into this directory)
REM   build.bat deploy     (also copy into game's mods dir)
REM
REM Deploy assumes MEWGENICS_DIR is set, e.g.:
REM   set MEWGENICS_DIR=C:\Program Files (x86)\Steam\steamapps\common\Mewgenics

setlocal

REM -- Locate Visual Studio via vswhere --
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Is Visual Studio installed?
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo ERROR: Could not find a Visual Studio installation.
    pause
    exit /b 1
)

if not exist "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo ERROR: vcvarsall.bat not found at "%VSDIR%\VC\Auxiliary\Build\"
    pause
    exit /b 1
)

echo Setting up x64 MSVC environment...
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

REM -- Build --
pushd "%~dp0"
echo.
echo Building csf_ext_attuned_tuner.dll...
REM /I..\.. so the extension resolves csf_core_api.h from the repo root
REM where the canonical copy of the header lives.  After the directory
REM reorganization there is no duplicate of csf_core_api.h next to the
REM extension source. csf_core_api.h at the repo root is the single source of truth
REM for both the core build and every extension.
cl /nologo /LD /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS ^
    /I..\.. ^
    csf_ext_attuned_tuner.c ^
    /Fe:csf_ext_attuned_tuner.dll

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build FAILED.
    popd
    pause
    exit /b 1
)

echo.
echo Build succeeded: csf_ext_attuned_tuner.dll

REM Clean intermediate files
del /Q *.obj 2>nul
del /Q csf_ext_attuned_tuner.lib csf_ext_attuned_tuner.exp 2>nul
popd

REM -- Deploy (optional) --
if /I "%1"=="deploy" (
    if not defined MEWGENICS_DIR (
        echo.
        echo WARNING: MEWGENICS_DIR not set. Cannot deploy.
        echo Set it to your Mewgenics install directory, e.g.:
        echo   set MEWGENICS_DIR=C:\Program Files ^(x86^)\Steam\steamapps\common\Mewgenics
        pause
        exit /b 1
    )
    set "DEST=%MEWGENICS_DIR%\mods\csf_ext_attuned_tuner"
    if not exist "%DEST%" (
        mkdir "%DEST%"
    )
    copy /Y "%~dp0csf_ext_attuned_tuner.dll" "%DEST%\"
    echo Deployed to %DEST%
)

pause
