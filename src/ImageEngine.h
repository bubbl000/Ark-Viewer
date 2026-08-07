#pragma once
#include "D2DRenderer.h"
#include "Pyramid.h"
#include "TileCache.h"
#include "Logger.h"
#include "ImageDecoder.h"
#include "AnimationPlayer.h"
#include "ColorManager.h"
#include "FileMapping.h"

// 前置声明，避免头文件引入 PreDecodeCache.h
class PreDecodeCache;
struct CachedImage;

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <unordered_map>
#include <shared_mutex>
#include <condition_variable>
#include <functional>
#include <algorithm>
#include <optional>
#include <chrono>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ─── 图片引擎 ───
// 管理图片加载、解码、显示、缩放、平移
// 对应原 C# MainWindow.xaml.cs 中的核心逻辑

class ImageEngine {
public:
    ImageEngine();
    ~ImageEngine();

    void Initialize(D2DRenderer* renderer, PreDecodeCache* preDecodeCache = nullptr);

    // ── 文件操作 ──
    // fwd/bwd: 预解码向后/向前张数，默认 (1,1) 对应"打开文件"场景（未点击时前后各1张）
    // forceAsync: 强制走异步解码路径（_bgCacheMiss 场景，小文件也异步避免阻塞 OnTimer）
    bool LoadFile(const std::wstring& path, int fwd = 1, int bwd = 1, bool forceAsync = false);
    void Unload();
    void Navigate(int steps);  // 负=向前N张, 正=向后N张（快速连按合并为一次跳转）
    void NavigateTo(int newIndex);  // 直接跳转到指定索引（OnPaint 跳帧调用，内部转调 Navigate）

    // ── 显示控制 ──
    void RenderFrame();
    void OnResize(int w, int h);
    void FitToWindow(int clientW, int clientH);
    void ZoomTo100();

    // ── 视图变换（只改变视图不改变原图） ──
    // R 键逆时针 90° 循环；H/V 键切换水平/垂直翻转
    void RotateLeft();
    void RotateRight();   // 顺时针 90°（右键菜单/工具栏↻）
    void ToggleFlipH();
    void ToggleFlipV();
    int  Rotation() const { return _rotation; }
    bool FlipH() const { return _flipH; }
    bool FlipV() const { return _flipV; }
    // 90/270 时显示宽高互换（FitToWindow/RenderFrame 计算用）
    bool IsRotatedSideways() const { return _rotation == 90 || _rotation == 270; }

    // ── 交互 ──
    void Zoom(double factor, double mouseX, double mouseY);
    void Pan(double dx, double dy);
    // 滚轮停止后的节流回调：由定时器周期调用，超时则切层+排瓦片
    // Zoom 期间不解码，纯 GPU 拉伸占位，停手后由此方法触发真正解码
    void CheckInteractionTimeout();
    // 3 分钟空闲超时：窗口在前台且无浏览操作时，移除非当前顶层预览
    void CheckIdleTimeout(HWND hwnd);

    // ── 状态 ──
    bool HasImage() const { return _sourceBitmap != nullptr; }
    double Scale() const { return _scaleX; }
    double ScaleY() const { return _scaleY; }   // 鸟瞰图视口计算用
    double OffsetX() const { return _offsetX; } // 鸟瞰图蓝框映射用
    double OffsetY() const { return _offsetY; }
    const std::wstring& FilePath() const { return _currentPath; }
    int CurrentIndex() const { return _currentIndex; }
    const std::vector<std::wstring>& GetDirFiles() const { return _dirFiles; }
    int DisplayedLevel() const { return _displayedLevel; }  // 当前显示的金字塔层级
    int LevelCount() const { return _pyramid ? _pyramid->LevelCount() : 0; }  // 金字塔总层数
    int SrcWidth() const { return _srcWidth; }   // 原图宽（EXIF 面板尺寸字段兜底用）
    int SrcHeight() const { return _srcHeight; }
    // 重命名后更新内部路径与目录列表项（不重新解码，纹理复用）
    void RenameCurrentPath(const std::wstring& newPath);

    // ── 跨文件夹导航（文件夹循环策略） ──
    // dir=+1 下一文件夹，-1 上一文件夹；返回 true 表示成功切换
    bool NavigateToSiblingFolder(int dir);
    bool IsFirstInDir() const { return _currentIndex == 0; }
    bool IsLastInDir() const { return !_dirFiles.empty() && _currentIndex == (int)_dirFiles.size() - 1; }

    // ── 性能遥测：瓦片缓存计数（snapshot 读取后清零）──
    uint64_t TileHitCount() const noexcept;   // 命中数
    uint64_t TileMissCount() const noexcept;  // 未命中数
    void TileResetHitMiss() noexcept;         // 清零（snapshot 读取后调用）
    int TileActiveWorkers() const noexcept { return _activeTileWorkers.load(std::memory_order_relaxed); }
    // 性能遥测：估算当前持有 ID2D1Bitmap 总像素 ×4B（显存近似，指针去重）
    size_t EstimateBitmapBytes() const;

    // ── 剪贴板支持 ──
    // 读回当前显示纹理的像素（BGRA，top-down），用于复制图片到剪贴板
    // stride: 行跨度（GPU pitch，可能 > width*4），传 nullptr 则不返回
    bool ReadDisplayPixels(std::vector<uint8_t>& pixels, int& width, int& height,
                           int* stride = nullptr);

    // 读原图全分辨率像素（另存为用）：保证输出尺寸 = 原图尺寸
    // 瓦片大图 _sourceBitmap 是缩放后的显示层，必须重新解码原图；
    // 非瓦片小图 _sourceBitmap 即原图，直接 GPU 回读省去重新解码
    bool ReadSourcePixels(std::vector<uint8_t>& pixels, int& width, int& height, int& stride);

    // ── 文件删除后处理 ──
    // 从目录列表移除当前文件并导航到相邻文件（文件已被删除到回收站后调用）
    void OnFileDeleted();

    // ── GIF 动画 ──
    void UpdateAnimation();  // 定时器回调：按帧延迟推进 GIF 动画
    bool IsPlayingAnimation() const { return _animPlayer.IsPlaying(); }
    // ── GIF 动画控制（供 UIEngine 控制面板调用）──
    int  AnimFrameCount() const;       // GIF 帧数（非 GIF 返回 0）
    int  AnimCurrentFrame() const;     // 当前帧索引
    bool AnimSetFrame(int i);          // 跳帧并暂停，更新 GPU 纹理
    void AnimTogglePlay();             // 播放/暂停切换

    // ── 瓦片解码状态（多窗口下每窗口独立，替代原全局标志） ──
    std::atomic<bool>& TileBusy()    { return _tileBusy; }    // 解码中：预解码让步
    std::atomic<bool>& TileUpdated() { return _tileUpdated; } // 已贴纹理待重绘

    // 异步顶层解码进行中：OnTimer 据此触发重绘，使 ApplyBgResultIfReady 能被调用
    bool IsBgDecodePending() const {
        return _bgState && _bgState->pending.load(std::memory_order_acquire);
    }

    // 检测并执行延迟的 UpdateViewport（OnTimer 调用，非绘制期安全）
    void FlushViewportUpdate();

    // ── 回调 ──
    std::function<void(const std::wstring&)> OnStatusChanged;

    // 静态解码回调（公开）：供侧边栏全文件夹缩略图生成线程调用，创建独立 FileMapping + decoder
    static bool DecodeForCache(const std::wstring& path, CachedImage& out);
    // 静态缩略图解码回调：供底部条/侧边栏共享缩略图生成线程调用，输出最终 80×110 等比小图（内存小）
    static bool DecodeForThumbnail(const std::wstring& path, CachedImage& out);

private:
    D2DRenderer* _renderer = nullptr;

    // ── 显示状态（对应原 _d2dSurface.ScaleX/Y 和 OffsetX/Y） ──
    double _scaleX  = 1.0;
    double _scaleY  = 1.0;
    double _offsetX = 0.0;
    double _offsetY = 0.0;

    // ── 视图变换状态（不改变原图，仅渲染时矩阵变换） ──
    int  _rotation = 0;    // 逆时针 0/90/180/270
    bool _flipH = false;   // 水平翻转
    bool _flipV = false;   // 垂直翻转

    // ── 交互节流 ──
    // Zoom 期间 _interacting=true，只更新几何参数+GPU 拉伸显示，不切层不解码
    // 停手超时（200ms）后由 CheckInteractionTimeout 触发 UpdateViewport 真正解码
    bool _interacting = false;
    std::chrono::steady_clock::time_point _interactionDeadline;
    // 渐进过渡：停手时若切层落差大（≥2 级），先切中间层，_pendingTargetLevel 记最终目标层
    // 200ms 后再切目标层，分两步消化落差，避免一次跨多层跳跃
    int _pendingTargetLevel = -1;

    // ── 滑动窗口批次预解码 ──
    // _fwdFrontier/_bwdFrontier: 向后/向前已预解码到的最远索引，-1=未启动
    // 每推进 5 张触发再预解码 10 张（如 frontier=10，当前推进到 5 时再解到 20）
    // _lastNavIndex: 上次 Navigate 后索引，检测方向切换重置前沿
    int _fwdFrontier = -1;
    int _bwdFrontier = -1;
    int _lastNavIndex = -1;
    // 翻页速度检测：间隔<300ms 累加 _navSpeedBurst，达到 2+ 视为快速连按
    // burst 时预解码 ±3 下限 + 防抖降为 0 立即触发，降低快速翻页未命中率
    std::chrono::steady_clock::time_point _lastNavStart;
    int _navSpeedBurst = 0;
    std::chrono::steady_clock::time_point _lastBrowseTick;  // 最后浏览操作时间（3 分钟超时用）

    // ── 图片数据 ──
    std::wstring _currentPath;
    std::wstring _currentDir;
    std::vector<std::wstring> _dirFiles;
    int _currentIndex = -1;

    ImageInfo _imageInfo;
    std::shared_ptr<Pyramid>      _pyramid;
    std::shared_ptr<TileCache>    _tileCache;

    // ── GPU 纹理 ──
    ComPtr<ID2D1Bitmap1> _sourceBitmap;  // 当前显示的 GPU 纹理
    int _srcWidth  = 0;                  // 原始图片尺寸（FitToWindow/RenderFrame 缩放计算用）
    int _srcHeight = 0;
    int _displayedLevel = -1;            // 当前纹理对应的金字塔层级
    ComPtr<ID2D1Bitmap1> _topLevelBitmap;  // 顶层预览 GPU 纹理，切层时 GPU 拉伸填充作占位

    // ── 异步切换：模糊占位 + 后台解顶层 ──
    // 大图（HEIF/RAW）顶层解码数百毫秒阻塞主线程，期间用缩略图/旧图模糊铺满视口
    ComPtr<ID2D1Bitmap1> _blurBitmap;     // 模糊占位 GPU 纹理（缩略图或上一张图）
    std::thread _bgThread;                // 后台顶层解码线程

    // BgState：后台解码线程的共享状态，shared_ptr 管理
    // 线程持有副本，Unload 时 detach 线程（不 join），通过 cancel 标志通知停止
    // 后台线程分两阶段：先 DecodeThumbnail（快）→ 更新 _blurBitmap，
    //                   再 DecodeLevel（慢）→ 更新 _sourceBitmap
    // 快速切换时旧线程在缩略图完成后检查 cancel，不进入重量级 DecodeLevel，避免 CPU 浪费
    struct BgResult {
        bool ready = false;
        int level = -1;
        std::vector<uint8_t> pixels;
        int width = 0, height = 0, stride = 0;
    };
    // 阶段0：文件打开结果（后台线程产出，主线程 ApplyBgResultIfReady 应用）
    // 把 FileMapping+FindDecoder+Open+Pyramid 从 UI 线程移到后台，消除缓存未命中时的同步阻塞
    struct BgOpenResult {
        bool ready = false;
        bool failed = false;          // FileMapping/FindDecoder/Open 失败
        std::shared_ptr<FileMapping>  fileMapping;
        std::shared_ptr<ImageDecoder> decoder;
        ImageDecoder::OpenResult      openResult;   // 含 info + state
        std::shared_ptr<Pyramid>      pyramid;
        int  topLevel = -1;
        bool supportsTiling = false;
    };
    struct BgState {
        std::atomic<bool> cancel{false};    // 取消标志：Unload/LoadFile 时置 true
        std::atomic<bool> pending{false};   // 异步解码进行中：OnTimer 据此触发重绘
        std::mutex mutex;                   // 保护 openResult/thumbResult/topResult
        BgOpenResult openResult;            // 阶段0：文件打开
        bool hasOpenStage = false;          // true=StartBgFullLoad 路径（需先应用阶段0）；StartBgTopLevelDecode 不置位
        bool openApplied = false;           // 主线程已将 openResult 应用到成员变量
        BgResult thumbResult;               // 阶段1：缩略图结果
        bool thumbApplied = false;          // 主线程已应用缩略图到 _blurBitmap
        BgResult topResult;                 // 阶段2：顶层解码结果
        int targetIndex = -1;               // 解码任务对应的图片索引，用于验证结果一致性
    };
    std::shared_ptr<BgState> _bgState;

    // ── 切换防抖：快速连续切换时延迟启动正式解码 ──
    // 每次 Navigate/LoadFile 重置 deadline，停手 200ms 后由 CheckInteractionTimeout 启动
    // 期间只显示缩略图/旧图占位，不做重量级 DecodeLevel，避免 CPU 浪费
    bool _bgDecodeScheduled = false;     // 有待启动的解码任务
    int _bgDecodeLevel = -1;             // 待解码的顶层 level
    bool _bgDecodeSkipThumb = false;     // 缓存命中时跳过缩略图阶段（缩略图已在 _blurBitmap）
    bool _bgCacheMiss = false;           // 缓存未命中：延迟到 CheckInteractionTimeout 执行完整 LoadFile
    int  _bgFwd = 1, _bgBwd = 1;         // 缓存未命中时暂存预解码参数（传给延迟执行的 LoadFile）
    std::wstring _bgDecodePendingPath;   // 延迟打开的文件路径（快速切换时跳过 FileMapping，停手后再打开）
    std::chrono::steady_clock::time_point _bgDecodeDeadline;

    // ── 预解码防抖：快速切换时不触发预解码，停手后才唤醒 worker 线程 ──
    // 避免 SetCurrentIndex 的 mutex 锁 + 文件列表复制 + worker 重启浪费
    bool _preDecodePending = false;
    int _preDecodeFwd = 0;
    int _preDecodeBwd = 0;
    std::chrono::steady_clock::time_point _preDecodeDeadline;

    // ── 延迟 UpdateViewport ──
    // ApplyBgResultIfReady 在 RenderFrame(BeginDraw/EndDraw 内)被调用，
    // 不能直接调 UpdateViewport→SwitchToLevel→CreateStretchedBitmap(含 BeginDraw)，
    // 否则嵌套 BeginDraw 触发 D2DERR_WRONG_STATE。改为设标志，由 OnTimer 在非绘制期执行
    bool _needsViewportUpdate = false;

    // ── TileWorker 资源保护 ──
    // Unload 置 true→清队列→等活跃 TileWorker 退出→reset 资源→置 false
    // TileWorker 取任务后检测此标志，避免访问已释放的 _decoder/_pyramid
    std::atomic<bool> _unloading{false};

    // ── 解码器 ──
    // shared_ptr：Unload 时成员 reset，但 TileWorker 持有本地副本保持资源存活
    // 避免Unload阻塞主线程等待TileWorker，同时防止TileWorker访问已释放资源
    std::shared_ptr<ImageDecoder> _decoder;
    ImageDecoder::OpenResult      _openResult;
    std::shared_ptr<FileMapping>  _fileMapping;  // 内存映射文件，按需分页加载

    // ── 瓦片调度器 ──
    struct TileTask {
        int level;
        int col;
        int row;
    };
    std::vector<std::thread> _tileThreads;  // 多线程并行解码瓦片
    std::mutex  _tileMutex;
    std::condition_variable _tileCV;
    std::queue<TileTask> _tileQueue;
    std::atomic<bool> _tileRunning{false};
    std::atomic<int> _activeTileWorkers{0};  // 正在解码瓦片的线程数，>0 时 _tileBusy=true
    std::atomic<int> _tileTotal{0};   // 当前批次瓦片总数（活动日志进度显示用）
    std::atomic<int> _tileDone{0};    // 当前批次已完成瓦片数
    // 多窗口下每窗口独立的瓦片状态标志（原全局 g_currentTileBusy/g_tileUpdated）
    std::atomic<bool> _tileBusy{false};     // 瓦片解码中：PreDecodeCache 让步
    std::atomic<bool> _tileUpdated{false};  // 瓦片已贴纹理待重绘，弥补队列清空后 busy=false 的时序漏洞

    // 待贴入 GPU 纹理的已解码瓦片：TileWorker 解码后存入，主线程 RenderFrame 处理
    // 避免后台线程操作 D2D 资源（单线程工厂）和 _sourceBitmap 生命周期竞态
    struct PendingTile {
        int level;
        int col;
        int row;
        int width;
        int height;
        int stride;
        std::vector<uint8_t> pixels;
        bool fromCache;  // true=缓存命中拷贝（RenderFrame 不 Put），false=新解码（Put 存缓存）
    };
    std::vector<PendingTile> _pendingTiles;
    std::mutex _pendingMutex;

    // 本层解码缓存：同一层多个瓦片共享一次整层解码，避免 DecodeTile 重复解整层
    // TileWorker 首次处理某层时 DecodeLevel 整层并缓存，后续瓦片直接 SubRegion 裁剪（零解码）
    // 大层（>4096²）不缓存避免爆内存，回退 DecodeTile
    // 多线程共享：shared_mutex 保护，读（SubRegion）并行，写（DecodeLevel）独占
    std::atomic<int> _cachedLevel{-1};
    std::wstring _cachedLevelPath;              // 绑定文件标识：跨图时 _cachedLevelPath != _currentPath 强制重解
    DecodeResult _cachedLevelResult;
    std::shared_mutex _cachedLevelMutex;

    // ── GIF 动画 ──
    AnimationPlayer _animPlayer;
    int             _lastAnimFrame = -1;

    // ── ICC 色彩管理 ──
    ColorManager    _colorManager;

    // ── 预解码缓存 ──
    PreDecodeCache* _preDecodeCache = nullptr;

    // ── 已解码图片缓存（GPU 纹理复用）──
    // 切换回已正式解码的图片时直接复用 GPU 纹理，跳过模糊预览和重复顶层解码
    // 淘汰：总缓存超 200MB 或单张超 3 分钟未访问
    struct DecodedCacheEntry {
        ComPtr<ID2D1Bitmap1> bitmap;   // 已解码 GPU 纹理（顶层+瓦片）
        int origWidth = 0;             // 原图尺寸
        int origHeight = 0;
        int level = -1;                // 纹理对应的金字塔层级
        int levelCount = 1;            // 金字塔总层数
        bool supportsTiling = false;
        bool hasFullQuality = false;   // true=含瓦片的高清纹理（切回无需重解），false=仅顶层低清
        std::chrono::steady_clock::time_point lastAccess;
        size_t bytes = 0;              // 估算显存占用 (w*h*4)
    };
    std::unordered_map<int, DecodedCacheEntry> _decodedCache;
    size_t _decodedCacheBytes = 0;
    // 300MB：原 200MB 存 ~24 张适窗纹理，300MB 存 ~36 张，回翻时减少重复解码
    // 现代设备 16GB+ 内存、2GB+ 显存，300MB 占比 <2%，无压力
    static constexpr size_t MAX_DECODED_CACHE_BYTES = 300 * 1024 * 1024;  // 300MB
    static constexpr int    DECODED_CACHE_EXPIRE_SEC = 180;               // 3 分钟未访问淘汰
    static constexpr size_t MAX_FULLQUALITY_PIXELS = 4 * 1024 * 1024;     // 4M 像素=16MB，超过则只缓存顶层
    bool _cacheFullQuality = false;  // 缓存命中且含高清瓦片：打开文件后跳过排瓦片

    void TileWorker();
    void EncodeTiles(int level);
    void RequestViewport(const Viewport& vp);

    // ── 异步顶层解码 ──
    // 启动后台线程解码顶层，主线程同步返回
    // skipThumb=true 跳过缩略图阶段（缓存命中时 _blurBitmap 已有缩略图）
    void StartBgTopLevelDecode(int level, bool skipThumb = false);
    // 全异步加载：FileMapping+Open+Pyramid+缩略图+顶层 全部在后台线程
    // 缓存未命中路径调用，UI 线程零阻塞（Open 50-150ms 不再卡 UI）
    void StartBgFullLoad(const std::wstring& path, int fwd, int bwd);
    // 主线程调用：检测后台结果就绪则上传 _sourceBitmap，清空 _blurBitmap，触发 UpdateViewport
    void ApplyBgResultIfReady();

    // ── 内部 ──
    // 旋转 delta 度（逆时针），以图片中心为原点，保持图片中心屏幕坐标 + 缩放不变
    void RotatePreservingCenter(int delta);
    // 统一坐标变换（含旋转分支）：屏幕↔原图互转，缩放/平移/瓦片视口共用
    // ImageToScreen = offset + ImageToScreenRel（offset 为纯屏幕平移项，与旋转无关）
    void ScreenToImage(double sx, double sy, double& ix, double& iy) const;
    void ImageToScreen(double ix, double iy, double& sx, double& sy) const;
    void ImageToScreenRel(double ix, double iy, double& rx, double& ry) const;
    void UpdateViewport();
    // 切层占位：GPU 拉伸顶层预览重建 _sourceBitmap 到目标层尺寸，不排瓦片
    // UpdateViewport 和 CheckInteractionTimeout 渐进过渡调用，避免频繁排队
    void SwitchToLevel(int level);
    // 排队视口可见瓦片，按距中心曼哈顿距离排序（中心先解码）
    void QueueTiles(int level);
    void LoadDirectoryFiles();

    // ── 预解码缓存 ──
    bool LoadFromCache(const std::wstring& path, int newIndex);
    void TriggerPreDecode(int fwd, int bwd);

    // ── 已解码图片缓存 ──
    void StoreDecodedCache();   // Unload 前保存当前 _sourceBitmap 到缓存
    bool LoadFromDecodedCache(const std::wstring& path, int newIndex);  // 命中则恢复
    void EvictDecodedCache();   // 淘汰超时/超量条目

    // ── 性能遥测：翻页两阶段延迟状态机 ──
    // Navigate 起点 → 占位帧（nav_to_placeholder_ms）/ 完整图（nav_to_full_ms）
    // 索引一致性验证：结果应用前 _navTargetIndex==_currentIndex，否则丢弃避免污染
    std::chrono::steady_clock::time_point _navStart;
    int  _navTargetIndex = -1;
    bool _navPlaceholderLogged = false;  // 占位帧已记
    bool _navFullLogged = false;         // 完整图已记
    // 上报翻页延迟：isFull=true 记 nav_to_full_ms，false 记 nav_to_placeholder_ms
    // 守卫 _navTargetIndex==_currentIndex 且未重复记，避免快速切换时旧任务结果污染
    void LogNavLatency(bool isFull);
};




