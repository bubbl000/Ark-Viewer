// DecodeBridge.cpp — C++/CLI 桥接项目
// 引用原 C# 解码器 DLL，暴露 C 接口给主项目
//
// 在 VS 中建新项目: 添加 → 新建项目 → C++ → CLR → 类库 (.NET Framework)
// 命名 DecodeBridge，引用原 C# DLL

#pragma once

#using "..\..\Ark Viewer方舟图片浏览器\src\Ghde.Adapters\Ghde.Jpeg\bin\Debug\net10.0\Ghde.Jpeg.dll"
#using "..\..\Ark Viewer方舟图片浏览器\src\Ghde.Adapters\Ghde.Webp\bin\Debug\net10.0\Ghde.Webp.dll"
#using "..\..\Ark Viewer方舟图片浏览器\src\Ghde.Adapters\Ghde.Psd\bin\Debug\net10.0\Ghde.Psd.dll"
#using "..\..\Ark Viewer方舟图片浏览器\src\Ghde.Adapters\Ghde.Raw\bin\Debug\net10.0\Ghde.Raw.dll"
#using "..\..\Ark Viewer方舟图片浏览器\src\Ghde.Adapters\Ghde.Heif\bin\Debug\net10.0\Ghde.Heif.dll"
#using "..\..\Ark Viewer方舟图片浏览器\src\Ghde.Adapters\Ghde.Hdr\bin\Debug\net10.0\Ghde.Hdr.dll"
#using "..\..\Ark Viewer方舟图片浏览器\src\Ghde.Adapters\Ghde.Svg\bin\Debug\net10.0\Ghde.Svg.dll"

using namespace System;
using namespace System::Runtime::InteropServices;

struct DecodeResultC { int w,h,stride; unsigned char* pixels; };

extern "C" __declspec(dllexport)
bool Bridge_Decode(const wchar_t* path, DecodeResultC* out) {
    // 自动识别格式并解码
    String^ p = gcnew String(path);
    String^ ext = IO::Path::GetExtension(p)->ToLower();

    try {
        Ghde::Interfaces::DecodeResult^ result = nullptr;

        if (ext == ".jpg" || ext == ".jpeg") {
            auto dec = gcnew Ghde::Jpeg::JpegDecoder();
            result = dec->Decode(path);
        }
        else if (ext == ".png") {
            auto dec = gcnew Ghde::Wic::WicDecoder();
            result = dec->Decode(path);
        }
        else if (ext == ".webp") {
            auto dec = gcnew Ghde::Webp::WebpDecoder();
            result = dec->Decode(path);
        }
        else if (ext == ".psd") {
            auto dec = gcnew Ghde::Psd::PsdDecoder();
            result = dec->Decode(path);
        }
        else if (ext == ".raw" || ext == ".dng" || ext == ".cr2" || ext == ".nef") {
            auto dec = gcnew Ghde::Raw::RawDecoder();
            result = dec->Decode(path);
        }
        else if (ext == ".heic" || ext == ".heif" || ext == ".avif") {
            auto dec = gcnew Ghde::Heif::HeifDecoder();
            result = dec->Decode(path);
        }
        else if (ext == ".hdr") {
            auto dec = gcnew Ghde::Hdr::HdrDecoder();
            result = dec->Decode(path);
        }
        else if (ext == ".svg") {
            auto dec = gcnew Ghde::Svg::SvgDecoder();
            result = dec->Decode(path);
        }
        else return false;

        if (result == nullptr) return false;

        out->w = result->Width;
        out->h = result->Height;
        out->stride = result->Stride;

        // 从 managed byte[] 拷贝到 native uint8_t*
        array<unsigned char>^ pixels = result->Pixels;
        out->pixels = (unsigned char*)malloc(pixels->Length);
        Marshal::Copy(pixels, 0, IntPtr(out->pixels), pixels->Length);
        return true;
    }
    catch (...) { return false; }
}
