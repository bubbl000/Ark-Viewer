#include "FileWatcherService.h"
#include "Logger.h"
#include "ActivityLog.h"

FileWatcherService::~FileWatcherService() { Stop(); }

bool FileWatcherService::Watch(const std::wstring& filePath) {
    // 常驻 worker 模式：只更新路径 + 自增版本号，不 join 不创建线程（零阻塞 UI）
    // worker 线程循环内检测版本号变化，重新 armed 新目录的通知句柄
    {
        std::lock_guard lock(_pathMutex);
        _path = filePath;
    }
    _pathVersion.fetch_add(1, std::memory_order_release);

    // 首次 Watch 启动常驻 worker（后续 Watch 复用同一线程，不再销毁重建）
    if (!_running.load(std::memory_order_relaxed)) {
        _running = true;
        _worker = std::thread(&FileWatcherService::WorkerThread, this);
    }
    return true;
}

void FileWatcherService::Stop() {
    // 仅析构调用：join 最多等 100ms（WaitForSingleObject 超时），程序退出时无感知
    _running = false;
    if (_worker.joinable()) _worker.join();
}

void FileWatcherService::WorkerThread() {
    ActivityLog::SetThreadName("FileWatcher");  // 线程角色名（性能遥测 JSONL 用）
    uint32_t lastVersion = 0;
    HANDLE hNotify = INVALID_HANDLE_VALUE;

    while (_running.load(std::memory_order_acquire)) {
        // 检测路径变更：Watch 更新 _path + 自增 _pathVersion
        uint32_t v = _pathVersion.load(std::memory_order_acquire);
        if (v != lastVersion) {
            lastVersion = v;
            std::wstring path;
            {
                std::lock_guard lock(_pathMutex);
                path = _path;
            }
            auto pos = path.find_last_of(L"\\/");
            std::wstring dir = (pos != std::wstring::npos) ? path.substr(0, pos) : path;

            // 关旧句柄，建新句柄
            if (hNotify != INVALID_HANDLE_VALUE) {
                FindCloseChangeNotification(hNotify);
                hNotify = INVALID_HANDLE_VALUE;
            }
            if (!dir.empty()) {
                hNotify = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
            }
            LOG_INFO("FileWatcher", "开始监视目录");
        }

        if (hNotify == INVALID_HANDLE_VALUE) {
            Sleep(100);  // 无有效目录，轻量等待
            continue;
        }

        // 100ms 超时：及时检测路径变更和 _running，避免 Watch/Stop 后延迟
        DWORD r = WaitForSingleObject(hNotify, 100);
        if (r == WAIT_TIMEOUT) continue;       // 超时：检查 _running 和路径变更后继续
        if (r != WAIT_OBJECT_0) break;          // 错误：退出

        // 目录中有文件变更，触发回调（由上层防抖+重载处理）
        if (OnChanged) OnChanged();

        // 重新 armed 通知句柄
        FindNextChangeNotification(hNotify);
    }

    if (hNotify != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(hNotify);
    }
    LOG_INFO("FileWatcher", "监视线程退出");
}
