@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "%~dp0"

echo === ????? ===
if exist "build" rmdir /s /q "build"

echo === ?? Release ===
cmake -B build -G Ninja -DCMAKE_MAKE_PROGRAM="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -DCMAKE_CXX_COMPILER=cl.exe -DUSE_TURBOJPEG=OFF -DUSE_LIBWEBP=OFF -DUSE_LIBPNG=OFF -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% NEQ 0 goto error

echo === ?? ===
cmake --build build
if %ERRORLEVEL% NEQ 0 goto error

echo ====== ???? ======
echo.
echo === ????? DLL ===
if not exist "build" mkdir "build"
copy /y "E:\06-xiangmu\???2\Ark Novel????\Ark Viewer???????\src\Ghde.Adapters\Ghde.Jpeg\bin\Debug\net10.0\win-x64\turbojpeg.dll" "build\" >nul 2>&1 && echo   turbojpeg.dll ???
copy /y "E:\06-xiangmu\???2\Ark Novel????\Ark Viewer???????\src\Ghde.Adapters\Ghde.Webp\bin\Debug\net10.0\win-x64\libwebp.dll" "build\" >nul 2>&1 && echo   libwebp.dll ???
copy /y "E:\06-xiangmu\???2\Ark Novel????\Ark Viewer???????\src\Ghde.Adapters\Ghde.Raw\bin\Debug\net10.0\win-x64\libraw.dll" "build\" >nul 2>&1 && echo   libraw.dll ???
copy /y "E:\06-xiangmu\???2\Ark Novel????\Ark Viewer???????\src\Ghde.Adapters\Ghde.Heif\bin\Debug\net10.0\win-x64\heif.dll" "build\" >nul 2>&1 && echo   heif.dll ???
copy /y "E:\06-xiangmu\???2\Ark Novel????\Ark Viewer???????\src\Ghde.Adapters\Ghde.Heif\bin\Debug\net10.0\win-x64\libde265.dll" "build\" >nul 2>&1 && echo   libde265.dll ???
copy /y "E:\06-xiangmu\???2\Ark Novel????\Ark Viewer???????\src\Ghde.Adapters\Ghde.Webp\bin\Debug\net10.0\win-x64\libwebpdemux.dll" "build\" >nul 2>&1 && echo   libwebpdemux.dll ???
copy /y "E:\06-xiangmu\???2\Ark Novel????\Ark Viewer???????\src\Ghde.Adapters\Ghde.Webp\bin\Debug\net10.0\win-x64\libsharpyuv.dll" "build\" >nul 2>&1 && echo   libsharpyuv.dll ???
echo.
echo ???: %~dp0build\ArkViewer2.exe
echo ??: %%LOCALAPPDATA%%\ArkViewer2\logs\
echo.
pause
goto :eof

:error
echo ====== ???? ======
pause
