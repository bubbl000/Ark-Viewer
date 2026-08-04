@echo off
REM ArkThumbProvider 缩略图扩展卸载脚本
REM 需以管理员身份运行
cd /d "%~dp0"

set "DLL=build\ArkThumbProvider.dll"

echo Unregistering ArkThumbProvider...
if exist "%DLL%" (
    regsvr32 /u /s "%DLL%"
) else (
    echo [WARN] %DLL% missing, attempting registry cleanup via regsvr32 may fail.
    regsvr32 /u /s "%DLL%" 1>nul 2>&1
)

ie4uinit.exe -show 1>nul 2>&1
echo [OK] Unregistered. Thumbnails will revert to default icons.
pause
