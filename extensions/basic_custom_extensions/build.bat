@echo off
REM -- Build basic_custom_extensions.dll --
REM
REM Official behavior pack shipped alongside csf_core.dll.  Contains
REM reusable slot behaviors a modder can attach to their own GON-declared
REM statuses without writing any C code.  Depends on csf_core.dll being
REM loaded first (everything is resolved at runtime via GetProcAddress,
REM so there is no build-time dependency -- only csf_core_api.h needs to
REM be on the include path).
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
echo Building basic_custom_extensions.dll...
REM /I..\.. so the extension resolves csf_core_api.h from the repo root
REM where the canonical copy of the header lives.
cl /nologo /LD /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS ^
    /I..\.. ^
    basic_custom_extensions.c ^
    /Fe:basic_custom_extensions.dll

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build FAILED.
    popd
    pause
    exit /b 1
)

echo.
echo Build succeeded: basic_custom_extensions.dll

REM Clean intermediate files
del /Q *.obj 2>nul
del /Q basic_custom_extensions.lib basic_custom_extensions.exp 2>nul
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
    set "DEST=%MEWGENICS_DIR%\mods\basic_custom_extensions"
    if not exist "%DEST%" (
        mkdir "%DEST%"
    )
    copy /Y "%~dp0basic_custom_extensions.dll" "%DEST%\"
    echo Deployed to %DEST%
)

pause
