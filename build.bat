@echo off
chcp 65001 >nul
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "%~dp0"

echo === 清理旧构建目录 ===
if exist "build" rmdir /s /q "build"

echo === 配置 CMake (Release) ===
cmake -B build -G Ninja -DCMAKE_MAKE_PROGRAM="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% NEQ 0 goto error

echo === 构建 ===
cmake --build build
if %ERRORLEVEL% NEQ 0 goto error

echo ====== 构建完成 ======
echo.
echo 程序位置: %~dp0build\ArkViewer2.exe
echo 运行日志: %LOCALAPPDATA%\ArkViewer2\logs\
echo.
echo 提示: 解码 DLL (turbojpeg/libwebp/libraw/heif) 已由 CMake POST_BUILD
echo       自动从 third_party\<库>\bin 拷贝到 build\ 目录，无需手动复制。
echo.
pause
goto :eof

:error
echo ====== 构建失败 ======
pause
