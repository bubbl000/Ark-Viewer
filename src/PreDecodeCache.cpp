#include "PreDecodeCache.h"
#include "ActivityLog.h"
#include "Logger.h"
#include <algorithm>
#include <chrono>
#include <Windows.h>
#include <objbase.h>

PreDecodeCache::PreDecodeCache() = default;
PreDecodeCache::~PreDecodeCache() { _running = false; if (_worker.joinable()) _worker.join(); }

void PreDecodeCache::SetCurrentIndex(int index,
    const std::vector<std::wstring>& files,
    std::function<bool(const std::wstring&, CachedImage&)> decoder,
    int forwardCount, int backwardCount)
{
    {
        std::lock_guard lock(_mutex);
        // 目录变更检测：文件列表不同才复制+清缓存（避免每次 Navigate 都 O(N) 复制）
        if (_files != files) {
            _cache.clear();
            _files = files;
        }
        _decoder = std::move(decoder);
        _forwardCount = forwardCount;
        _backwardCount = backwardCount;
        // 距离淘汰：删除超出保留范围的条目
        // MAX_RANGE=12 固定（预解码范围10+2余量），不随 fwd/bwd 变化
        // 避免方向切换时 fwd/bwd=0 导致范围缩小、刚预解码的被淘汰
        for (auto it = _cache.begin(); it != _cache.end(); ) {
            if (std::abs(it->first - index) > MAX_RANGE)
                it = _cache.erase(it);
            else
                ++it;
        }
        EvictByCount(index);  // 数量上限淘汰：超 MAX_CACHED_COUNT 时删最远的
        _currentIndex = index;  // 最后更新，WorkerThread 检测到变化时其他成员已就绪
    }
    // 未运行则启动工作线程；已运行则工作线程下一轮迭代会拾取新状态
    bool wasRunning = _running.exchange(true);
    if (!wasRunning)
        _worker = std::thread(&PreDecodeCache::WorkerThread, this);
}

std::shared_ptr<CachedImage> PreDecodeCache::Get(int index) {
    std::lock_guard lock(_mutex);
    auto it = _cache.find(index);
    if (it != _cache.end()) {
        it->second->lastAccessTick = std::chrono::steady_clock::now().time_since_epoch().count();
        return it->second;
    }
    return nullptr;
}

void PreDecodeCache::Clear() { std::lock_guard lock(_mutex); _cache.clear(); }

// 3 分钟空闲超时调用：仅保留当前图，移除其他顶层预览
// 不停止 WorkerThread：它只检测 _currentIndex 变化，清空不触发重新解码
void PreDecodeCache::ClearExcept(int keepIdx) {
    std::lock_guard lock(_mutex);
    for (auto it = _cache.begin(); it != _cache.end(); ) {
        if (it->first != keepIdx) it = _cache.erase(it);
        else ++it;
    }
}

// 数量上限淘汰：超 MAX_CACHED_COUNT 时按距 currentIdx 距离降序删最远的
// 调用方已持 _mutex
void PreDecodeCache::EvictByCount(int currentIdx) {
    if ((int)_cache.size() <= MAX_CACHED_COUNT) return;
    std::vector<std::pair<int, int>> dist;  // {distance, idx}
    dist.reserve(_cache.size());
    for (auto& kv : _cache) {
        dist.push_back({std::abs(kv.first - currentIdx), kv.first});
    }
    std::sort(dist.begin(), dist.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;  // 远的在前
    });
    for (size_t i = 0; i < dist.size() && (int)_cache.size() > MAX_CACHED_COUNT; i++) {
        _cache.erase(dist[i].second);
    }
}

void PreDecodeCache::WorkerThread() {
    ActivityLog::SetThreadName("PreDecode");  // 线程角色名（性能遥测 JSONL 用）
    // WIC 解码器需要 COM 初始化（工作线程不会继承主线程的 COM apartment）
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // 低优先级：当前图片瓦片解码（NORMAL）优先于后台预解码
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (_running) {
        // 复制状态快照（持锁期间拷贝，避免与 SetCurrentIndex 竞态）
        int idx;
        std::vector<std::wstring> files;
        std::function<bool(const std::wstring&, CachedImage&)> decoder;
        int fwd, bwd;
        {
            std::lock_guard lock(_mutex);
            idx = _currentIndex.load();
            files = _files;
            decoder = _decoder;
            fwd = _forwardCount;
            bwd = _backwardCount;
        }
        if (files.empty() || !decoder) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        // 向后预解码（含当前索引）：缩略图栏依赖本缓存绘制当前图，idx 必须优先解码
        int fwdEnd = (std::min)(idx + fwd, (int)files.size() - 1);
        for (int i = idx; i <= fwdEnd; i++) {
            if (!_running.load()) return;
            // 用户切换：立即中止旧计划
            if (_currentIndex.load() != idx) break;
            if (Get(i)) continue;
            // 当前图瓦片解码忙则让步，保证浏览当前图时不被预解码拖慢
            // _busyFlag 指向所属窗口 ImageEngine 的 _tileBusy（多窗口下互不干扰）
            // 5ms 轮询：兼顾让步响应速度和 CPU 开销（原 30ms 累计延迟过大）
            while (_busyFlag && _busyFlag->load(std::memory_order_relaxed)) {
                if (!_running.load() || _currentIndex.load() != idx) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!_running.load() || _currentIndex.load() != idx) break;
            CachedImage cached;
            ActivityLog::Instance().Log(L"预解码",
                L"开始: " + ActivityFmt::ShortName(files[i]));
            if (decoder(files[i], cached)) {
                {
                    std::lock_guard lock(_mutex);
                    _cache[i] = std::make_shared<CachedImage>(std::move(cached));
                }
                ActivityLog::Instance().Log(L"预解码",
                    L"完成: " + ActivityFmt::ShortName(files[i]) +
                    L" " + std::to_wstring(cached.width) + L"x" + std::to_wstring(cached.height));
                if (_onReady) _onReady(i);  // 锁外回调，避免回调中调 Get() 死锁
            }
        }
        // 向前预解码（索引减小方向）
        int bwdEnd = (std::max)(idx - bwd, 0);
        for (int i = idx - 1; i >= bwdEnd; i--) {
            if (!_running.load()) return;
            if (_currentIndex.load() != idx) break;
            if (Get(i)) continue;
            while (_busyFlag && _busyFlag->load(std::memory_order_relaxed)) {
                if (!_running.load() || _currentIndex.load() != idx) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!_running.load() || _currentIndex.load() != idx) break;
            CachedImage cached;
            ActivityLog::Instance().Log(L"预解码",
                L"开始: " + ActivityFmt::ShortName(files[i]));
            if (decoder(files[i], cached)) {
                {
                    std::lock_guard lock(_mutex);
                    _cache[i] = std::make_shared<CachedImage>(std::move(cached));
                }
                ActivityLog::Instance().Log(L"预解码",
                    L"完成: " + ActivityFmt::ShortName(files[i]) +
                    L" " + std::to_wstring(cached.width) + L"x" + std::to_wstring(cached.height));
                if (_onReady) _onReady(i);
            }
        }
        // 等待用户切换或停止
        // 30ms 轮询：原 200ms 会导致用户切换后 Worker 最长延迟 200ms 才开始新一轮预解码，
        // 叠加主线程 200ms 防抖，缓存缺口明显（快速连点时跟不上）
        while (_running.load() && _currentIndex.load() == idx)
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    CoUninitialize();
}
