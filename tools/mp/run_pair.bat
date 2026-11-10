@echo off
rem Starts a multiplayer test pair on this machine: one host and one client,
rem each with its own save file (host.sav / client.sav).
rem
rem Usage: tools\mp\run_pair.bat [extra game args...]
rem   Extra args are passed to both instances (e.g. --scale 2).
rem
rem Environment overrides (set before running):
rem   EXE       game executable       (default: <repo>\pokemon-spectrum64.exe)
rem   SAVE_DIR  where the saves live  (default: <repo>)
rem   PORT      UDP port              (default: 27500)
rem
rem SDL2.dll must be next to the executable.
rem If host.sav / client.sav don't exist yet and <repo>\pokemon-spectrum.sav does,
rem it is copied to create them. Existing saves are never overwritten.
rem
rem See docs\multiplayer\testing.md.

setlocal
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
if not defined EXE set "EXE=%ROOT%\pokemon-spectrum64.exe"
if not defined SAVE_DIR set "SAVE_DIR=%ROOT%"
if not defined PORT set "PORT=27500"

if not exist "%EXE%" (
    echo Executable not found: %EXE%
    echo Build it with "make winwsl", or set EXE=C:\path\to\pokemon-spectrum64.exe
    exit /b 1
)

if not exist "%SAVE_DIR%" mkdir "%SAVE_DIR%"

if not exist "%SAVE_DIR%\host.sav" if exist "%ROOT%\pokemon-spectrum.sav" (
    echo Creating %SAVE_DIR%\host.sav from %ROOT%\pokemon-spectrum.sav
    copy "%ROOT%\pokemon-spectrum.sav" "%SAVE_DIR%\host.sav" >nul
)
if not exist "%SAVE_DIR%\client.sav" if exist "%ROOT%\pokemon-spectrum.sav" (
    echo Creating %SAVE_DIR%\client.sav from %ROOT%\pokemon-spectrum.sav
    copy "%ROOT%\pokemon-spectrum.sav" "%SAVE_DIR%\client.sav" >nul
)

echo Starting host   (save: %SAVE_DIR%\host.sav, port %PORT%)
start "Spectrum host" "%EXE%" --save "%SAVE_DIR%\host.sav" --host %PORT% --mp-debug %*

rem Give the host a moment to start listening before the client connects.
ping -n 2 127.0.0.1 >nul

echo Starting client (save: %SAVE_DIR%\client.sav, joining 127.0.0.1:%PORT%)
start "Spectrum client" "%EXE%" --save "%SAVE_DIR%\client.sav" --join 127.0.0.1:%PORT% --mp-debug %*

endlocal
