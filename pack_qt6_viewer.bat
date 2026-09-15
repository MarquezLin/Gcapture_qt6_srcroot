@echo off
setlocal EnableExtensions

set "PROJECT_DIR=%~dp0"
set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"

if not defined SOURCE_BIN set "SOURCE_BIN=%PROJECT_DIR%\build\Desktop_Qt_6_10_2_MSVC2022_64bit-Release\bin"
if not defined QT_BIN set "QT_BIN=C:\Qt\6.10.2\msvc2022_64\bin"
if not defined VSDEVCMD set "VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"

if "%~1"=="" (
    set "OUTPUT_DIR=%PROJECT_DIR%\packages"
) else (
    set "OUTPUT_DIR=%~f1"
)

for /f %%I in ('powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Date -Format yyyyMMdd_HHmm"') do set "STAMP=%%I"
for /f "tokens=3" %%I in ('findstr /C:"project(win_capture_sdk VERSION" "%PROJECT_DIR%\CMakeLists.txt"') do set "PACKAGE_VERSION=%%I"

if not defined PACKAGE_VERSION (
    echo [package] ERROR: Project version not found in CMakeLists.txt.
    goto fail
)

set "PACKAGE_NAME=GIGA_Utility_%STAMP%_%PACKAGE_VERSION%"
set "ZIP_PATH=%OUTPUT_DIR%\%PACKAGE_NAME%.zip"
set "STAGE_ROOT=%TEMP%\%PACKAGE_NAME%_%RANDOM%"
set "STAGE_DIR=%STAGE_ROOT%\%PACKAGE_NAME%"
set "WINDEPLOYQT=%QT_BIN%\windeployqt.exe"
set "FFMPEG_BIN=%PROJECT_DIR%\third_party\ffmpeg\bin"
set "EDID_EXE=%PROJECT_DIR%\third_party\edid-decode\vs\x64\Release\edid-decode.exe"
echo [package] Configuration: Release
echo [package] Source: "%SOURCE_BIN%"
echo [package] Output: "%ZIP_PATH%"

for %%F in (qt6_viewer.exe gcapture.dll gdisplay.dll gvfg.dll gvfg_preview.dll) do (
    if not exist "%SOURCE_BIN%\%%F" (
        echo [package] ERROR: Required application file not found: %%F
        goto fail
    )
)
if not exist "%WINDEPLOYQT%" (
    echo [package] ERROR: "%WINDEPLOYQT%" not found.
    goto fail
)
if not exist "%VSDEVCMD%" (
    echo [package] ERROR: "%VSDEVCMD%" not found.
    goto fail
)
if not exist "%FFMPEG_BIN%\ffmpeg.exe" (
    echo [package] ERROR: FFmpeg executable not found: "%FFMPEG_BIN%\ffmpeg.exe"
    goto fail
)
if not exist "%EDID_EXE%" (
    echo [package] ERROR: edid-decode.exe not found: "%EDID_EXE%"
    goto fail
)

echo [package] Check FFmpeg release license flags...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$v = & '%FFMPEG_BIN%\ffmpeg.exe' -version 2>$null; $cfg = ($v | Select-String '^configuration:').Line; if ($cfg -match '--enable-gpl|--enable-nonfree') { Write-Host '[package] ERROR: FFmpeg build uses GPL/nonfree options.'; exit 2 }"
if errorlevel 1 goto fail

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%" || goto fail
if exist "%STAGE_ROOT%" rmdir /S /Q "%STAGE_ROOT%" || goto fail
mkdir "%STAGE_DIR%" || goto fail

echo [package] Copy application files...
for %%F in (qt6_viewer.exe gcapture.dll gdisplay.dll gvfg.dll gvfg_preview.dll) do (
    copy /Y "%SOURCE_BIN%\%%F" "%STAGE_DIR%\" >nul || goto fail
)
copy /Y "%FFMPEG_BIN%\*.dll" "%STAGE_DIR%\" >nul || goto fail
copy /Y "%EDID_EXE%" "%STAGE_DIR%\" >nul || goto fail

echo [package] Deploy Qt runtime...
call "%VSDEVCMD%" -arch=x64 >nul || goto fail
"%WINDEPLOYQT%" --release --compiler-runtime --force --no-translations --dir "%STAGE_DIR%" "%STAGE_DIR%\qt6_viewer.exe"
if errorlevel 1 goto fail

echo [package] Deploy MSVC runtime...
if not defined VCToolsRedistDir (
    echo [package] ERROR: VCToolsRedistDir was not set by VsDevCmd.bat.
    goto fail
)
set "VC_RUNTIME_DIR=%VCToolsRedistDir%x64\Microsoft.VC143.CRT"
if not exist "%VC_RUNTIME_DIR%\msvcp140.dll" (
    echo [package] ERROR: MSVC runtime not found in "%VC_RUNTIME_DIR%".
    goto fail
)
copy /Y "%VC_RUNTIME_DIR%\*.dll" "%STAGE_DIR%\" >nul || goto fail

for %%F in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do (
    if not exist "%STAGE_DIR%\%%F" (
        echo [package] ERROR: Required MSVC runtime was not deployed: %%F
        goto fail
    )
)

echo [package] Create zip...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Compress-Archive -Path '%STAGE_DIR%' -DestinationPath '%ZIP_PATH%' -Force"
if errorlevel 1 goto fail

rmdir /S /Q "%STAGE_ROOT%"

echo [package] Done.
echo [package] Zip: "%ZIP_PATH%"
if "%NO_PAUSE%"=="" pause
exit /b 0

:fail
echo [package] FAILED.
if defined STAGE_ROOT if exist "%STAGE_ROOT%" rmdir /S /Q "%STAGE_ROOT%" >nul 2>nul
if "%NO_PAUSE%"=="" pause
exit /b 1
