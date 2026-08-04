#include "NvjpegHardDecoder.h"
#include "Logger.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <memory>

#ifdef HAS_NVJPEG

// ─── nvjpeg + CUDA 类型声明（动态加载，不依赖头文件，无 CUDA Toolkit 也可编译） ───
// 句柄均为 opaque pointer
typedef struct nvjpegHandle*      nvjpegHandle_t;
typedef struct nvjpegJpegState*   nvjpegJpegState_t;
typedef struct nvjpegJpegStream*  nvjpegJpegStream_t;
typedef struct nvjpegDecodeParams* nvjpegDecodeParams_t;
typedef struct nvjpegJpegDecoder* nvjpegJpegDecoder_t;
typedef struct CUstream_st*       cudaStream_t;  // CUDA stream（opaque）

// 枚举值（从 nvjpeg.h / cuda_runtime.h 提取，用 int 承载）
enum {
    NVJPEG_STATUS_SUCCESS = 0
};
enum {
    NVJPEG_OUTPUT_BGRI = 6  // interleaved BGR，写入 channel[0]
};
enum {
    NVJPEG_SCALE_NONE     = 0,
    NVJPEG_SCALE_1_BY_2   = 1,
    NVJPEG_SCALE_1_BY_4   = 2,
    NVJPEG_SCALE_1_BY_8   = 3
};
enum {
    NVJPEG_BACKEND_DEFAULT = 0  // 让库自动选最佳后端（含硬件解码）
};
enum {
    cudaMemcpyDeviceToHost = 2
};

#define NVJPEG_MAX_COMPONENT 4
typedef struct {
    unsigned char* channel[NVJPEG_MAX_COMPONENT];
    size_t         pitch[NVJPEG_MAX_COMPONENT];
} nvjpegImage_t;

// ─── 函数指针类型 ───
typedef int (*fn_nvjpegCreateSimple)(nvjpegHandle_t*);
typedef int (*fn_nvjpegDestroy)(nvjpegHandle_t);
typedef int (*fn_nvjpegJpegStreamCreate)(nvjpegHandle_t, nvjpegJpegStream_t*);
typedef int (*fn_nvjpegJpegStreamDestroy)(nvjpegJpegStream_t);
typedef int (*fn_nvjpegJpegStreamParse)(nvjpegHandle_t, const unsigned char*, size_t, int, int, nvjpegJpegStream_t);
typedef int (*fn_nvjpegJpegStreamGetFrameDimensions)(nvjpegJpegStream_t, unsigned int*, unsigned int*);
typedef int (*fn_nvjpegDecodeParamsCreate)(nvjpegHandle_t, nvjpegDecodeParams_t*);
typedef int (*fn_nvjpegDecodeParamsDestroy)(nvjpegDecodeParams_t);
typedef int (*fn_nvjpegDecodeParamsSetOutputFormat)(nvjpegDecodeParams_t, int);
typedef int (*fn_nvjpegDecodeParamsSetScaleFactor)(nvjpegDecodeParams_t, int);
typedef int (*fn_nvjpegDecoderCreate)(nvjpegHandle_t, int, nvjpegJpegDecoder_t*);
typedef int (*fn_nvjpegDecoderDestroy)(nvjpegJpegDecoder_t);
typedef int (*fn_nvjpegDecoderJpegSupported)(nvjpegJpegDecoder_t, nvjpegJpegStream_t, nvjpegDecodeParams_t, int*);
typedef int (*fn_nvjpegJpegStateCreate)(nvjpegHandle_t, nvjpegJpegState_t*);
typedef int (*fn_nvjpegJpegStateDestroy)(nvjpegJpegState_t);
typedef int (*fn_nvjpegDecodeJpeg)(nvjpegHandle_t, nvjpegJpegDecoder_t, nvjpegJpegState_t,
    nvjpegJpegStream_t, nvjpegImage_t*, nvjpegDecodeParams_t, cudaStream_t);

typedef int (*fn_cudaMalloc)(void**, size_t);
typedef int (*fn_cudaFree)(void*);
typedef int (*fn_cudaMemcpy)(void*, const void*, size_t, int);
typedef int (*fn_cudaStreamCreate)(cudaStream_t*);
typedef int (*fn_cudaStreamDestroy)(cudaStream_t);
typedef int (*fn_cudaStreamSynchronize)(cudaStream_t);

// ─── NvjpegApi 单例：动态加载 + 全局 handle ───
struct NvjpegApi {
    HMODULE hNvjpeg = nullptr;
    HMODULE hCudart = nullptr;
    nvjpegHandle_t handle = nullptr;
    bool initialized = false;
    bool available = false;

    // nvjpeg 函数指针
    fn_nvjpegCreateSimple                  pfnCreateSimple = nullptr;
    fn_nvjpegDestroy                       pfnDestroy = nullptr;
    fn_nvjpegJpegStreamCreate              pfnStreamCreate = nullptr;
    fn_nvjpegJpegStreamDestroy             pfnStreamDestroy = nullptr;
    fn_nvjpegJpegStreamParse               pfnStreamParse = nullptr;
    fn_nvjpegJpegStreamGetFrameDimensions  pfnGetFrameDimensions = nullptr;
    fn_nvjpegDecodeParamsCreate            pfnParamsCreate = nullptr;
    fn_nvjpegDecodeParamsDestroy           pfnParamsDestroy = nullptr;
    fn_nvjpegDecodeParamsSetOutputFormat   pfnSetOutputFormat = nullptr;
    fn_nvjpegDecodeParamsSetScaleFactor    pfnSetScaleFactor = nullptr;
    fn_nvjpegDecoderCreate                 pfnDecoderCreate = nullptr;
    fn_nvjpegDecoderDestroy                pfnDecoderDestroy = nullptr;
    fn_nvjpegDecoderJpegSupported          pfnDecoderSupported = nullptr;
    fn_nvjpegJpegStateCreate               pfnStateCreate = nullptr;
    fn_nvjpegJpegStateDestroy              pfnStateDestroy = nullptr;
    fn_nvjpegDecodeJpeg                    pfnDecodeJpeg = nullptr;

    // cuda 函数指针
    fn_cudaMalloc                 pfnCudaMalloc = nullptr;
    fn_cudaFree                   pfnCudaFree = nullptr;
    fn_cudaMemcpy                 pfnCudaMemcpy = nullptr;
    fn_cudaStreamCreate           pfnStreamCreateCuda = nullptr;
    fn_cudaStreamDestroy          pfnStreamDestroyCuda = nullptr;
    fn_cudaStreamSynchronize      pfnStreamSync = nullptr;

    static NvjpegApi& Instance() {
        static NvjpegApi inst;
        EnsureInit(inst);
        return inst;
    }

private:
    // 懒加载：首次调用时尝试加载 DLL + 解析函数 + 创建 handle，结果缓存
    static void EnsureInit(NvjpegApi& inst) {
        if (inst.initialized) return;
        inst.initialized = true;  // 标记已尝试，避免重复 LoadLibrary

        inst.hNvjpeg = LoadLibraryW(L"nvjpeg64_12.dll");
        inst.hCudart = LoadLibraryW(L"cudart64_12.dll");
        if (!inst.hNvjpeg || !inst.hCudart) {
            LOG_INFO("NVJPEG", "DLL 加载失败，硬解禁用");
            return;
        }

        // 解析 nvjpeg 函数指针（任一缺失则禁用）
        inst.pfnCreateSimple         = (fn_nvjpegCreateSimple)GetProcAddress(inst.hNvjpeg, "nvjpegCreateSimple");
        inst.pfnDestroy              = (fn_nvjpegDestroy)GetProcAddress(inst.hNvjpeg, "nvjpegDestroy");
        inst.pfnStreamCreate         = (fn_nvjpegJpegStreamCreate)GetProcAddress(inst.hNvjpeg, "nvjpegJpegStreamCreate");
        inst.pfnStreamDestroy        = (fn_nvjpegJpegStreamDestroy)GetProcAddress(inst.hNvjpeg, "nvjpegJpegStreamDestroy");
        inst.pfnStreamParse          = (fn_nvjpegJpegStreamParse)GetProcAddress(inst.hNvjpeg, "nvjpegJpegStreamParse");
        inst.pfnGetFrameDimensions   = (fn_nvjpegJpegStreamGetFrameDimensions)GetProcAddress(inst.hNvjpeg, "nvjpegJpegStreamGetFrameDimensions");
        inst.pfnParamsCreate         = (fn_nvjpegDecodeParamsCreate)GetProcAddress(inst.hNvjpeg, "nvjpegDecodeParamsCreate");
        inst.pfnParamsDestroy        = (fn_nvjpegDecodeParamsDestroy)GetProcAddress(inst.hNvjpeg, "nvjpegDecodeParamsDestroy");
        inst.pfnSetOutputFormat      = (fn_nvjpegDecodeParamsSetOutputFormat)GetProcAddress(inst.hNvjpeg, "nvjpegDecodeParamsSetOutputFormat");
        inst.pfnSetScaleFactor       = (fn_nvjpegDecodeParamsSetScaleFactor)GetProcAddress(inst.hNvjpeg, "nvjpegDecodeParamsSetScaleFactor");
        inst.pfnDecoderCreate        = (fn_nvjpegDecoderCreate)GetProcAddress(inst.hNvjpeg, "nvjpegDecoderCreate");
        inst.pfnDecoderDestroy       = (fn_nvjpegDecoderDestroy)GetProcAddress(inst.hNvjpeg, "nvjpegDecoderDestroy");
        inst.pfnDecoderSupported     = (fn_nvjpegDecoderJpegSupported)GetProcAddress(inst.hNvjpeg, "nvjpegDecoderJpegSupported");
        inst.pfnStateCreate          = (fn_nvjpegJpegStateCreate)GetProcAddress(inst.hNvjpeg, "nvjpegJpegStateCreate");
        inst.pfnStateDestroy         = (fn_nvjpegJpegStateDestroy)GetProcAddress(inst.hNvjpeg, "nvjpegJpegStateDestroy");
        inst.pfnDecodeJpeg           = (fn_nvjpegDecodeJpeg)GetProcAddress(inst.hNvjpeg, "nvjpegDecodeJpeg");

        inst.pfnCudaMalloc           = (fn_cudaMalloc)GetProcAddress(inst.hCudart, "cudaMalloc");
        inst.pfnCudaFree             = (fn_cudaFree)GetProcAddress(inst.hCudart, "cudaFree");
        inst.pfnCudaMemcpy           = (fn_cudaMemcpy)GetProcAddress(inst.hCudart, "cudaMemcpy");
        inst.pfnStreamCreateCuda     = (fn_cudaStreamCreate)GetProcAddress(inst.hCudart, "cudaStreamCreate");
        inst.pfnStreamDestroyCuda    = (fn_cudaStreamDestroy)GetProcAddress(inst.hCudart, "cudaStreamDestroy");
        inst.pfnStreamSync           = (fn_cudaStreamSynchronize)GetProcAddress(inst.hCudart, "cudaStreamSynchronize");

        // 校验关键函数指针非空
        if (!inst.pfnCreateSimple || !inst.pfnDecodeJpeg || !inst.pfnCudaMalloc
            || !inst.pfnCudaMemcpy || !inst.pfnStreamSync) {
            LOG_WARN("NVJPEG", "函数指针解析不完整，硬解禁用");
            return;
        }

        // 创建全局 handle：无 N 卡 GPU 时 nvjpegCreateSimple 失败
        if (inst.pfnCreateSimple(&inst.handle) != NVJPEG_STATUS_SUCCESS) {
            LOG_INFO("NVJPEG", "nvjpegCreateSimple 失败（无 N 卡 GPU），硬解禁用");
            inst.handle = nullptr;
            return;
        }

        inst.available = true;
        LOG_INFO("NVJPEG", "硬解初始化成功，JPEG 将使用 nvJPEG GPU 解码");
    }
};

// ─── 每线程独立状态（nvjpeg 文档要求 bitstream/state/params 句柄每线程独立） ───
struct ThreadState {
    nvjpegJpegStream_t   stream = nullptr;
    nvjpegDecodeParams_t params = nullptr;
    nvjpegJpegState_t    state = nullptr;
    nvjpegJpegDecoder_t  decoder = nullptr;
    cudaStream_t         cudaStream = nullptr;
    bool valid = false;
    // 析构不调用 destroy：避免 NvjpegApi 单例已销毁的竞态（进程退出由 OS 清理）
    ~ThreadState() = default;
};
static thread_local std::unique_ptr<ThreadState> g_tls;

// 首次调用时惰性创建线程级句柄
static ThreadState& Tls() {
    if (!g_tls) {
        auto& api = NvjpegApi::Instance();
        auto ts = std::make_unique<ThreadState>();
        bool ok = true;
        ok = ok && api.pfnStreamCreate(api.handle, &ts->stream) == NVJPEG_STATUS_SUCCESS;
        ok = ok && api.pfnParamsCreate(api.handle, &ts->params) == NVJPEG_STATUS_SUCCESS;
        ok = ok && api.pfnStateCreate(api.handle, &ts->state) == NVJPEG_STATUS_SUCCESS;
        ok = ok && api.pfnDecoderCreate(api.handle, NVJPEG_BACKEND_DEFAULT, &ts->decoder) == NVJPEG_STATUS_SUCCESS;
        ok = ok && api.pfnStreamCreateCuda(&ts->cudaStream) == 0;  // cudaSuccess=0
        ts->valid = ok;
        if (!ok) LOG_WARN("NVJPEG", "线程级句柄创建失败，该线程硬解禁用");
        g_tls = std::move(ts);
    }
    return *g_tls;
}

// level → nvjpeg scale factor
static int LevelToScale(int level) {
    switch (level) {
        case 0: return NVJPEG_SCALE_NONE;
        case 1: return NVJPEG_SCALE_1_BY_2;
        case 2: return NVJPEG_SCALE_1_BY_4;
        default: return NVJPEG_SCALE_1_BY_8;  // level >= 3
    }
}

bool NvjpegHardDecoder::Available() {
    return NvjpegApi::Instance().available;
}

std::optional<DecodeResult> NvjpegHardDecoder::DecodeFull(const uint8_t* data, size_t len) {
    return DecodeLevel(data, len, 0);  // scale=NONE 复用 DecodeLevel 路径
}

std::optional<DecodeResult> NvjpegHardDecoder::DecodeLevel(const uint8_t* data, size_t len, int level) {
    if (!Available()) return {};
    auto& api = NvjpegApi::Instance();
    auto& tls = Tls();
    if (!tls.valid) return {};

    // 1. 解析 JPEG 流（表 + 头，不解码）
    if (api.pfnStreamParse(api.handle, data, len, 1, 0, tls.stream) != NVJPEG_STATUS_SUCCESS) {
        LOG_WARN("NVJPEG", "JpegStreamParse 失败");
        return {};
    }

    // 2. 检测硬解支持（渐进 JPEG / 410/411 子采样可能不支持）
    int supported = 0;
    if (api.pfnDecoderSupported(tls.decoder, tls.stream, tls.params, &supported) != NVJPEG_STATUS_SUCCESS
        || !supported) {
        return {};  // 静默回退，常见情况不打日志
    }

    // 3. 设置输出格式 + scale factor
    if (api.pfnSetOutputFormat(tls.params, NVJPEG_OUTPUT_BGRI) != NVJPEG_STATUS_SUCCESS) return {};
    if (api.pfnSetScaleFactor(tls.params, LevelToScale(level)) != NVJPEG_STATUS_SUCCESS) return {};

    // 4. 查询原始尺寸，计算 scale 后的输出尺寸
    //    尺寸公式与 JpegDecoder turbojpeg 一致：ceil(origW / 2^tjLevel)
    unsigned int origW = 0, origH = 0;
    if (api.pfnGetFrameDimensions(tls.stream, &origW, &origH) != NVJPEG_STATUS_SUCCESS) return {};
    int tjLevel = (std::min)(level, 3);
    int scaledW = (std::max)(1, (int)((origW + (1u << tjLevel) - 1) >> tjLevel));
    int scaledH = (std::max)(1, (int)((origH + (1u << tjLevel) - 1) >> tjLevel));

    // 5. 分配 GPU 输出 buffer（pitch 256 对齐，BGRI = 4 字节/像素）
    size_t pitch = ((scaledW * 4 + 255) / 256) * 256;
    void* dBuf = nullptr;
    if (api.pfnCudaMalloc(&dBuf, pitch * scaledH) != 0) {
        LOG_WARN("NVJPEG", "cudaMalloc 失败（GPU 显存不足）");
        return {};
    }
    nvjpegImage_t img{};
    img.channel[0] = (unsigned char*)dBuf;
    img.pitch[0] = pitch;

    // 6. 解码 + 同步
    int st = api.pfnDecodeJpeg(api.handle, tls.decoder, tls.state,
        tls.stream, &img, tls.params, tls.cudaStream);
    if (st != NVJPEG_STATUS_SUCCESS) {
        api.pfnCudaFree(dBuf);
        LOG_WARN_STREAM("NVJPEG") << "DecodeJpeg 失败 status=" << st;
        return {};
    }
    api.pfnStreamSync(tls.cudaStream);

    // 7. 按行回拷到 CPU（pitch 可能 != width*4）
    DecodeResult result;
    result.width = scaledW;
    result.height = scaledH;
    result.stride = scaledW * 4;
    result.pixels.resize((size_t)scaledW * scaledH * 4);
    for (int y = 0; y < scaledH; y++) {
        api.pfnCudaMemcpy(
            result.pixels.data() + (size_t)y * result.stride,
            (unsigned char*)dBuf + (size_t)y * pitch,
            (size_t)scaledW * 4,
            cudaMemcpyDeviceToHost);
    }
    api.pfnCudaFree(dBuf);

    // 8. BGRI→BGRA：JPEG 无 alpha，每像素第4字节填 255
    for (size_t i = 3; i < result.pixels.size(); i += 4) result.pixels[i] = 255;

    // 9. level > 3 时 nvjpeg 只能 1/8，CPU 端再缩放到目标尺寸
    if (tjLevel < level) {
        int targetW = (std::max)(1, (int)origW >> level);
        int targetH = (std::max)(1, (int)origH >> level);
        result = result.ScaleDown(targetW, targetH);
    }
    return result;
}

#else  // HAS_NVJPEG 未启用：空 stub，硬解功能不可用

bool NvjpegHardDecoder::Available() { return false; }
std::optional<DecodeResult> NvjpegHardDecoder::DecodeFull(const uint8_t*, size_t) { return {}; }
std::optional<DecodeResult> NvjpegHardDecoder::DecodeLevel(const uint8_t*, size_t, int) { return {}; }

#endif  // HAS_NVJPEG
