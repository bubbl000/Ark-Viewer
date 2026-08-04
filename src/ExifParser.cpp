#include "ExifParser.h"
#include "Logger.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <propidl.h>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// WIC 工厂单例（与 WicDecoder 同样用 STA，匹配项目 COM 初始化模式）
static IWICImagingFactory* GetWicFactory() {
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    return factory.Get();
}

// ── 文件大小格式化：自动 B/KB/MB ──
static std::wstring FormatSize(uintmax_t bytes) {
    std::wostringstream oss;
    if (bytes < 1024) {
        oss << bytes << L" B";
    } else if (bytes < 1024 * 1024) {
        oss.precision(1);
        oss << std::fixed << (double)bytes / 1024.0 << L" KB";
    } else {
        oss.precision(2);
        oss << std::fixed << (double)bytes / (1024.0 * 1024.0) << L" MB";
    }
    return oss.str();
}

// ── 曝光时间格式化：<1s 显示 1/xs，≥1s 显示 xs ──
static std::wstring FormatExposure(double sec) {
    if (sec <= 0) return L"";
    std::wostringstream oss;
    if (sec >= 1.0) {
        oss.precision(sec < 10 ? 1 : 0);
        oss << std::fixed << sec << L"s";
    } else {
        oss << L"1/" << (int)(1.0 / sec + 0.5) << L"s";
    }
    return oss.str();
}

// ── 文件修改时间格式化：YYYY-MM-DD HH:MM:SS ──
static std::wstring FormatFileTime(const FILETIME& ft) {
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) return L"";
    std::wostringstream oss;
    oss << st.wYear << L"-"
        << std::setw(2) << std::setfill(L'0') << st.wMonth << L"-"
        << std::setw(2) << std::setfill(L'0') << st.wDay << L" "
        << std::setw(2) << std::setfill(L'0') << st.wHour << L":"
        << std::setw(2) << std::setfill(L'0') << st.wMinute << L":"
        << std::setw(2) << std::setfill(L'0') << st.wSecond;
    return oss.str();
}

// 测光模式枚举翻译（EXIF MeteringMode）
static std::wstring MeteringName(int m) {
    switch (m) {
    case 1: return L"平均"; case 2: return L"中央重点"; case 3: return L"点";
    case 4: return L"多区"; case 5: return L"模式"; case 6: return L"局部";
    case 255: return L"其他"; default: return m > 0 ? std::to_wstring(m) : L"";
    }
}

// 曝光程序枚举翻译（EXIF ExposureProgram）
static std::wstring ExposureProgramName(int p) {
    switch (p) {
    case 1: return L"手动"; case 2: return L"程序"; case 3: return L"光圈优先";
    case 4: return L"快门优先"; case 5: return L"创意"; case 6: return L"运动";
    case 7: return L"人像"; case 8: return L"风景"; default: return p > 0 ? std::to_wstring(p) : L"";
    }
}

ExifInfo ExifParser::Parse(const std::wstring& filePath, int knownW, int knownH) {
    ExifInfo info;

    // ── 文件系统字段（始终可得，不依赖 WIC）──
    std::wstring fileName = fs::path(filePath).filename().wstring();
    std::wstring ext = fs::path(filePath).extension().wstring();
    if (!ext.empty()) {
        // 去点并转大写
        ext = ext.substr(1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towupper);
    }

    uintmax_t fileSize = 0;
    std::wstring modifyTime;
    std::error_code ec;
    auto status = fs::status(filePath, ec);
    if (!ec && fs::exists(status)) {
        fileSize = fs::file_size(filePath, ec);
        if (ec) fileSize = 0;
        // fs::last_write_time → FILETIME
        auto lwt = fs::last_write_time(filePath, ec);
        if (!ec) {
            const auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(lwt);
            const auto epoch = sctp.time_since_epoch();
            // __int64 自 1601-01-01 的 100ns 计数（FILETIME 语义）
            const auto intervals = std::chrono::duration_cast<
                std::chrono::duration<__int64, std::ratio<1, 10000000>>>(epoch).count();
            FILETIME ft;
            ft.dwLowDateTime = (DWORD)(intervals & 0xFFFFFFFF);
            ft.dwHighDateTime = (DWORD)(intervals >> 32);
            modifyTime = FormatFileTime(ft);
        }
    }

    info.fields.push_back({ L"文件名", fileName });
    info.fields.push_back({ L"类型", ext });
    info.fields.push_back({ L"大小", FormatSize(fileSize) });
    info.fields.push_back({ L"修改时间", modifyTime });

    // ── WIC 路径：尺寸 + EXIF ──
    auto* factory = GetWicFactory();
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICMetadataQueryReader> reader;
    int imgW = knownW, imgH = knownH;

    if (factory) {
        HRESULT hr = factory->CreateDecoderFromFilename(
            filePath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder);
        if (SUCCEEDED(hr) && decoder) {
            decoder->GetFrame(0, &frame);
        }
    }
    if (frame) {
        UINT w = 0, h = 0;
        if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0) {
            imgW = (int)w; imgH = (int)h;
        }
        frame->GetMetadataQueryReader(&reader);
    }

    // 尺寸字段（已知或 WIC 解出）
    if (imgW > 0 && imgH > 0) {
        info.fields.push_back({ L"尺寸", std::to_wstring(imgW) + L" × " + std::to_wstring(imgH) });
    } else {
        info.fields.push_back({ L"尺寸", L"" });
    }

    // 无元数据读取器：仅文件系统字段
    if (!reader) {
        for (auto& f : info.fields) if (!f.value.empty()) { info.valid = true; break; }
        return info;
    }

    // ── 查询辅助 ──
    // 属性路径查询（System.Photo.*）：WIC 已规范化为 double/字符串，JPEG/TIFF 通用
    auto queryPropString = [&](const wchar_t* name) -> std::wstring {
        PROPVARIANT pv = {};
        std::wstring r;
        if (SUCCEEDED(reader->GetMetadataByName(name, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
            r = pv.pwszVal;
        PropVariantClear(&pv);
        return r;
    };
    auto queryPropDouble = [&](const wchar_t* name) -> double {
        PROPVARIANT pv = {};
        double v = 0;
        if (SUCCEEDED(reader->GetMetadataByName(name, &pv))) {
            switch (pv.vt) {
            case VT_R8: v = pv.dblVal; break; case VT_R4: v = pv.fltVal; break;
            case VT_UI2: v = pv.uiVal; break; case VT_UI4: v = pv.ulVal; break;
            case VT_I2: v = pv.iVal; break; case VT_I4: v = pv.lVal; break;
            }
        }
        PropVariantClear(&pv);
        return v;
    };

    // 原生 IFD 字符串查询：尝试 TIFF 风格 /ifd/ 和 JPEG 风格 /app1/ifd/ 两种前缀
    // ARW 等 TIFF 系 RAW 的 System.Photo.* 常为空，直查 IFD 可靠取到 VT_LPWSTR
    // tagPath 形如 L"/{ushort=271}"（IFD0）或 L"/exif/{ushort=36867}"（Exif 子 IFD）
    auto queryIfdString = [&](const wchar_t* tagPath) -> std::wstring {
        for (auto prefix : { L"/ifd", L"/app1/ifd" }) {
            std::wstring full = std::wstring(prefix) + tagPath;
            PROPVARIANT pv = {};
            std::wstring r;
            if (SUCCEEDED(reader->GetMetadataByName(full.c_str(), &pv)) &&
                pv.vt == VT_LPWSTR && pv.pwszVal) {
                r = pv.pwszVal;
            }
            PropVariantClear(&pv);
            if (!r.empty()) return r;
        }
        return L"";
    };
    // 原生 IFD 数值查询（含 URational 解析）：System.Photo.* 为空时的 ARW 兜底
    // URational 在 WIC 中返回 VT_UNKNOWN（子查询读取器），解析 numerator/denominator
    auto queryIfdNumeric = [&](const wchar_t* tagPath) -> double {
        for (auto prefix : { L"/ifd", L"/app1/ifd" }) {
            std::wstring full = std::wstring(prefix) + tagPath;
            PROPVARIANT pv = {};
            double v = 0; bool ok = false;
            if (SUCCEEDED(reader->GetMetadataByName(full.c_str(), &pv))) {
                switch (pv.vt) {
                case VT_R8: v = pv.dblVal; ok = true; break;
                case VT_R4: v = pv.fltVal; ok = true; break;
                case VT_UI2: v = pv.uiVal; ok = true; break;
                case VT_UI4: v = pv.ulVal; ok = true; break;
                case VT_I2: v = pv.iVal; ok = true; break;
                case VT_I4: v = pv.lVal; ok = true; break;
                case VT_UNKNOWN: {
                    // URational：子读取器查 numerator/denominator
                    ComPtr<IWICMetadataQueryReader> sub;
                    if (pv.punkVal && SUCCEEDED(pv.punkVal->QueryInterface(IID_PPV_ARGS(&sub)))) {
                        PROPVARIANT num = {}, den = {};
                        if (SUCCEEDED(sub->GetMetadataByName(L"numerator", &num)) &&
                            SUCCEEDED(sub->GetMetadataByName(L"denominator", &den))) {
                            double n = 0, d = 0;
                            if (num.vt == VT_UI4) n = num.ulVal; else if (num.vt == VT_I4) n = num.lVal;
                            else if (num.vt == VT_UI2) n = num.uiVal;
                            if (den.vt == VT_UI4) d = den.ulVal; else if (den.vt == VT_I4) d = den.lVal;
                            else if (den.vt == VT_UI2) d = den.uiVal;
                            if (d != 0) { v = n / d; ok = true; }
                        }
                        PropVariantClear(&num); PropVariantClear(&den);
                    }
                    break;
                }
                }
            }
            PropVariantClear(&pv);
            if (ok) return v;
        }
        return 0;
    };

    // ── EXIF 字段（顺序按需求文档）──
    // 字符串类：优先原生 IFD（ARW 可靠），为空再试 System.Photo.*
    std::wstring dateTimeOrig = queryIfdString(L"/exif/{ushort=36867}");
    if (dateTimeOrig.empty()) dateTimeOrig = queryPropString(L"System.Photo.DateTaken");
    // EXIF 日期 "YYYY:MM:DD HH:MM:SS" → "YYYY-MM-DD HH:MM:SS"
    if (dateTimeOrig.size() >= 19 && dateTimeOrig[4] == L':') {
        dateTimeOrig[4] = L'-'; dateTimeOrig[7] = L'-';
    }
    info.fields.push_back({ L"拍摄时间", dateTimeOrig });

    std::wstring make = queryIfdString(L"/{ushort=271}");
    if (make.empty()) make = queryPropString(L"System.Photo.CameraManufacturer");
    info.fields.push_back({ L"厂商", make });

    std::wstring model = queryIfdString(L"/{ushort=272}");
    if (model.empty()) model = queryPropString(L"System.Photo.CameraModel");
    info.fields.push_back({ L"型号", model });

    std::wstring lens = queryIfdString(L"/exif/{ushort=42036}");
    if (lens.empty()) lens = queryPropString(L"System.Photo.LensModel");
    info.fields.push_back({ L"镜头", lens });

    // 数值类：优先 System.Photo.*（已规范化 double），为空再试原生 IFD rational
    double fNumber = queryPropDouble(L"System.Photo.FNumber");
    if (fNumber <= 0) fNumber = queryIfdNumeric(L"/exif/{ushort=33437}");
    {
        std::wostringstream oss;
        if (fNumber > 0) { oss.precision(1); oss << std::fixed << L"f/" << fNumber; }
        info.fields.push_back({ L"光圈", oss.str() });
    }

    double maxAp = queryPropDouble(L"System.Photo.MaxAperture");
    if (maxAp <= 0) maxAp = queryIfdNumeric(L"/exif/{ushort=37381}");
    {
        std::wostringstream oss;
        if (maxAp > 0) { oss.precision(1); oss << std::fixed << L"f/" << maxAp; }
        info.fields.push_back({ L"最大光圈", oss.str() });
    }

    double exposure = queryPropDouble(L"System.Photo.ExposureTime");
    if (exposure <= 0) exposure = queryIfdNumeric(L"/exif/{ushort=33434}");
    info.fields.push_back({ L"曝光时间", FormatExposure(exposure) });

    double bias = queryPropDouble(L"System.Photo.ExposureBias");
    if (bias == 0) bias = queryIfdNumeric(L"/exif/{ushort=37380}");
    {
        std::wostringstream oss;
        if (bias != 0) {
            oss.precision(1); oss << std::fixed;
            oss << (bias > 0 ? L"+" : L"") << bias << L" EV";
        }
        info.fields.push_back({ L"曝光补偿", oss.str() });
    }

    int iso = (int)queryPropDouble(L"System.Photo.ISOSpeed");
    if (iso <= 0) iso = (int)queryIfdNumeric(L"/exif/{ushort=34855}");
    info.fields.push_back({ L"ISO", iso > 0 ? std::to_wstring(iso) : L"" });

    double focal = queryPropDouble(L"System.Photo.FocalLength");
    if (focal <= 0) focal = queryIfdNumeric(L"/exif/{ushort=37386}");
    {
        std::wostringstream oss;
        if (focal > 0) { oss.precision(0); oss << std::fixed << focal << L"mm"; }
        info.fields.push_back({ L"焦距", oss.str() });
    }

    int metering = (int)queryPropDouble(L"System.Photo.MeteringMode");
    if (metering == 0) metering = (int)queryIfdNumeric(L"/exif/{ushort=37383}");
    info.fields.push_back({ L"测光模式", MeteringName(metering) });

    // Flash：位 0 = 是否闪光
    int flash = (int)queryPropDouble(L"System.Photo.Flash");
    if (flash == 0) flash = (int)queryIfdNumeric(L"/exif/{ushort=37385}");
    info.fields.push_back({ L"闪光灯", flash > 0 ? ((flash & 1) ? L"闪光" : L"未闪光") : L"" });

    int wb = (int)queryPropDouble(L"System.Photo.WhiteBalance");
    if (wb == 0) wb = (int)queryIfdNumeric(L"/exif/{ushort=41993}");
    info.fields.push_back({ L"白平衡", wb > 0 ? (wb == 1 ? L"手动" : L"自动") : L"" });

    double bright = queryPropDouble(L"System.Photo.Brightness");
    if (bright == 0) bright = queryIfdNumeric(L"/exif/{ushort=37379}");
    {
        std::wostringstream oss;
        if (bright != 0) { oss.precision(2); oss << std::fixed << bright; }
        info.fields.push_back({ L"亮度", oss.str() });
    }

    int prog = (int)queryPropDouble(L"System.Photo.ExposureProgram");
    if (prog == 0) prog = (int)queryIfdNumeric(L"/exif/{ushort=34850}");
    info.fields.push_back({ L"曝光程序", ExposureProgramName(prog) });

    // valid：至少一个字段有值
    for (auto& f : info.fields) {
        if (!f.value.empty()) { info.valid = true; break; }
    }
    return info;
}
