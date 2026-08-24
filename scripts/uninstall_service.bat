@echo off
:: EGoTouchService Uninstall Script
:: Requires Administrator privileges

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Administrator privileges required. Right-click "Run as administrator".
    pause
    exit /b 1
)

echo === EGoTouchService Uninstall ===
echo.

:: Stop service
echo [INFO] Stopping service...
sc stop EGoTouchService >nul 2>&1
timeout /t 3 /nobreak >nul

:: Kill any lingering process
taskkill /F /IM OpenEGoHubService.exe >nul 2>&1

:: Delete service
sc delete EGoTouchService
if %errorlevel% equ 0 (
    echo [OK] Service uninstalled successfully.
) else (
    echo [WARN] Uninstall may require a reboot to take effect.
)

echo.
echo Note: C:\ProgramData\OpenEGoHub\ directory and contents were NOT removed.
echo.
pause
