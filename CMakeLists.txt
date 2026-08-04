cmake_minimum_required(VERSION 3.20)
project(ArkViewer2 VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")

# D2D requirement: Windows 10+ SDK




# 鈹€鈹€ Sources 鈹€鈹€
file(GLOB_RECURSE SOURCES
    src/*.cpp
    src/*.h
)

# miniz (C library for ZIP/Deflate decompression, used by PSD/SVGZ)
set(MINIZ_SOURCES
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_tdef.c
)

# 鈹€鈹€ Main executable 鈹€鈹€
add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN)

add_executable(ArkViewer2 WIN32
    ${SOURCES}
    ${MINIZ_SOURCES}
    resources/resource.rc
)

# MSVC: 源文件为 UTF-8（含中文注释），需显式声明避免按 GBK 解码导致语法错误
# /Zi + /DEBUG：Release 亦生成 PDB，便于定位卡顿点（体积增加可接受）
if(MSVC)
    target_compile_options(ArkViewer2 PRIVATE /utf-8 /Zi)
    target_link_options(ArkViewer2 PRIVATE /DEBUG)
endif()

target_include_directories(ArkViewer2 PRIVATE
    src
    include
    ${CMAKE_CURRENT_SOURCE_DIR}
    third_party/miniz
)

# 鈹€鈹€ Windows SDK + D2D 鈹€鈹€
target_link_libraries(ArkViewer2 PRIVATE
    d3d11
    d2d1
    
    dwrite
    dxgi
    windowscodecs
    ole32
    oleaut32
    shell32
    shlwapi
    comctl32
    gdi32
  kernel32
    user32
    dwmapi
    uuid
)

# JpegDecoder/WebpDecoder 运行时 LoadLibrary 动态加载 turbojpeg.dll/libwebp.dll，无需链接 .lib
# PngDecoder 使用 WIC 解码，不依赖 libpng

# ─── Option: NVIDIA nvJPEG 硬件解码（动态加载，不链接 .lib，无 CUDA 机器零依赖） ───
option(USE_NVJPEG "Build with NVIDIA nvJPEG hardware decode" ON)
if(USE_NVJPEG)
    set(CUDA_PATH "$ENV{CUDA_PATH}" CACHE PATH "CUDA Toolkit root")
    if(EXISTS "${CUDA_PATH}/bin/nvjpeg64_12.dll")
        target_compile_definitions(ArkViewer2 PRIVATE HAS_NVJPEG)
        # 运行时 DLL 拷贝到输出目录（开发期调试 + install 分发）
        foreach(DLL nvjpeg64_12.dll cudart64_12.dll)
            add_custom_command(TARGET ArkViewer2 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CUDA_PATH}/bin/${DLL}" "$<TARGET_FILE_DIR:ArkViewer2>"
                COMMENT "Copy ${DLL}")
        endforeach()
        install(FILES
            "${CUDA_PATH}/bin/nvjpeg64_12.dll"
            "${CUDA_PATH}/bin/cudart64_12.dll"
            DESTINATION .)
    else()
        message(STATUS "CUDA Toolkit 未找到 nvjpeg64_12.dll (CUDA_PATH=${CUDA_PATH})，nvJPEG 硬解禁用")
        set(USE_NVJPEG OFF CACHE BOOL "" FORCE)
    endif()
endif()

# ─── Install + DLL 拷贝 ───
install(TARGETS ArkViewer2 RUNTIME DESTINATION .)

# 第三方解码 DLL 源目录：turbojpeg/libwebp 编译产物在 ../临时/，libheif/libraw 在 third_party/
# decoder 运行时 LoadLibrary 加载这些 DLL，需拷贝到 exe 旁
set(THIRD_PARTY_DLL_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../临时/libjpeg-turbo-main/build_simd"
    "${CMAKE_CURRENT_SOURCE_DIR}/../临时/libwebp-main/build_mt"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libheif/bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libraw/bin"
)
foreach(DLL_DIR ${THIRD_PARTY_DLL_DIRS})
    file(GLOB DLLS "${DLL_DIR}/*.dll")
    foreach(DLL ${DLLS})
        install(FILES "${DLL}" DESTINATION .)
        add_custom_command(TARGET ArkViewer2 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLL}" "$<TARGET_FILE_DIR:ArkViewer2>"
        )
    endforeach()
endforeach()

# 鈹€鈹€ ArkThumbProvider: 资源管理器缩略图 COM DLL（独立 SHARED，不依赖主程序） 鈹€鈹€
# 复用主程序解码器源码，ARK_THUMB_DLL 宏隔离 ActivityLog/nvJPEG 等主程序独有依赖
# 第三方库（libraw/heif/libde265）均运行时 LoadLibrary 动态加载，链接期零三方依赖
add_library(ArkThumbProvider SHARED
    src_thumb/DllMain.cpp
    src_thumb/ClassFactory.cpp
    src_thumb/ThumbProvider.cpp
    src_thumb/ThumbExtractor.cpp
    src_thumb/ArkThumbProvider.def
    src/decoders/RawDecoder.cpp
    src/decoders/PsdDecoder.cpp
    src/decoders/HeifDecoder.cpp
    src/decoders/SvgDecoder.cpp
    src/decoders/HdrDecoder.cpp
    src/ImageDecoder.cpp
    src/FileMapping.cpp
    ${MINIZ_SOURCES}
)
target_compile_definitions(ArkThumbProvider PRIVATE
    ARK_THUMB_DLL NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(ArkThumbProvider PRIVATE
    src include third_party/miniz)
if(MSVC)
    # /utf-8：源文件含中文注释；/Zi：生成 PDB 便于调试
    # SEH 访问违规防护由 GetThumbnail 内 __try/__except 提供，无需 /EHa
    target_compile_options(ArkThumbProvider PRIVATE /utf-8 /Zi)
endif()
target_link_libraries(ArkThumbProvider PRIVATE
    ole32 oleaut32 uuid shell32 shlwapi windowscodecs
    kernel32 user32 gdi32 d2d1 d3d11 dxgi dwrite)

# 拷贝运行时依赖 DLL 到 ArkThumbProvider 输出目录（与主程序共用 build/，确保 dllhost 能定位）
foreach(THUMB_DLL_DIR third_party/libraw/bin third_party/libheif/bin)
    file(GLOB THUMB_DLLS "${THUMB_DLL_DIR}/*.dll")
    foreach(DLL ${THUMB_DLLS})
        add_custom_command(TARGET ArkThumbProvider POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLL}" "$<TARGET_FILE_DIR:ArkThumbProvider>")
    endforeach()
endforeach()







