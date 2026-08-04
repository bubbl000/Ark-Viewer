#pragma once
#include <string>
#include <vector>

// ─── EXIF 元数据信息 ───
// 用于信息面板展示（AcdSee 风格，读不到的留空）
// 采用 label/value 行模型，渲染层只需遍历 fields 跳过空值，与字段数解耦
struct ExifInfo {
    // 单行：静态标签 + 已格式化值（值为空则该行不显示）
    struct Field {
        const wchar_t* label;   // 静态字面量，无需释放
        std::wstring   value;   // 已格式化（含单位/枚举翻译）
    };
    std::vector<Field> fields;  // 按显示顺序排列
    bool valid = false;         // 至少一个字段有值
};

// ─── EXIF 解析器 ───
// WIC MetadataQueryReader 通用提取，支持 JPEG/TIFF/PNG/HEIF 等 WIC 格式
// ARW 等 TIFF 系 RAW：System.Photo.* 常为空，改直查 EXIF IFD 原生路径补全字符串字段
class ExifParser {
public:
    // filePath: 图片完整路径
    // knownW/knownH: 调用方已知的原图尺寸（WIC 不支持的格式如 PSD 用此兜底），0=未知
    static ExifInfo Parse(const std::wstring& filePath, int knownW = 0, int knownH = 0);
};
