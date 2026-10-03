@echo off
rem Build XP Code: compile with Tiny C Compiler, then embed icon, version info and manifest.
rem
rem   build.bat [path\to\tcc.exe]
rem
rem Needs Tiny C Compiler 0.9.27 with the "winapi-full" headers, and Python 3.4+ (for the
rem resource step). If no compiler path is given, tcc.exe is taken from PATH.
setlocal
cd /d "%~dp0"

set TCC=%~1
if "%TCC%"=="" for %%i in (tcc.exe) do set TCC=%%~$PATH:i
if "%TCC%"=="" if exist "C:\dev\tools\tcc\tcc.exe" set TCC=C:\dev\tools\tcc\tcc.exe
if "%TCC%"=="" (
    echo Tiny C Compiler not found. Put tcc.exe on PATH or run: build.bat C:\path\to\tcc.exe
    exit /b 1
)

if not exist build\lib mkdir build\lib

rem Import definitions for system DLLs that TCC does not ship.
for %%d in (comctl32 comdlg32 shell32 ole32) do (
    if not exist build\lib\%%d.def "%TCC%" -impdef %SystemRoot%\system32\%%d.dll -o build\lib\%%d.def
)

rem A running build\xpcode.exe cannot be overwritten but can be renamed, so XP Code can rebuild
rem itself from its own terminal. Move the old copy to %TEMP% (same drive, so it is just a rename).
del /q "%TEMP%\xpcode-*.old" >NUL 2>&1
if exist build\xpcode.exe move /y build\xpcode.exe "%TEMP%\xpcode-%RANDOM%.old" >NUL

echo Compiling XP Code...
"%TCC%" -Wl,-subsystem=windows ^
    src\main.c src\editor.c src\explorer.c src\terminal.c src\palette.c ^
    build\lib\comctl32.def build\lib\comdlg32.def build\lib\shell32.def build\lib\ole32.def ^
    -o build\xpcode.exe
if errorlevel 1 goto failed

echo Embedding resources...
python tools\embed_resources.py build\xpcode.exe
if errorlevel 1 goto failed

echo Build succeeded: %CD%\build\xpcode.exe
exit /b 0

:failed
echo Build FAILED
exit /b 1
