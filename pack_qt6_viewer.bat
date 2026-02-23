@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem === PLEASE MODIFY THESE TWO PATHS ===
set "QT_BIN=C:\Qt\6.10.0\msvc2022_64\bin"
set "BUILD_ROOT=D:\Synology_Backup\CaptureCode\Gcapture_qt6_srcroot\build\Desktop_Qt_6_10_0_MSVC2022_64bit-Release"
rem ====================================

set "BIN_DIR=%BUILD_ROOT%\bin"
set "OUT_ROOT=%BUILD_ROOT%\dist"
set "OUT_DIR=%OUT_ROOT%\qt6_viewer_win64"
set "ZIP_OUT=%OUT_ROOT%\qt6_viewer_win64.zip"

echo.
echo ============================================================
echo [0/5] Check environment
echo ============================================================

if not exist "%QT_BIN%\windeployqt.exe" (
    echo ERROR: windeployqt.exe not found: "%QT_BIN%\windeployqt.exe"
    pause
    exit /b 1
)

if not exist "%BIN_DIR%" (
    echo ERROR: BIN_DIR not found: "%BIN_DIR%"
    pause
    exit /b 1
)

if not exist "%BIN_DIR%\qt6_viewer.exe" (
    echo ERROR: qt6_viewer.exe not found: "%BIN_DIR%\qt6_viewer.exe"
    pause
    exit /b 1
)

echo.
echo ============================================================
echo [1/5] Prepare output folder
echo ============================================================

if exist "%OUT_DIR%" (
    echo Removing old: "%OUT_DIR%"
    rmdir /s /q "%OUT_DIR%"
)

mkdir "%OUT_DIR%"
if errorlevel 1 (
    echo ERROR: Failed to create: "%OUT_DIR%"
    pause
    exit /b 1
)

echo.
echo ============================================================
echo [2/5] Copy application binaries (exe + your dlls)
echo ============================================================

copy /y "%BIN_DIR%\qt6_viewer.exe" "%OUT_DIR%\" >nul

for %%F in (gcapture.dll gdisplay.dll CaptureSDK.dll) do (
    if exist "%BIN_DIR%\%%F" (
        echo Copy %%F
        copy /y "%BIN_DIR%\%%F" "%OUT_DIR%\" >nul
    )
)

echo.
echo ============================================================
echo [3/5] Run windeployqt
echo ============================================================

pushd "%OUT_DIR%"
"%QT_BIN%\windeployqt.exe" --release --compiler-runtime --no-translations "qt6_viewer.exe"
if errorlevel 1 (
    popd
    echo ERROR: windeployqt failed.
    pause
    exit /b 1
)
popd

echo.
echo ============================================================
echo [4/5] Create zip
echo ============================================================

if exist "%ZIP_OUT%" del /f /q "%ZIP_OUT%"

powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%OUT_DIR%\*' -DestinationPath '%ZIP_OUT%' -Force"
if errorlevel 1 (
    echo ERROR: Compress-Archive failed. (Need PowerShell 5+)
    pause
    exit /b 1
)

echo.
echo ============================================================
echo [5/5] Done
echo ============================================================
echo Output folder: "%OUT_DIR%"
echo Zip         : "%ZIP_OUT%"
echo.
pause
endlocal
