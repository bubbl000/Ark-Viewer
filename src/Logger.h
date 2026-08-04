#pragma once
#include <string>
#include <sstream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <Windows.h>

// ─── 日志系统 ───
// 无宏冲突：枚举值使用 L_ 前缀避免和 Windows 宏重名

class Logger {
public:
    enum Level { L_DBG = 0, L_INFO = 1, L_WARN = 2, L_ERR = 3 };

    static Logger& Instance() {
        static Logger inst;
        return inst;
    }

    void Log(Level level, const char* tag, const std::string& msg) {
        std::lock_guard<std::mutex> lock(_mutex);
        EnsureFile();
        if (!_file.is_open()) return;

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        struct tm local; localtime_s(&local, &tt);

        char buf[512];
        int n = snprintf(buf, sizeof(buf),
            "[%02d:%02d:%02d.%03lld] [%s] %s: %s\n",
            local.tm_hour, local.tm_min, local.tm_sec, (long long)ms.count(),
            LevelStr(level), tag, msg.c_str());
        OutputDebugStringA(buf);

        _file.write(buf, n);
        _file.flush();
    }

    static std::wstring GetLogDir() {
        wchar_t path[MAX_PATH];
        GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
        return std::wstring(path) + L"\\ArkViewer2\\logs";
    }

private:
    std::mutex _mutex;
    std::ofstream _file;
    std::wstring _currentDate;

    Logger() = default;
    ~Logger() { if (_file.is_open()) _file.close(); }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static const char* LevelStr(Level l) {
        switch (l) {
        case L_DBG:  return "DEBUG";
        case L_INFO: return "INFO";
        case L_WARN: return "WARN";
        case L_ERR:  return "ERROR";
        default:     return "?";
        }
    }

    void EnsureFile() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        struct tm local; localtime_s(&local, &tt);

        wchar_t ds[16];
        swprintf(ds, 16, L"%04d-%02d-%02d",
            local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);

        if (_currentDate == ds && _file.is_open()) return;
        _currentDate = ds;
        if (_file.is_open()) _file.close();

        auto dir = GetLogDir();
        // CreateDirectoryW 不递归，需先建父目录 ArkViewer2 再建 logs
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring parent = dir.substr(0, pos);
            CreateDirectoryW(parent.c_str(), nullptr);
        }
        CreateDirectoryW(dir.c_str(), nullptr);
        _file.open(dir + L"\\arkviewer2-" + ds + L".log", std::ios::app);
    }
};

// ─── 便利宏 ───
#ifdef ARK_THUMB_DLL
// DLL 模式：日志写固定路径 C:\Users\aoebc\AppData\Local\Temp\arkthumb.log（调试用）
// 用固定路径而非 getenv("TEMP")：dllhost 低权限进程环境变量可能缺失，避免 nullptr 崩溃
#include <sstream>
#include <fstream>
namespace {
inline void ThumbLog(const char* tag, const std::string& msg) {
    std::ofstream f(L"C:\\Users\\aoebc\\AppData\\Local\\Temp\\arkthumb.log", std::ios::app);
    if (f) f << tag << ": " << msg << "\n";
}
}
#define LOG_DBG(tag, msg)  ThumbLog(tag, msg)
#define LOG_INFO(tag, msg) ThumbLog(tag, msg)
#define LOG_WARN(tag, msg) ThumbLog(tag, msg)
#define LOG_ERR(tag, msg)  ThumbLog(tag, msg)
#define LOG_DBG_STREAM(tag)  std::ostringstream()
#define LOG_INFO_STREAM(tag) std::ostringstream()
#define LOG_WARN_STREAM(tag) std::ostringstream()
#define LOG_ERR_STREAM(tag)  std::ostringstream()
#else
#define LOG_DBG(tag, msg)  Logger::Instance().Log(Logger::L_DBG, tag, msg)
#define LOG_INFO(tag, msg) Logger::Instance().Log(Logger::L_INFO, tag, msg)
#define LOG_WARN(tag, msg) Logger::Instance().Log(Logger::L_WARN, tag, msg)
#define LOG_ERR(tag, msg)  Logger::Instance().Log(Logger::L_ERR,  tag, msg)

// 流式日志：LOG_INFO_STREAM("Tag") << "val=" << val;
class LogStream {
public:
    LogStream(Logger::Level l, const char* tag) : _l(l), _tag(tag) {}
    ~LogStream() { Logger::Instance().Log(_l, _tag, _ss.str()); }
    std::ostringstream& S() { return _ss; }
private:
    Logger::Level _l;
    const char* _tag;
    std::ostringstream _ss;
};

#define LOG_DBG_STREAM(tag) LogStream(Logger::L_DBG, tag).S()
#define LOG_INFO_STREAM(tag) LogStream(Logger::L_INFO, tag).S()
#define LOG_WARN_STREAM(tag) LogStream(Logger::L_WARN, tag).S()
#define LOG_ERR_STREAM(tag)  LogStream(Logger::L_ERR,  tag).S()
#endif
