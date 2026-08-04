#include "PngDecoder.h"
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static IWICImagingFactory* WIC() {
    static ComPtr<IWICImagingFactory> f;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f));
    return f.Get();
}

std::optional<ImageDecoder::OpenResult> PngDecoder::Open(const uint8_t* d, size_t l) {
    const uint8_t sig[]={0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    if(l<8||memcmp(d,sig,8)!=0) return {};
    if(l<33) return {};
    int w=(d[16]<<24)|(d[17]<<16)|(d[18]<<8)|d[19];
    int h=(d[20]<<24)|(d[21]<<16)|(d[22]<<8)|d[23];
    if(w<=0||h<=0) return {};
    OpenResult r; r.info.width=w; r.info.height=h;
    r.info.format="PNG"; r.info.decoderName="WIC/libpng";
    return r;
}

// 用 WIC 解码 PNG 文件
extern "C" __declspec(dllexport)
bool PngDecodeFile(const wchar_t* path, uint8_t** out, int* w, int* h, int* stride) {
    auto* wic=WIC(); if(!wic) return false;
    ComPtr<IWICBitmapDecoder> dec;
    HRESULT hr=wic->CreateDecoderFromFilename(path,nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&dec);
    if(FAILED(hr)) return false;
    ComPtr<IWICBitmapFrameDecode> f; hr=dec->GetFrame(0,&f); if(FAILED(hr)) return false;
    UINT fw=0,fh=0; f->GetSize(&fw,&fh);
    ComPtr<IWICFormatConverter> cv; hr=wic->CreateFormatConverter(&cv); if(FAILED(hr)) return false;
    hr=cv->Initialize(f.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    if(FAILED(hr)) return false;
    *w=(int)fw; *h=(int)fh; *stride=fw*4;
    *out=(uint8_t*)malloc((size_t)(*stride)*(*h));
    cv->CopyPixels(nullptr,*stride,(UINT)(*stride)*(*h),*out);
    return true;
}

std::optional<DecodeResult> PngDecoder::DecodeFull(const OpenResult&) { return {}; }
std::optional<DecodeResult> PngDecoder::DecodeTile(const OpenResult&,int,int,int){return {};}
std::optional<DecodeResult> PngDecoder::DecodeLevel(const OpenResult&,int){return {};}
