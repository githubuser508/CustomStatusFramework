@echo off
REM -- Build csf_core.dll --
REM
REM Compiles the Custom Status Framework Core DLL.  Extensions live in
REM their own DLLs alongside csf_core.dll (see csf_core_api.h).
REM
REM Usage:
REM   build.bat            (build DLL into this directory)
REM   build.bat deploy     (also copy csf_core.dll into game's mods dir)
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
echo Building csf_core.dll...
cl /nologo /LD /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS ^
    csf_core.c ^
    csf_core_gon_loader.c ^
    csf_core_registry.c ^
    DonorVtables.c ^
    GonParser.c ^
    /Fe:csf_core.dll ^
    /link dbghelp.lib

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build FAILED.
    popd
    pause
    exit /b 1
)

echo.
echo Build succeeded: csf_core.dll

REM Clean intermediate files
del /Q *.obj 2>nul
del /Q csf_core.lib csf_core.exp 2>nul
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
    set "DEST=%MEWGENICS_DIR%\mods\csf_core"
    if not exist "%DEST%" (
        mkdir "%DEST%"
    )
    copy /Y "%~dp0csf_core.dll" "%DEST%\"
    echo Deployed to %DEST%
)

pause
