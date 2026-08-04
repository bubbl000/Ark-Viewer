#include "ColorManager.h"

ComPtr<ID2D1Effect> ColorManager::CreateGpuIccEffect(
    const uint8_t* iccData, size_t iccLen)
{
    if (!_renderer || !iccData || iccLen == 0) return nullptr;
    return _renderer->CreateIccEffect(iccData, iccLen);
}

bool ColorManager::ApplyIccTransform(uint8_t* /*pixels*/, int /*w*/, int /*h*/,
    const uint8_t* /*iccData*/, size_t /*iccLen*/)
{
    // 简化版：未来使用 LCMS2 或 Windows Color System
    // 当前返回 false 表示不处理
    return false;
}

void ColorManager::SetIccProfile(const uint8_t* data, size_t len) {
    if (data && len > 0) {
        _iccProfile.assign(data, data + len);
    } else {
        _iccProfile.clear();
    }
    _hasIccProfile = !_iccProfile.empty();
    if (_renderer && _hasIccProfile) {
        _iccEffect = _renderer->CreateIccEffect(data, len);
    } else {
        _iccEffect.Reset();
    }
}

void ColorManager::SetSourceBitmap(ID2D1Bitmap1* bitmap) {
    if (_iccEffect && bitmap) {
        _iccEffect->SetInput(0, bitmap);
    }
}

void ColorManager::Reset() {
    _iccProfile.clear();
    _hasIccProfile = false;
    _iccEffect.Reset();
}
