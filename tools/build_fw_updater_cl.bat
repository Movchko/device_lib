@echo off
setlocal EnableExtensions EnableDelayedExpansion

set SRC=fw_updater_gui.c
set OUT=fw_updater_gui.exe

rem --- Try to initialize MSVC toolchain automatically if cl is missing ---
where cl >nul 2>&1
if errorlevel 1 (
    echo [*] cl.exe not found in PATH, trying to load Visual Studio environment...

    set "VCVARS_BAT="
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

    if exist "%VSWHERE%" (
        for /f "usebackq delims=" %%I in (`
            "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        `) do (
            if exist "%%~I\VC\Auxiliary\Build\vcvars64.bat" (
                set "VCVARS_BAT=%%~I\VC\Auxiliary\Build\vcvars64.bat"
            )
        )
    )

    if not defined VCVARS_BAT (
        for %%V in (2022 2019) do (
            for %%E in (BuildTools Community Professional Enterprise) do (
                if not defined VCVARS_BAT if exist "%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat" (
                    set "VCVARS_BAT=%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
                )
            )
        )
    )

    if not defined VCVARS_BAT (
        echo [X] Could not find vcvars64.bat.
        echo [X] Install Visual Studio Build Tools with C++ workload.
        exit /b 1
    )

    call "!VCVARS_BAT!" >nul
    if errorlevel 1 (
        echo [X] Failed to initialize VS environment: !VCVARS_BAT!
        exit /b 1
    )
)

where cl >nul 2>&1
if errorlevel 1 (
    echo [X] cl.exe still not available after VS environment init.
    exit /b 1
)

echo Compiling %SRC% ...
cl /nologo /EHsc /utf-8 /DUNICODE /D_UNICODE /W3 "%SRC%" /link /SUBSYSTEM:WINDOWS /OUT:"%OUT%" user32.lib gdi32.lib comctl32.lib comdlg32.lib

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Build succeeded: %OUT%
endlocal

