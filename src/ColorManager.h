#pragma once
#include "D2DRenderer.h"
#include <vector>
#include <cstdint>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ─── ICC 色彩管理 ───
// 对应原 C# 的 ColorSpaceTransform + IccProfileCache

class ColorManager {
public:
    ColorManager() = default;

    // 设置渲染器
    void SetRenderer(D2DRenderer* renderer) { _renderer = renderer; }

    // 从 ICC profile bytes 创建 GPU effect
    ComPtr<ID2D1Effect> CreateGpuIccEffect(const uint8_t* iccData, size_t iccLen);

    // CPU 端 ICC 变换（GPU 不支持时回退）
    // 直接修改像素数据
    static bool ApplyIccTransform(uint8_t* pixels, int w, int h,
        const uint8_t* iccData, size_t iccLen);

    // 获取当前 ICC
    bool HasIcc() const { return _hasIccProfile; }
    void SetIccProfile(const uint8_t* data, size_t len);

    // 给 effect 设置输入位图（渲染前必须调用）
    void SetSourceBitmap(ID2D1Bitmap1* bitmap);

    // 获取已创建的 GPU effect（可能为 null，表示 GPU 不支持）
    ID2D1Effect* IccEffect() const { return _iccEffect.Get(); }

    // 重置（切换图片时调用）
    void Reset();

private:
    D2DRenderer* _renderer = nullptr;
    ComPtr<ID2D1Effect> _iccEffect;
    std::vector<uint8_t> _iccProfile;
    bool _hasIccProfile = false;
};
