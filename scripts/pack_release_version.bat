@echo off
echo ==============================================
echo EGoTouchRev - Build ^& Pack Release Version
echo ==============================================

cd /d "%~dp0\.."

set "BUILD_VERSION=%~1"
if not "%BUILD_VERSION%"=="" goto :version_ready

REM No version on the command line: read the one in CMakeLists.txt, which the
REM build already cross-checks against Common\include\AppVersion.h. A literal
REM default here goes stale silently and ships an MSI whose ProductVersion does
REM not match the binaries inside it.
for /f "tokens=3" %%v in ('findstr /r /c:"^project(.*VERSION " CMakeLists.txt') do set "BUILD_VERSION=%%v"

if "%BUILD_VERSION%"=="" (
    echo [ERROR] Could not read the project version from CMakeLists.txt.
    exit /b 1
)

:version_ready
echo Packing version %BUILD_VERSION%

REM hal builds separately and first: the main CMakeLists fails configuration when
REM hal\build\Release\GaokunHal.lib is missing, and the MSI packs the hosts from
REM the same directory.
set "HAL_OUTPUT_DIR=hal\build\Release"

echo.
echo [1/4] Building gaokun-hal (Release)...
pwsh -NoProfile -ExecutionPolicy Bypass -File hal\scripts\build.ps1 -Config Release
if %errorlevel% neq 0 (
    echo [ERROR] gaokun-hal build failed.
    exit /b %errorlevel%
)

echo.
REM Go through scripts\build.ps1 rather than calling cmake directly. A bare
REM "cmake --build" needs vcvarsarm64 to have run in the caller's shell already,
REM so from an ordinary terminal it dies on a missing cl.exe while step 1 above
REM has just succeeded -- that script imports the environment itself. build.ps1
REM does the same, pins the arm64-Release preset, and configures when the build
REM directory is not there yet.
echo [2/4] Building arm64-Release CMake targets...
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release
if %errorlevel% neq 0 (
    echo [ERROR] CMake build failed.
    exit /b %errorlevel%
)

echo.
echo [3/4] Preparing and building MSI Installer using WiX...
REM The two extensions are a machine-level prerequisite that nothing in the
REM repository declares otherwise; "add" is idempotent when already installed.
wix extension add -g WixToolset.UI.wixext >nul 2>&1
wix extension add -g WixToolset.Util.wixext >nul 2>&1
wix build -ext WixToolset.UI.wixext -ext WixToolset.Util.wixext -arch arm64 -d BuildVersion=%BUILD_VERSION% -d BuildOutputDir=build\arm64-Release -d HalOutputDir=%HAL_OUTPUT_DIR% scripts\OpenEGoHubSetup.wxs -loc scripts\zh-CN.wxl -out build\OpenEGoHubSetup.msi
if %errorlevel% neq 0 (
    echo [ERROR] WiX build failed.
    exit /b %errorlevel%
)

echo.
echo [4/4] Build Successful!
echo Release installer has been generated at: build\OpenEGoHubSetup.msi
echo Installed Version: %BUILD_VERSION%
echo ==============================================
exit /b 0
