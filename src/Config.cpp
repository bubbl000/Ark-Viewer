#include "Config.h"
#include "Logger.h"
#include <Windows.h>
#include <fstream>
#include <sstream>

Config& Config::Instance() {
    static Config inst;
    return inst;
}

bool Config::Load() {
    // 取 exe 同目录作为配置文件路径
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;
    std::wstring dir = std::wstring(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return false;
    _path = dir.substr(0, pos + 1) + L"ArkViewer2.json";

    std::wifstream f(_path);
    if (!f.is_open()) return false;  // 文件不存在用默认值

    // 极简解析：按行找 "key": value
    // 不引第三方 JSON 库，字段少直接字符串匹配
    std::wstringstream ss;
    ss << f.rdbuf();
    std::wstring content = ss.str();

    auto extractInt = [&](const std::wstring& key, int def) -> int {
        std::wstring pat = L"\"" + key + L"\"";
        size_t p = content.find(pat);
        if (p == std::wstring::npos) return def;
        p = content.find(L':', p);
        if (p == std::wstring::npos) return def;
        // 跳过空白
        while (p < content.size() && (content[p] == L':' || content[p] == L' ')) p++;
        try { return std::stoi(content.substr(p)); }
        catch (...) { return def; }
    };
    auto extractBool = [&](const std::wstring& key, bool def) -> bool {
        std::wstring pat = L"\"" + key + L"\"";
        size_t p = content.find(pat);
        if (p == std::wstring::npos) return def;
        p = content.find(L':', p);
        if (p == std::wstring::npos) return def;
        while (p < content.size() && (content[p] == L':' || content[p] == L' ')) p++;
        if (content.compare(p, 4, L"true") == 0) return true;
        if (content.compare(p, 5, L"false") == 0) return false;
        return def;
    };

    _cfg.folderNavPolicy = extractInt(L"folderNavPolicy", 0);
    _cfg.thumbnailBarVisible = extractBool(L"thumbnailBarVisible", false);
    _cfg.birdsEyeVisible = extractBool(L"birdsEyeVisible", false);
    _cfg.wheelBehavior = extractInt(L"wheelBehavior", 0);
    _cfg.alwaysOnTop = extractBool(L"alwaysOnTop", false);
    _cfg.allowMultipleWindows = extractBool(L"allowMultipleWindows", true);
    _cfg.gifPanelX = extractInt(L"gifPanelX", -1);
    _cfg.gifPanelY = extractInt(L"gifPanelY", -1);
    _cfg.uiScale = extractInt(L"uiScale", 100);
    // 范围钳制：100~200%，防手改配置非法值
    if (_cfg.uiScale < 100) _cfg.uiScale = 100;
    if (_cfg.uiScale > 200) _cfg.uiScale = 200;
    return true;
}

void Config::Save() {
    if (_path.empty()) return;
    std::wofstream f(_path);
    if (!f.is_open()) {
        LOG_WARN("Config", "保存配置失败");
        return;
    }
    f << L"{\n";
    f << L"  \"folderNavPolicy\": " << _cfg.folderNavPolicy << L",\n";
    f << L"  \"thumbnailBarVisible\": " << (_cfg.thumbnailBarVisible ? L"true" : L"false") << L",\n";
    f << L"  \"birdsEyeVisible\": " << (_cfg.birdsEyeVisible ? L"true" : L"false") << L",\n";
    f << L"  \"wheelBehavior\": " << _cfg.wheelBehavior << L",\n";
    f << L"  \"alwaysOnTop\": " << (_cfg.alwaysOnTop ? L"true" : L"false") << L",\n";
    f << L"  \"allowMultipleWindows\": " << (_cfg.allowMultipleWindows ? L"true" : L"false") << L",\n";
    f << L"  \"gifPanelX\": " << _cfg.gifPanelX << L",\n";
    f << L"  \"gifPanelY\": " << _cfg.gifPanelY << L",\n";
    f << L"  \"uiScale\": " << _cfg.uiScale << L"\n";
    f << L"}\n";
}

void Config::Set(const AppConfig& c) {
    _cfg = c;
    Save();
}
