@echo off
echo ==============================================
echo EGoTouchRev - Build ^& Pack Release Version
echo ==============================================

cd /d "%~dp0\.."

set "BUILD_VERSION=%~1"
if "%BUILD_VERSION%"=="" (
    set "BUILD_VERSION=0.1.1"
)

echo.
echo [1/3] Building arm64-Release CMake targets...
cmake --build build/arm64-Release --config Release
if %errorlevel% neq 0 (
    echo [ERROR] CMake build failed.
    exit /b %errorlevel%
)

echo.
echo [2/3] Preparing and building MSI Installer using WiX...
wix build -ext WixToolset.UI.wixext -arch arm64 -d BuildVersion=%BUILD_VERSION% -d BuildOutputDir=build\arm64-Release scripts\EGoTouchSetup.wxs -loc scripts\zh-CN.wxl -out build\EGoTouchSetup.msi
if %errorlevel% neq 0 (
    echo [ERROR] WiX build failed.
    exit /b %errorlevel%
)

echo.
echo [3/3] Build Successful! 
echo Release installer has been generated at: build\EGoTouchSetup.msi
echo Installed Version: %BUILD_VERSION%
echo ==============================================
exit /b 0
