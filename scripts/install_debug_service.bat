@echo off
:: EGoTouchService Debug Install Script
:: Installs from current directory
:: Requires Administrator privileges

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Administrator privileges required. Right-click "Run as administrator".
    pause
    exit /b 1
)

echo === EGoTouchService Debug Install ===
echo.
set "BUILD_PRESET=arm64-Debug"
if not "%~1"=="" (
    echo [ERROR] Usage: install_debug_service.bat
    pause
    exit /b 2
)
set "SERVICE_BIN=%~dp0..\build\%BUILD_PRESET%\OpenEGoHubService.exe"
if not exist "%SERVICE_BIN%" (
    echo [ERROR] Service binary not found:
    echo         %SERVICE_BIN%
    echo         Build the matching preset before installing the debug service.
    pause
    exit /b 1
)
echo Preset: %BUILD_PRESET%
echo Binary: %SERVICE_BIN%
echo.

:: Stop and remove old debug service if exists
sc query OpenEGoHubServiceDebug >nul 2>&1
if %errorlevel% equ 0 (
    echo [INFO] Stopping existing debug service...
    sc stop OpenEGoHubServiceDebug >nul 2>&1
    timeout /t 3 /nobreak >nul
    echo [INFO] Removing existing debug service...
    sc delete OpenEGoHubServiceDebug >nul 2>&1
    timeout /t 2 /nobreak >nul
)

:: Create data directory
if not exist "C:\ProgramData\OpenEGoHub" mkdir "C:\ProgramData\OpenEGoHub"
if not exist "C:\ProgramData\OpenEGoHub\logs" mkdir "C:\ProgramData\OpenEGoHub\logs"

:: Install service pointing to debug build directory (Manual start)
sc create OpenEGoHubServiceDebug binPath= "%SERVICE_BIN%" start= demand
if %errorlevel% neq 0 (
    echo [ERROR] Failed to create service.
    pause
    exit /b 1
)

:: No failure recovery. A development service that resurrects itself hides the
:: failure it should be showing. Worse, a hung service stops answering the SCM
:: (sc stop returns 1061) and force-killing is the only way out -- with restart
:: actions configured the SCM brings it back five seconds later, and it and the
:: vendor driver take turns claiming the touch controller so neither gets it.
:: The shipping service has its own installer and is unaffected.
::
:: Keep this file ASCII-only: cmd.exe reads .bat in the OEM code page, and
:: non-ASCII comment bytes can decode into characters that terminate the
:: comment, leaving the rest of the line to run as a command.
sc failure OpenEGoHubServiceDebug reset= 0 actions= ""

:: Description
sc description OpenEGoHubServiceDebug "EGoTouch Capacitive Touch Controller Driver Service (Debug)"

:: Start the service is removed for debug install, user needs to start it manually or attach debugger first
echo [INFO] Debug service installed. You can start it manually or attach a debugger.

sc query OpenEGoHubServiceDebug | findstr STATE
echo.
echo [OK] Debug Install complete. Service registered as OpenEGoHubServiceDebug (Manual Start) from:
echo     %SERVICE_BIN%
echo.
pause
