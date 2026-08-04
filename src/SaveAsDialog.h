#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <wrl/client.h>
#include "D2DRenderer.h"

using Microsoft::WRL::ComPtr;

// D2D 自绘 JPEG 质量选择弹窗（另存为用）
// 滑条选质量 1-100（默认 100），实时显示该质量下预估文件大小（防抖 200ms 重算）
// 深色主题，与主界面统一
class SaveAsDialog {
public:
    // pixels/w/h/stride: 当前图 BGRA 像素（用于预估大小编码一次）
    // 返回用户选的质量(1-100)，取消/Esc 返回 -1
    int Show(HWND parent, const uint8_t* pixels, int w, int h, int stride);

private:
    HWND _hwnd = nullptr;
    D2DRenderer _r;
    ComPtr<IDWriteTextFormat> _font;
    ComPtr<IDWriteTextFormat> _smallFont;

    const uint8_t* _pixels = nullptr;
    int _w = 0, _h = 0, _stride = 0;

    int  _quality = 100;       // 1-100
    bool _dragging = false;    // 滑条拖动中
    int  _hoverId = -1;        // hover 元素 ID（1=保存 2=取消 -1=无）
    bool _pressedSave = false; // 保存按钮按下中

    // 预估大小（防抖重算，避免拖动时频繁编码）
    size_t _estimatedSize = 0;
    bool _needRecompute = false;
    std::chrono::steady_clock::time_point _recomputeDeadline;
    static constexpr int RECOMPUTE_DEBOUNCE_MS = 200;
    static constexpr UINT_PTR TIMER_ID = 9001;

    int  _result = -1;         // -1=取消, >0=质量

    // 布局
    static constexpr int WIN_W = 380, WIN_H = 280;
    struct FRect { float x, y, w, h; };
    FRect _sliderR, _saveR, _cancelR;

    void Paint();
    void RecomputeSize();      // 用当前质量编码一次，更新 _estimatedSize
    void ScheduleRecompute();  // 标记防抖重算
    void LDown(int x, int y);
    void LUp(int x, int y);
    void Move(int x, int y);
    void Key(int vk);
    int  HitTest(int x, int y);     // 命中测试：1=保存 2=取消 3=滑条 -1=无
    int  XToQuality(int x) const;   // 屏幕 x → 质量 1-100
    static LRESULT CALLBACK WProc(HWND, UINT, WPARAM, LPARAM);
};
