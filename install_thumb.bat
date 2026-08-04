@echo off
REM ArkThumbProvider 缩略图扩展注册脚本
REM 需以管理员身份运行（写 HKCR 需要管理员权限）
cd /d "%~dp0"

set "DLL=build\ArkThumbProvider.dll"
if not exist "%DLL%" (
    echo [ERROR] %DLL% not found, build first.
    pause
    exit /b 1
)

echo Registering ArkThumbProvider...
regsvr32 /s "%DLL%"
if errorlevel 1 (
    echo [FAILED] Registration failed. Run this script as Administrator.
    pause
    exit /b 1
)

REM Refresh shell icon/thumbnail cache
ie4uinit.exe -show 1>nul 2>&1
echo [OK] Registered. Explorer will show ARW/PSD/HEIF/SVG/HDR thumbnails.
echo NOTE: DLL path is recorded in registry - do not move build\ArkThumbProvider.dll.
pause
