#include "AnimationPlayer.h"
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

AnimationPlayer::AnimationPlayer() = default;
AnimationPlayer::~AnimationPlayer() = default;

bool AnimationPlayer::Open(const uint8_t* d, size_t l, const std::string& ext) {
    _frames.clear(); _currentFrame = 0;
    std::string e = ext; for (auto& c : e) c = (char)tolower(c);
    if (e == ".gif") return LoadGif(d, l);
    return false;
}

bool AnimationPlayer::LoadGif(const uint8_t* d, size_t l) {
    auto* wic = WIC(); if (!wic) return false;
    ComPtr<IWICStream> s; HRESULT hr = wic->CreateStream(&s);
    if (FAILED(hr)) return false;
    hr = s->InitializeFromMemory((uint8_t*)d, (DWORD)l);
    if (FAILED(hr)) return false;
    ComPtr<IWICBitmapDecoder> dec;
    hr = wic->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) return false;
    UINT fc = 0; dec->GetFrameCount(&fc);
    if (fc < 2) return false;
    for (UINT i = 0; i < fc; i++) {
        ComPtr<IWICBitmapFrameDecode> f;
        hr = dec->GetFrame(i, &f); if (FAILED(hr)) continue;
        UINT fw=0, fh=0; f->GetSize(&fw, &fh);
        ComPtr<IWICFormatConverter> cv;
        hr = wic->CreateFormatConverter(&cv); if (FAILED(hr)) continue;
        hr = cv->Initialize(f.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) continue;
        AnimationFrame af; af.width=(int)fw; af.height=(int)fh; af.stride=fw*4;
        af.pixels.resize(af.stride * fh);
        cv->CopyPixels(nullptr, af.stride, (UINT)(af.stride*fh), af.pixels.data());
        af.delayMs = 100;
        _frames.push_back(std::move(af));
    }
    _playing = !_frames.empty();
    _lastTick = std::chrono::steady_clock::now();
    return _playing;
}

int AnimationPlayer::NextFrame() {
    if (!_playing || _frames.empty()) return -1;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastTick).count();
    if (elapsed >= _frames[_currentFrame].delayMs) {
        _currentFrame = (_currentFrame + 1) % (int)_frames.size();
        _lastTick = now;
    }
    return _currentFrame;
}

const AnimationFrame* AnimationPlayer::CurrentFrame() const {
    return _frames.empty() ? nullptr : &_frames[_currentFrame];
}

bool AnimationPlayer::SetFrame(int index) {
    if (_frames.empty()) return false;
    if (index < 0) index = 0;
    if (index >= (int)_frames.size()) index = (int)_frames.size() - 1;
    _currentFrame = index;
    _playing = false;  // 步进后暂停
    return true;
}

void AnimationPlayer::Reset() { _currentFrame = 0; _lastTick = std::chrono::steady_clock::now(); }
