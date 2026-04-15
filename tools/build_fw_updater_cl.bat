@echo off
setlocal

set SRC=fw_updater_gui.c
set OUT=fw_updater_gui.exe

echo Compiling %SRC% ...
cl /nologo /EHsc /utf-8 /DUNICODE /D_UNICODE /W3 "%SRC%" /link /SUBSYSTEM:WINDOWS /OUT:%OUT% user32.lib gdi32.lib comctl32.lib comdlg32.lib

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Build succeeded: %OUT%
endlocal

