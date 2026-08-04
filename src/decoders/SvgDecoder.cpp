#include "SvgDecoder.h"
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <mutex>
#include <wrl/client.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "Logger.h"
#include "miniz.h"
#include "../FileMapping.h"

using Microsoft::WRL::ComPtr;

// ─── D2D 资源单例（避免每次解码重建 D3D 设备） ───
struct D2DResources {
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1DeviceContext5> ctx5;
};

// D2D 工厂以 SINGLE_THREADED 创建，ctx5 不允许跨线程并发使用
// 后台解码线程与主线程可能同时 DecodeFull，用互斥锁串行化 D2D 上下文访问
static std::mutex g_d2dMutex;

static D2DResources* GetD2DResources() {
    static D2DResources res;
    if (res.ctx5) return &res;

    // 创建 D2D 工厂
    D2D1_FACTORY_OPTIONS opts = {};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &opts, (void**)&res.factory)))
        return nullptr;

    // 创建 D3D11 设备（BGRA 支持是 D2D 必需的）
    // 硬件驱动失败时回退 WARP 软件光栅：缩略图 DLL 跑在 dllhost，
    // 可能无 GPU 上下文或受限令牌，软件光栅保证 SVG 仍可渲染
    ComPtr<ID3D11Device> d3dDevice;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &d3dDevice, &fl, nullptr))) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3dDevice, &fl, nullptr)))
            return nullptr;
    }

    // D3D → DXGI → D2D 设备
    ComPtr<IDXGIDevice> dxgiDevice;
    d3dDevice.As(&dxgiDevice);
    ComPtr<ID2D1Device> d2dDevice;
    if (FAILED(res.factory->CreateDevice(dxgiDevice.Get(), &d2dDevice)))
        return nullptr;

    // 创建设备上下文并升级到 ID2D1DeviceContext5
    ComPtr<ID2D1DeviceContext> ctx;
    if (FAILED(d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx)))
        return nullptr;
    ctx->SetDpi(96.0f, 96.0f);

    if (FAILED(ctx.As(&res.ctx5)))
        return nullptr;

    return &res;
}

// ─── 从 SVG 文本解析 intrinsic 尺寸 ───
// 查找 <svg ...> 标签中的 width/height/viewBox 属性
static void ParseSvgSize(const uint8_t* data, size_t len, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    // 转为字符串方便搜索
    std::string text((const char*)data, len < 8192 ? len : 8192);

    // 查找 <svg 标签
    size_t svgPos = text.find("<svg");
    if (svgPos == std::string::npos) return;

    // 查找 > 结束标签
    size_t tagEnd = text.find('>', svgPos);
    if (tagEnd == std::string::npos) return;

    std::string tag = text.substr(svgPos, tagEnd - svgPos);

    // 查找 width=
    auto findAttr = [&](const std::string& attr) -> std::string {
        size_t pos = tag.find(attr);
        if (pos == std::string::npos) return "";
        pos += attr.size();
        // 跳过空格和引号
        while (pos < tag.size() && (tag[pos] == ' ' || tag[pos] == '=' ||
               tag[pos] == '"' || tag[pos] == '\''))
            pos++;
        std::string val;
        while (pos < tag.size() && tag[pos] != '"' && tag[pos] != '\'' &&
               tag[pos] != ' ' && tag[pos] != '>')
            val += tag[pos++];
        return val;
    };

    // 解析数值（去掉 px/cm 等单位）
    auto parseNum = [](const std::string& s) -> int {
        if (s.empty()) return 0;
        char* end = nullptr;
        double v = strtod(s.c_str(), &end);
        return (int)v;
    };

    int w = parseNum(findAttr("width"));
    int h = parseNum(findAttr("height"));

    if (w <= 0 || h <= 0) {
        // 尝试 viewBox="x y w h"
        std::string vb = findAttr("viewBox");
        if (vb.empty()) vb = findAttr("viewbox");
        if (!vb.empty()) {
            // 解析 4 个数字
            int idx = 0;
            int vals[4] = {0, 0, 0, 0};
            const char* p = vb.c_str();
            char* end;
            while (idx < 4 && *p) {
                vals[idx++] = (int)strtod(p, &end);
                p = end;
                while (*p && (*p == ' ' || *p == ',')) p++;
            }
            if (vals[2] > 0 && vals[3] > 0) {
                w = vals[2];
                h = vals[3];
            }
        }
    }

    if (w <= 0 || h <= 0) {
        w = 1024;
        h = 1024;
    }

    outW = w;
    outH = h;
}

// ─── gzip 解压（SVGZ 支持） ───
// gzip 格式：10 字节头 + 可选额外字段 + raw DEFLATE + CRC32 + 原始大小
static std::vector<uint8_t> GunzipSvgz(const uint8_t* data, size_t len) {
    if (len < 18 || data[0] != 0x1F || data[1] != 0x8B) return {};

    // 跳过 gzip 头
    size_t pos = 10;
    uint8_t flags = data[3];
    // FEXTRA
    if (flags & 0x04) {
        if (pos + 2 > len) return {};
        size_t xlen = data[pos] | (data[pos + 1] << 8);
        pos += 2 + xlen;
    }
    // FNAME
    if (flags & 0x08) {
        while (pos < len && data[pos] != 0) pos++;
        pos++;
    }
    // FCOMMENT
    if (flags & 0x10) {
        while (pos < len && data[pos] != 0) pos++;
        pos++;
    }
    // FHCRC
    if (flags & 0x02) pos += 2;

    if (pos >= len) return {};

    // raw DEFLATE 数据从 pos 到 len-8（末尾 4 字节 CRC32 + 4 字节原始大小）
    size_t deflateLen = (len > 8 + pos) ? len - 8 - pos : len - pos;

    // 先尝试解压到 4x 原始大小，不够再扩
    size_t outCap = deflateLen * 6;
    std::vector<uint8_t> out(outCap);

    mz_stream stream = {};
    stream.next_in = data + pos;
    stream.avail_in = (mz_uint32)deflateLen;
    stream.next_out = out.data();
    stream.avail_out = (mz_uint32)outCap;

    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        return {};

    int status = mz_inflate(&stream, MZ_FINISH);
    mz_inflateEnd(&stream);

    if (status != MZ_STREAM_END && status != MZ_OK)
        return {};

    out.resize(stream.total_out);
    return out;
}

// ─── Open：魔数检测 ───
std::optional<ImageDecoder::OpenResult> SvgDecoder::Open(const uint8_t* data, size_t len) {
    // SVGZ: gzip 魔数 1F 8B
    if (len >= 2 && data[0] == 0x1F && data[1] == 0x8B) {
        OpenResult r; r.info.format = "SVGZ"; r.info.decoderName = "Direct2D SVG";
        return r;
    }
    // SVG: 搜索前 4096 字节中的 <svg 标签
    size_t scanLen = (len < 4096) ? len : 4096;
    for (size_t i = 0; i + 3 < scanLen; i++) {
        if (data[i] == '<' && data[i + 1] == 's' &&
            data[i + 2] == 'v' && data[i + 3] == 'g') {
            OpenResult r; r.info.format = "SVG"; r.info.decoderName = "Direct2D SVG";
            return r;
        }
    }
    return std::nullopt;
}

// ─── DecodeFull：渲染 SVG 到 BGRA8 ───
std::optional<DecodeResult> SvgDecoder::DecodeFull(const OpenResult& open) {
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    // SVGZ → 先解压
    std::vector<uint8_t> svgData;
    const uint8_t* svgPtr = fileMap->Data();
    size_t svgLen = fileMap->Size();

    if (svgLen >= 2 && svgPtr[0] == 0x1F && svgPtr[1] == 0x8B) {
        svgData = GunzipSvgz(svgPtr, svgLen);
        if (svgData.empty()) {
            LOG_WARN("SvgDecoder", "SVGZ 解压失败");
            return std::nullopt;
        }
        svgPtr = svgData.data();
        svgLen = svgData.size();
    }

    // 解析 intrinsic 尺寸
    int width = 0, height = 0;
    ParseSvgSize(svgPtr, svgLen, width, height);
    if (width <= 0 || height <= 0) {
        LOG_WARN("SvgDecoder", "无法解析 SVG 尺寸");
        return std::nullopt;
    }

    // 持锁覆盖 GetD2DResources 初始化 + 整个 D2D 渲染过程
    // 首次并发调用时两线程同时看到 ctx5==nullptr，并发创建 D3D 设备并写
    // res.factory/res.ctx5（ComPtr 并发赋值）导致内存损坏；锁内串行化解决
    std::lock_guard<std::mutex> lk(g_d2dMutex);

    // 获取 D2D 设备上下文5（SVG 渲染需要 ID2D1DeviceContext5）
    auto* res = GetD2DResources();
    if (!res || !res->ctx5) {
        LOG_WARN("SvgDecoder", "无法创建 D2D 资源");
        return std::nullopt;
    }
    auto* ctx5 = res->ctx5.Get();

    // 创建 D2D 目标位图（GPU 渲染目标）
    D2D1_BITMAP_PROPERTIES1 targetProps = {};
    targetProps.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    targetProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    targetProps.dpiX = 96.0f; targetProps.dpiY = 96.0f;
    D2D1_SIZE_U size = D2D1::SizeU((UINT32)width, (UINT32)height);
    ComPtr<ID2D1Bitmap1> targetBitmap;
    if (FAILED(ctx5->CreateBitmap(size, nullptr, 0, &targetProps, &targetBitmap))) {
        LOG_WARN("SvgDecoder", "CreateBitmap (target) 失败");
        return std::nullopt;
    }
    ctx5->SetTarget(targetBitmap.Get());

    // 从 SVG 数据创建 IStream（CreateSvgDocument 需要 IStream 输入）
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, svgLen);
    if (!hMem) { ctx5->SetTarget(nullptr); return std::nullopt; }
    void* memPtr = GlobalLock(hMem);
    if (!memPtr) { GlobalFree(hMem); ctx5->SetTarget(nullptr); return std::nullopt; }
    std::memcpy(memPtr, svgPtr, svgLen);
    GlobalUnlock(hMem);
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &stream))) {
        GlobalFree(hMem); ctx5->SetTarget(nullptr); return std::nullopt;
    }

    // 创建 SVG 文档并渲染到目标位图
    ComPtr<ID2D1SvgDocument> svgDoc;
    D2D1_SIZE_F viewport = D2D1::SizeF((float)width, (float)height);
    HRESULT hr = ctx5->CreateSvgDocument(stream.Get(), viewport, &svgDoc);
    if (FAILED(hr)) {
        LOG_WARN_STREAM("SvgDecoder") << "CreateSvgDocument 失败: 0x" << std::hex << hr;
        ctx5->SetTarget(nullptr);
        return std::nullopt;
    }

    ctx5->BeginDraw();
    ctx5->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));  // 透明背景
    ctx5->DrawSvgDocument(svgDoc.Get());
    ctx5->EndDraw();

    // 创建 CPU 可读位图，从目标位图拷贝像素后映射读取
    D2D1_BITMAP_PROPERTIES1 readProps = {};
    readProps.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    readProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    readProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    readProps.dpiX = 96.0f; readProps.dpiY = 96.0f;
    ComPtr<ID2D1Bitmap1> readBitmap;
    if (FAILED(ctx5->CreateBitmap(size, nullptr, 0, &readProps, &readBitmap)) ||
        FAILED(readBitmap->CopyFromBitmap(nullptr, targetBitmap.Get(), nullptr))) {
        LOG_WARN("SvgDecoder", "创建读回位图失败");
        ctx5->SetTarget(nullptr);
        return std::nullopt;
    }

    D2D1_MAPPED_RECT mapped = {};
    if (FAILED(readBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
        LOG_WARN("SvgDecoder", "Map 失败");
        ctx5->SetTarget(nullptr);
        return std::nullopt;
    }

    DecodeResult result;
    result.width = width;
    result.height = height;
    result.stride = width * 4;
    result.pixels.resize((size_t)width * height * 4);

    // 逐行拷贝（mapped.pitch 可能不等于 width*4）
    int rowBytes = width * 4;
    for (int y = 0; y < height; y++) {
        std::memcpy(result.pixels.data() + (size_t)y * rowBytes,
                    mapped.bits + (size_t)y * mapped.pitch, rowBytes);
    }

    readBitmap->Unmap();
    ctx5->SetTarget(nullptr);  // 恢复单例上下文状态

    LOG_INFO_STREAM("SvgDecoder") << "已渲染: " << width << "x" << height;
    return result;
}
