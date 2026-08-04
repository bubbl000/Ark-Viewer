#include "ActivityLog.h"
#include <chrono>
#include <ctime>
#include <windowsx.h>
#include <filesystem>
#include <cstdio>
#include <algorithm>

static const wchar_t* ACTIVITY_CLASS = L"ArkViewer2ActivityLog";
static const wchar_t* ACTIVITY_TITLE = L"活动日志 — Ark Viewer 2（F12 开关 · 右键复制）";

// 自定义消息：Log() 后 PostMessage 通知主线程尽快刷新
// 优先级高于 WM_TIMER（posted message 先于 WM_TIMER 被取出），避免快速操作时日志显示滞后
static constexpr UINT WM_FLUSH = WM_APP + 1;

// ─── 性能遥测辅助函数（匿名命名空间，仅本文件使用）───
namespace {

// 当前线程角色名（供 JSONL thread_name 字段）
thread_local std::string t_threadName{"Unknown"};

// 取当前 Unix 毫秒时间戳
int64_t NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 取今日日期 YYYYMMDD（用于文件名）
DWORD TodayYyyymmdd() {
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm local;
    localtime_s(&local, &tt);
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

// 取 exe 所在目录（绝对路径，末尾不带斜杠）
std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    auto pos = p.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? p.substr(0, pos) : p;
}

// 宽字符串转 UTF-8
std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &s[0], len, nullptr, nullptr);
    return s;
}

// UTF-8 转 widechar
std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}

// JSON 字符串转义（追加到 out）
void EscapeJson(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
}

// 读取环境变量（宽字符），返回是否存在且非空
bool GetEnvW(const wchar_t* name, std::wstring& out) {
    wchar_t buf[512];
    DWORD n = GetEnvironmentVariableW(name, buf, 512);
    if (n == 0 || n >= 512) return false;
    out = buf;
    return true;
}

} // namespace

// ─── ActivityLog 单例 ───

ActivityLog& ActivityLog::Instance() {
    static ActivityLog inst;
    return inst;
}

void ActivityLog::Init(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(DKGRAY_BRUSH);
    wc.lpszClassName = ACTIVITY_CLASS;
    RegisterClassExW(&wc);

    _hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, ACTIVITY_CLASS, ACTIVITY_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 780, 540,
        nullptr, nullptr, hInst, nullptr);

    _edit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 780, 540,
        _hwnd, nullptr, hInst, nullptr);

    _font = CreateFontW(
        14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(_edit, WM_SETFONT, (WPARAM)_font, TRUE);

    // 底部"清空"按钮
    _clearBtn = CreateWindowExW(
        0, L"BUTTON", L"清空",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 80, 26,
        _hwnd, (HMENU)(INT_PTR)IDC_CLEAR_BTN, hInst, nullptr);
    SendMessageW(_clearBtn, WM_SETFONT, (WPARAM)_font, TRUE);

    // 兜底定时器：PostMessage 为主路径，定时器仅防止极端情况下消息丢失
    SetTimer(_hwnd, FLUSH_TIMER_ID, FLUSH_INTERVAL_MS, nullptr);

    // 性能遥测启动开关：ARK_PERF=1 开启（运行时也可由 perf_data/control 文件动态开关）
    std::wstring v;
    if (GetEnvW(L"ARK_PERF", v) && (v == L"1" || v == L"true")) {
        // 可选：ARK_PERF_STALL_MS 覆盖卡顿阈值
        std::wstring ms;
        if (GetEnvW(L"ARK_PERF_STALL_MS", ms) && !ms.empty()) {
            try { _stallMs = std::stod(WToUtf8(ms)); } catch (...) {}
        }
        // 可选：ARK_PERF_DIR 覆盖 perf_data 目录
        std::wstring dir;
        if (GetEnvW(L"ARK_PERF_DIR", dir) && !dir.empty()) {
            _perfDir = WToUtf8(dir);
        }
        PerfEnable();
    }
}

void ActivityLog::Toggle() {
    if (!_hwnd) return;
    if (IsWindowVisible(_hwnd)) {
        ShowWindow(_hwnd, SW_HIDE);
    } else {
        ShowWindow(_hwnd, SW_SHOW);
        SetForegroundWindow(_hwnd);
    }
}

bool ActivityLog::IsVisible() const {
    return _hwnd && IsWindowVisible(_hwnd);
}

void ActivityLog::Log(const wchar_t* category, const std::wstring& msg) {
    // 时间戳
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    struct tm local;
    localtime_s(&local, &tt);

    wchar_t ts[32];
    swprintf(ts, 32, L"%02d:%02d:%02d.%03lld",
        local.tm_hour, local.tm_min, local.tm_sec, (long long)ms.count());

    std::wstring line = std::wstring(ts) + L" [" + category + L"] " + msg;

    bool needPost = false;
    {
        std::lock_guard lock(_mutex);
        _pending.push_back(std::move(line));
        _lastActivity = std::wstring(category) + L": " + msg;
        // 仅在无待处理 WM_FLUSH 时 PostMessage，防止快速日志导致消息队列积压
        needPost = !_flushPending.exchange(true, std::memory_order_acq_rel);
    }
    if (needPost && _hwnd) PostMessageW(_hwnd, WM_FLUSH, 0, 0);

    // 性能遥测开启时：镜像一行 type=activity 到 JSONL（供 MCP search_logs 检索 F12 日志内容）
    // _mutex 已释放，仅持 _perfMutex，无锁嵌套
    if (_perfEnabled.load(std::memory_order_relaxed)) {
        std::string line = BuildJsonLine("activity", category, nullptr, 0,
                                         {Perf::S("msg", WToUtf8(msg))});
        WritePerfLine(line);
    }
}

std::wstring ActivityLog::LastActivity() const {
    std::lock_guard lock(_mutex);
    return _lastActivity;
}

void ActivityLog::Clear() {
    std::lock_guard lock(_mutex);
    _pending.clear();
    _lastActivity.clear();
    _totalLines = 0;
    _flushPending.store(false, std::memory_order_release);
    if (_edit) SetWindowTextW(_edit, L"");
}

void ActivityLog::FlushToEdit() {
    std::deque<std::wstring> batch;
    {
        std::lock_guard lock(_mutex);
        batch.swap(_pending);
        _flushPending.store(false, std::memory_order_release);  // 允许下次 PostMessage
    }
    if (batch.empty() || !_edit) return;

    // 批量合并：所有行拼为一个字符串，一次 EM_REPLACESEL 追加
    // 避免逐行 SendMessage（N 行 × 3 次调用 → 5 次总调用），大幅减少主线程阻塞
    std::wstring all;
    all.reserve(batch.size() * 80);
    for (const auto& line : batch) {
        all += line;
        all += L"\r\n";
    }
    _totalLines += batch.size();

    SendMessageW(_edit, EM_SETREADONLY, FALSE, 0);
    int len = GetWindowTextLengthW(_edit);
    SendMessageW(_edit, EM_SETSEL, len, len);
    SendMessageW(_edit, EM_REPLACESEL, FALSE, (LPARAM)all.c_str());
    SendMessageW(_edit, EM_SETREADONLY, TRUE, 0);
    SendMessageW(_edit, EM_SCROLLCARET, 0, 0);  // 自动滚到底部

    // 超上限：截断前半部分，保留最近 MAX_LINES/2 行
    if (_totalLines > MAX_LINES) {
        int textLen = GetWindowTextLengthW(_edit);
        if (textLen > 0) {
            std::wstring full(textLen + 1, L'\0');
            GetWindowTextW(_edit, full.data(), textLen + 1);
            full.resize(textLen);
            size_t keepLines = MAX_LINES / 2;
            size_t pos = full.size();
            for (size_t i = 0; i < keepLines && pos > 0; i++) {
                pos = full.rfind(L'\n', pos - 1);
                if (pos == std::wstring::npos) { pos = 0; break; }
            }
            if (pos > 0 && pos < full.size()) {
                SetWindowTextW(_edit, full.substr(pos + 1).c_str());
            }
            _totalLines = keepLines;
        }
    }
}

LRESULT CALLBACK ActivityLog::WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_SIZE: {
        // Edit 填满上方区域，底部留 34px 给按钮
        int w = LOWORD(l), h = HIWORD(l);
        HWND edit = FindWindowExW(hwnd, nullptr, L"EDIT", nullptr);
        if (edit) MoveWindow(edit, 0, 0, w, h - 34, TRUE);
        HWND btn = GetDlgItem(hwnd, IDC_CLEAR_BTN);
        if (btn) MoveWindow(btn, w - 90, h - 30, 80, 26, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDC_CLEAR_BTN) {
            ActivityLog::Instance().Clear();
            return 0;
        }
        break;
    case WM_TIMER:
        if (w == FLUSH_TIMER_ID) {
            ActivityLog::Instance().FlushToEdit();
            ActivityLog::Instance().PollTick();  // 性能遥测：控制文件检查 + 每 1s 轮转/快照/flush
            return 0;
        }
        break;
    case WM_FLUSH:
        // Log() 后 PostMessage 触发：优先级高于 WM_TIMER，快速操作时也能及时刷新
        ActivityLog::Instance().FlushToEdit();
        return 0;
    case WM_CONTEXTMENU: {
        // 右键菜单：清空日志 / 全选复制
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"清空日志");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 2, L"全选并复制");
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (pt.x < 0 || pt.y < 0) GetCursorPos(&pt);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN,
            pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        HWND edit = FindWindowExW(hwnd, nullptr, L"EDIT", nullptr);
        if (cmd == 1 && edit) {
            SetWindowTextW(edit, L"");
            ActivityLog::Instance()._totalLines = 0;
        } else if (cmd == 2 && edit) {
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SendMessageW(edit, WM_COPY, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE:
        // 隐藏而非销毁：F12 可重新显示
        ShowWindow(hwnd, SW_HIDE);
        return 0;  // 阻止 DefWindowProc 调用 DestroyWindow
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// ═══ 性能遥测实现 ═══

void ActivityLog::SetThreadName(const char* name) {
    t_threadName = name;
}

void ActivityLog::SetSnapshotCollector(std::function<void()> cb) {
    // 仅主线程注册一次，之后只读，无需锁
    _snapshotCollector = std::move(cb);
}

void ActivityLog::PerfEnable() {
    if (_perfEnabled.exchange(true)) return;  // 已开启
    if (_perfDir.empty()) {
        _perfDir = WToUtf8(ExeDir()) + "\\perf_data";
    }
    std::error_code ec;
    std::filesystem::create_directories(Utf8ToW(_perfDir), ec);
    _perfCurrentDay = TodayYyyymmdd();
    std::lock_guard lock(_perfMutex);
    _perfCurrentFile = OpenTodayFile();
}

void ActivityLog::PerfDisable() {
    if (!_perfEnabled.exchange(false)) return;
    std::lock_guard lock(_perfMutex);
    if (_perfStream) { _perfStream.flush(); _perfStream.close(); }
}

std::string ActivityLog::OpenTodayFile() {
    // 调用方须持有 _perfMutex
    std::string fname = "perf_" + std::to_string(_perfCurrentDay) + ".jsonl";
    // 用 wide string 打开：路径含中文时 ofstream.open(const char*) 按 GBK 解析会乱码失败
    std::wstring wpath = Utf8ToW(_perfDir + "\\" + fname);
    _perfStream.close();
    _perfStream.open(wpath.c_str(), std::ios::app | std::ios::out);
    return fname;
}

void ActivityLog::WritePerfLine(const std::string& line) {
    std::lock_guard lock(_perfMutex);
    if (_perfStream) _perfStream << line << '\n';
}

std::string ActivityLog::BuildJsonLine(const char* type, const wchar_t* category,
                                       const char* op, double duration_ms,
                                       const std::vector<PerfKV>& extra) {
    std::string s;
    s.reserve(256);
    s += "{\"ts\":";
    s += std::to_string(NowUnixMs());
    s += ",\"thread_id\":";
    s += std::to_string(GetCurrentThreadId());
    s += ",\"thread_name\":\"";
    EscapeJson(s, t_threadName);
    s += "\",\"type\":\"";
    s += type;
    s += "\",\"category\":\"";
    EscapeJson(s, WToUtf8(category ? category : L""));
    if (op) {
        s += "\",\"op\":\"";
        EscapeJson(s, op);
    }
    s += "\",\"duration_ms\":";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", duration_ms);
    s += buf;
    s += ",\"extra\":{";
    for (size_t i = 0; i < extra.size(); ++i) {
        if (i) s += ",";
        s += "\"";
        EscapeJson(s, extra[i].key);
        s += "\":";
        if (extra[i].isString) {
            s += "\"";
            EscapeJson(s, extra[i].sval);
            s += "\"";
        } else {
            std::snprintf(buf, sizeof(buf), "%.3f", extra[i].nval);
            s += buf;
        }
    }
    s += "}}";
    return s;
}

void ActivityLog::LogTimed(const wchar_t* category, const char* op,
                           double duration_ms, std::vector<PerfKV> extra) {
    if (!_perfEnabled.load(std::memory_order_relaxed)) return;
    WritePerfLine(BuildJsonLine("event", category, op, duration_ms, extra));
}

void ActivityLog::LogStall(const wchar_t* category, const char* op,
                           double duration_ms, std::vector<PerfKV> extra) {
    if (!_perfEnabled.load(std::memory_order_relaxed)) return;
    // 调用栈采集为字符串（模块+偏移，分号分隔），运行时不解析符号，留给 Python 端
    std::string stackStr;
    void* frames[16];
    USHORT n = CaptureStackBackTrace(0, 16, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)frames[i], &mod);
        char modName[MAX_PATH] = {};
        if (mod) GetModuleFileNameA(mod, modName, MAX_PATH);
        // 取模块文件名
        const char* base = modName;
        for (const char* p = modName; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        char off[48];
        std::snprintf(off, sizeof(off), "%s+0x%llx", base,
                      (unsigned long long)((char*)frames[i] - (char*)mod));
        if (i) stackStr += ";";
        stackStr += off;
    }
    if (!stackStr.empty()) extra.push_back(Perf::S("stack", stackStr));
    WritePerfLine(BuildJsonLine("stall", category, op, duration_ms, extra));
}

void ActivityLog::LogSnapshot(const wchar_t* category, std::vector<PerfKV> extra) {
    if (!_perfEnabled.load(std::memory_order_relaxed)) return;
    WritePerfLine(BuildJsonLine("snapshot", category, "snapshot", 0, extra));
}

void ActivityLog::PollPerfControl() {
    // control 文件固定在 exe 目录\perf_data\control（MCP 写入位置）
    std::wstring ctrlPath = ExeDir() + L"\\perf_data\\control";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(ctrlPath.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;  // control 文件不存在
    FindClose(h);
    uint64_t mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                     fd.ftLastWriteTime.dwLowDateTime;
    if (mtime == _controlLastMtime) return;  // 未变，跳过（避免每 200ms 读文件内容）
    _controlLastMtime = mtime;
    // 读取内容前 8 字节判断 ON/OFF
    HANDLE f = CreateFileW(ctrlPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[8] = {};
    DWORD read = 0;
    ReadFile(f, buf, 7, &read, nullptr);
    CloseHandle(f);
    bool wantOn = (read >= 2 &&
                   (buf[0] == 'O' || buf[0] == 'o') &&
                   (buf[1] == 'N' || buf[1] == 'n'));
    if (wantOn && !_perfEnabled.load(std::memory_order_relaxed)) PerfEnable();
    else if (!wantOn && _perfEnabled.load(std::memory_order_relaxed)) PerfDisable();
}

void ActivityLog::PollTick() {
    PollPerfControl();
    if (++_perfTickCount >= PERF_TICKS_PER_SECOND) {
        _perfTickCount = 0;
        if (_perfEnabled.load(std::memory_order_relaxed)) {
            RotationCheck();
            if (_snapshotCollector) _snapshotCollector();
            std::lock_guard lock(_perfMutex);
            if (_perfStream) _perfStream.flush();
        }
    }
}

void ActivityLog::RotationCheck() {
    // 跨天：开新文件
    DWORD today = TodayYyyymmdd();
    if (today != _perfCurrentDay) {
        _perfCurrentDay = today;
        std::lock_guard lock(_perfMutex);
        _perfCurrentFile = OpenTodayFile();
    }
    // 单文件超 100MB：切到 _NNN 后缀
    {
        std::lock_guard lock(_perfMutex);
        if (_perfStream && (size_t)_perfStream.tellp() >= PERF_FILE_MAX) {
            std::string base = "perf_" + std::to_string(_perfCurrentDay);
            for (int seq = 1; seq < 999; ++seq) {
                std::string fname = base + "_" + std::to_string(seq) + ".jsonl";
                std::string path = _perfDir + "\\" + fname;
                if (GetFileAttributesW(Utf8ToW(path).c_str()) == INVALID_FILE_ATTRIBUTES) {
                    _perfStream.close();
                    _perfStream.open(Utf8ToW(path).c_str(), std::ios::app | std::ios::out);
                    _perfCurrentFile = fname;
                    break;
                }
            }
        }
    }
    // 目录超 200MB：按文件名排序删最旧（不碰 _perfStream，无需锁）
    namespace fs = std::filesystem;
    std::wstring wdir = Utf8ToW(_perfDir);
    uintmax_t total = 0;
    std::vector<std::pair<fs::path, uintmax_t>> files;
    try {
        for (auto& e : fs::directory_iterator(wdir)) {
            if (!e.is_regular_file()) continue;
            auto name = e.path().filename().string();
            if (name.rfind("perf_", 0) != 0) continue;  // 只算 perf_*.jsonl
            if (name.find(".jsonl") == std::string::npos) continue;
            total += e.file_size();
            files.push_back({e.path(), e.file_size()});
        }
    } catch (...) { return; }
    if (total <= PERF_DIR_MAX) return;
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [path, sz] : files) {
        if (total <= PERF_DIR_MAX) break;
        std::error_code ec;
        fs::remove(path, ec);
        if (!ec) total -= sz;
    }
}

// ═══ PerfScope 实现 ═══

PerfScope::PerfScope(const wchar_t* category, const char* op) noexcept
    : _cat(category), _op(op),
      _active(ActivityLog::Instance().IsPerfEnabled()) {
    if (_active) _start = std::chrono::steady_clock::now();
}

PerfScope::~PerfScope() {
    if (!_active) return;
    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - _start).count();
    auto& log = ActivityLog::Instance();
    if (ms >= log.StallMs())
        log.LogStall(_cat, _op, ms, std::move(_extra));
    else
        log.LogTimed(_cat, _op, ms, std::move(_extra));
}

// ─── ActivityFmt 辅助函数 ───

namespace ActivityFmt {

// 宽字符串转 UTF-8（供 PerfKV 的 sval 用）
std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &s[0], len, nullptr, nullptr);
    return s;
}

std::wstring ShortName(const std::wstring& path) {
    auto pos = path.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
}

std::wstring SizeStr(size_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L"B";
    if (bytes < 1024 * 1024) return std::to_wstring(bytes / 1024) + L"KB";
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1fMB", (double)bytes / (1024.0 * 1024.0));
    return buf;
}

} // namespace ActivityFmt
