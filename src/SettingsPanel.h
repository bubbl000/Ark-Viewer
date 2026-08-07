#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <wrl/client.h>
#include "D2DRenderer.h"
#include "Config.h"

using Microsoft::WRL::ComPtr;

// D2D 自绘设置面板（模态，与主界面统一深色主题）
// 左侧竖向 Tab 导航 + 右侧内容区 + 底部确定按钮
class SettingsPanel {
public:
    void Show(HWND parent);  // 模态显示，阻塞直到关闭

private:
    HWND _hwnd = nullptr;
    HWND _parent = nullptr;
    D2DRenderer _r;
    ComPtr<IDWriteTextFormat> _font;       // 14px 主字体
    ComPtr<IDWriteTextFormat> _smallFont;  // 12px 小字体

    int  _tab = 0;           // 当前页 0=常规 1=习惯 2=文件关联
    int  _hoverId = -1;      // 当前 hover 的元素 ID（-1=无）
    bool _pressedOk = false; // 确定按钮按下中

    AppConfig _cfg;          // 配置副本（确定才写入）

    struct Assoc { std::wstring ext; bool on; };
    std::vector<Assoc> _assocs;
    int  _assocScroll = 0;   // 列表滚动偏移（像素）

    // DPI 缩放因子（DPI/96）：高分屏（2K/4K）放大 UI 尺寸与字号
    float _s = 1.0f;
    // 设计值 → 当前 DPI 物理像素
    int   S(int v)   const { return (int)(v * _s); }
    float S(float v) const { return v * _s; }
    // 布局常量（设计值，96 DPI 基准；使用时经 S() 缩放）
    static constexpr int WIN_W = 750, WIN_H = 620;
    static constexpr int NAV_W = 100, TAB_H = 38, ROW_H = 30;
    static constexpr int BTN_W = 80, BTN_H = 28;
    static constexpr int LIST_ROW_H = 24;

    // 命中矩形（Paint 中更新，交互中使用）
    struct FRect { float x, y, w, h; };
    FRect _navR[3], _okR, _allR, _listR;
    FRect _scaleR;              // 界面缩放滑块轨道
    bool  _scaleDragging = false;  // 滑块拖动中
    FRect _checkerR;            // 棋盘格透明度滑块轨道
    bool  _checkerDragging = false; // 透明度滑块拖动中

    // 元素 ID 编码（hover/click 统一用 ID）
    // 0-2: Tab 导航
    // 10-12: 常规页穿透单选
    // 20-22: 常规页视图复选（20 鸟瞰图 21 缩略图 22 棋盘格）
    // 30-31: 习惯页复选
    // 40-41: 习惯页滚轮单选
    // 50: 全选复选
    // 60: 界面缩放滑块
    // 100+i: 列表项
    // 1000: 确定按钮

    // 滑块 x → 缩放值 100-200
    int XToScale(int x) const;
    int XToOpacity(int x) const;

    void Paint();
    void DrawCheck(float x, float y, bool on, bool hover);
    void DrawRadio(float x, float y, bool on, bool hover);
    void DrawSection(float x, float y, float w, const wchar_t* title);
    void DrawTextVCenter(const wchar_t* text, float x, float y, float w, float h,
                         D2D1_COLOR_F color, IDWriteTextFormat* fmt);
    void LDown(int x, int y);
    void LUp(int x, int y);
    void Move(int x, int y);
    void Wheel(int delta);
    void Key(int vk);
    void Apply();
    int  HitTest(int x, int y);  // 返回元素 ID，-1=无

    static LRESULT CALLBACK WProc(HWND, UINT, WPARAM, LPARAM);
};
