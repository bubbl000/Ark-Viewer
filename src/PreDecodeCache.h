#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>

struct CachedImage {
    int width = 0, height = 0, stride = 0;           // 顶层预览尺寸
    std::shared_ptr<std::vector<uint8_t>> pixels;
    uint64_t lastAccessTick = 0;
    int origWidth = 0, origHeight = 0;                // 原始尺寸（FitToWindow/Pyramid 重建用）
    int topLevel = 0;                                  // 缓存像素对应的金字塔层级
    int levelCount = 1;                                // 金字塔总层数（<=1 时跳过瓦片解码）
    bool supportsTiling = false;                       // 该格式是否支持瓦片解码
};

class PreDecodeCache {
public:
    // 缓存保留范围：当前索引 ±MAX_RANGE 之外的条目会被淘汰
    // 预解码范围是 10，+6 余量：回翻 16 张内仍有缩略图缓存，减少缓存未命中
    static constexpr int MAX_RANGE = 16;
    static constexpr int MAX_CACHED_COUNT = 60;  // 数量上限，超限按距离淘汰最远的
    PreDecodeCache();
    ~PreDecodeCache();
    // forwardCount: 向后（索引增大）预解码张数
    // backwardCount: 向前（索引减小）预解码张数
    void SetCurrentIndex(int index, const std::vector<std::wstring>& files,
        std::function<bool(const std::wstring&, CachedImage&)> decoder,
        int forwardCount, int backwardCount);
    std::shared_ptr<CachedImage> Get(int index);
    void Clear();
    // 3 分钟空闲超时调用：仅保留当前图，移除其他顶层预览
    void ClearExcept(int keepIdx);
    void OnCacheReady(std::function<void(int)> cb) { _onReady = std::move(cb); }
    // 设置当前图瓦片解码忙标志指针：工作线程据此让步（多窗口下每窗口独立）
    void SetBusyFlag(std::atomic<bool>* flag) { _busyFlag = flag; }
private:
    std::unordered_map<int, std::shared_ptr<CachedImage>> _cache;
    std::mutex _mutex;  // 保护 _cache 及下方状态字段
    std::thread _worker;
    std::atomic<bool> _running{false};
    std::atomic<int>  _currentIndex{-1};  // 跨线程无锁检测用户切换
    std::vector<std::wstring> _files;
    std::function<bool(const std::wstring&, CachedImage&)> _decoder;
    int _forwardCount = 0;
    int _backwardCount = 0;
    std::function<void(int)> _onReady;
    std::atomic<bool>* _busyFlag = nullptr;  // 指向所属 ImageEngine 的 _tileBusy
    void EvictByCount(int currentIdx);  // 数量超限时淘汰最远索引（调用方持锁）
    void WorkerThread();
};
