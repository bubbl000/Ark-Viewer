#pragma once
#include <string>

// ─── 应用配置（持久化） ───
// JSON 文件存于 exe 同目录（ArkViewer2.json），便携友好
// 字段少，手写序列化避免引入第三方库

struct AppConfig {
    // 文件夹穿透策略：0=总在本文件夹循环(默认) 1=总是进入下个文件夹 2=提示询问
    int  folderNavPolicy = 0;
    bool thumbnailBarVisible = false;  // 缩略图条开关（由"更多"菜单激活）
    bool birdsEyeVisible = false;      // 鸟瞰图开关
    // 滚轮行为：0=滚轮缩放(默认) 1=滚轮切换图片；Ctrl+滚轮始终执行另一项
    int  wheelBehavior = 0;
    bool alwaysOnTop = false;          // 窗口总是置顶于所有窗口最前面
    bool allowMultipleWindows = true;  // 允许进程内打开多个看图窗口（默认 true 保留现有行为）
    int  gifPanelX = -1;              // GIF 控制面板位置（-1=默认右下角）
    int  gifPanelY = -1;
};

class Config {
public:
    static Config& Instance();
    const AppConfig& Get() const { return _cfg; }
    // 写入内存 + 立即落盘
    void Set(const AppConfig& c);
    // 程序启动调用：读取 exe 同目录 ArkViewer2.json，失败用默认值
    bool Load();
private:
    Config() = default;
    AppConfig _cfg;
    std::wstring _path;  // exe 同目录 ArkViewer2.json

    void Save();  // 序列化到 _path
};
