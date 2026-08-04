#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <Windows.h>

class FileWatcherService {
public:
    FileWatcherService() = default;
    ~FileWatcherService();
    // 更新监视路径：只更新 _path + 自增版本号，零阻塞（不 join 不创建线程）
    bool Watch(const std::wstring& filePath);
    // 停止常驻线程：仅析构调用（join 最多等 100ms）
    void Stop();
    std::function<void()> OnChanged;
private:
    std::wstring _path;
    std::mutex   _pathMutex;                       // 保护 _path（Watch 写，worker 读）
    std::thread  _worker;
    std::atomic<bool>     _running{false};
    std::atomic<uint32_t> _pathVersion{0};         // 路径变更版本号，Watch 自增触发 worker 重 armed
    void WorkerThread();
};
