#pragma once
#include <string>
#include <mutex>
#include <deque>
#include <atomic>
#include <fstream>
#include <functional>
#include <vector>
#include <chrono>
#include <windows.h>

// ─── 性能遥测字段键值对 ───
// extra 字段用：字符串值（isString=true）或数值（isString=false）
struct PerfKV {
    std::string key;
    std::string sval;        // 字符串值
    double      nval = 0;    // 数值
    bool        isString = true;
};
namespace Perf {
    inline PerfKV S(std::string k, std::string v) { return {std::move(k), std::move(v), 0, true}; }
    inline PerfKV N(std::string k, double v)      { return {std::move(k), {},      v, false}; }
}

// ─── 活动日志窗口（单例） ───
// 独立窗口显示程序内部操作过程的详细日志，供排查问题用
// F12 键切换显示/隐藏；右键菜单可清空日志
// Log() 线程安全：任意线程调用，日志暂存队列，PostMessage 通知主线程尽快刷新
//
// 性能遥测扩展（默认关闭，ARK_PERF=1 或控制文件开启）：
//   LogTimed/LogStall/LogSnapshot 落盘 JSONL 到 perf_data/，供 MCP server 读取
class ActivityLog {
public:
    static ActivityLog& Instance();

    // 创建日志窗口（初始隐藏），必须在主线程调用
    void Init(HINSTANCE hInst);

    // F12 切换显示/隐藏
    void Toggle();

    // 记录一条活动日志（线程安全）
    // category: 分类标签（如 L"加载" L"解码" L"瓦片" L"预解码" L"切换" L"缩放"）
    // msg: 详细消息
    void Log(const wchar_t* category, const std::wstring& msg);

    // 获取最近一条活动（简短），供底部状态栏显示
    std::wstring LastActivity() const;

    // 清空日志
    void Clear();

    bool IsVisible() const;

    // ── 性能遥测（默认关闭，开启时落盘 JSONL）──
    bool IsPerfEnabled() const noexcept {
        return _perfEnabled.load(std::memory_order_relaxed);
    }
    // 卡顿阈值（毫秒），超此值记为 stall 行；Init 时可由 ARK_PERF_STALL_MS 覆盖
    double StallMs() const noexcept { return _stallMs; }

    // 设置当前线程角色名（线程入口调用一次，写入 thread_local）
    static void SetThreadName(const char* name);

    // 结构化事件行（type=event）
    void LogTimed(const wchar_t* category, const char* op,
                  double duration_ms, std::vector<PerfKV> extra = {});
    // 卡顿行（type=stall）：额外采集调用栈 {模块+偏移}，运行时不解析符号
    void LogStall(const wchar_t* category, const char* op,
                  double duration_ms, std::vector<PerfKV> extra);
    // 周期快照行（type=snapshot）
    void LogSnapshot(const wchar_t* category, std::vector<PerfKV> extra);

    // 注册快照采集器：内部 1s 定时器调用（_perfEnabled 时），由 WindowManager 注册
    // 采集器内部自行调 LogSnapshot/LogTimed 上报各窗口数据
    void SetSnapshotCollector(std::function<void()> cb);

private:
    ActivityLog() = default;
    HWND _hwnd = nullptr;
    HWND _edit = nullptr;
    HWND _clearBtn = nullptr;
    HFONT _font = nullptr;
    static constexpr int IDC_CLEAR_BTN = 101;  // 清空按钮控件 ID
    mutable std::mutex _mutex;
    std::wstring _lastActivity;  // 最后一行简短描述，供状态栏
    std::deque<std::wstring> _pending;  // 待刷新到 Edit 的行（跨线程暂存）
    std::atomic<bool> _flushPending{false};  // 已 PostMessage 通知刷新，避免消息队列积压
    size_t _totalLines = 0;
    static constexpr size_t MAX_LINES = 5000;  // 超过则截断前半部分
    static constexpr UINT FLUSH_TIMER_ID = 9001;
    static constexpr UINT FLUSH_INTERVAL_MS = 200;  // 兜底定时器（PostMessage 为主）
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void FlushToEdit();  // 主线程调用：将 _pending 批量写入 Edit

    // ── 性能遥测私有成员 ──
    std::atomic<bool> _perfEnabled{false};
    double _stallMs = 100.0;              // 卡顿阈值，可由 ARK_PERF_STALL_MS 覆盖
    std::mutex _perfMutex;                // 保护 _perfStream（与 _mutex 分离，避免嵌套死锁）
    std::ofstream _perfStream;            // 追加写当前 JSONL
    std::string _perfDir;                 // perf_data 绝对路径
    std::string _perfCurrentFile;         // 当前写入的文件名
    DWORD _perfCurrentDay = 0;            // 当前文件日期（YYYYMMDD），跨天轮转
    int _perfTickCount = 0;               // 200ms 计数，每 5 tick=1s 做轮转/快照
    uint64_t _controlLastMtime = 0;       // control 文件上次 mtime，未变则跳过读取
    std::function<void()> _snapshotCollector;
    static constexpr size_t PERF_FILE_MAX = 100ULL * 1024 * 1024;   // 单文件 100MB
    static constexpr size_t PERF_DIR_MAX  = 200ULL * 1024 * 1024;   // 目录 200MB
    static constexpr int PERF_TICKS_PER_SECOND = 5;                  // 200ms × 5 = 1s

    // 性能遥测私有方法
    void PerfEnable();                    // 建目录、开今日文件
    void PerfDisable();                   // 关流
    void PollPerfControl();               // 检查 perf_data/control 文件，动态开关
    void PollTick();                      // WM_TIMER 调用：PollPerfControl + 每 1s 轮转/快照/flush
    void RotationCheck();                 // 跨天/单文件超限/目录超限删旧
    std::string OpenTodayFile();          // 打开 perf_YYYYMMDD.jsonl，返回文件名
    void WritePerfLine(const std::string& line);  // 持 _perfMutex 写一行（不含 \n）
    // 拼接一条 JSONL 行（不含末尾 \n）。op 为 nullptr 时省略 op 字段
    std::string BuildJsonLine(const char* type, const wchar_t* category,
                              const char* op, double duration_ms,
                              const std::vector<PerfKV>& extra);
};

// ─── 轻量 RAII 计时器 ───
// 构造记起点，析构算耗时：超阈值调 LogStall，否则 LogTimed
// _perfEnabled=false 时：构造仅一次原子读，析构/SetExtra 均 no-op（零开销）
class PerfScope {
public:
    PerfScope(const wchar_t* category, const char* op) noexcept;
    ~PerfScope();
    // 析构前补充上下文（关闭时 no-op，避免热路径字符串分配）
    void SetExtra(std::vector<PerfKV> extra) {
        if (_active) _extra = std::move(extra);
    }
private:
    const wchar_t* _cat;
    const char*    _op;
    std::chrono::steady_clock::time_point _start;
    bool _active;
    std::vector<PerfKV> _extra;
};

// ─── 日志格式化辅助 ───
namespace ActivityFmt {
    // 从完整路径提取文件名
    std::wstring ShortName(const std::wstring& path);
    // 文件大小格式化：B/KB/MB
    std::wstring SizeStr(size_t bytes);
    // 宽字符串转 UTF-8（供 PerfKV 的 sval 用）
    std::string NarrowUtf8(const std::wstring& w);
}
