#include "FileMapping.h"
#include "Logger.h"

FileMapping::FileMapping(const std::wstring& path) {
    Open(path);
}

FileMapping::~FileMapping() {
    Close();
}

bool FileMapping::Open(const std::wstring& path) {
    Close();

    _hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (_hFile == INVALID_HANDLE_VALUE) {
        LOG_WARN("FileMapping", "CreateFile 失败");
        _hFile = nullptr;
        return false;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(_hFile, &fileSize)) {
        Close();
        return false;
    }
    _size = (size_t)fileSize.QuadPart;

    // 创建文件映射内核对象（PAGE_READONLY → 只读，OS 按需 Page In）
    _hMapping = CreateFileMappingW(_hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!_hMapping) {
        LOG_WARN("FileMapping", "CreateFileMapping 失败");
        Close();
        return false;
    }

    // 映射整个文件到虚拟地址空间（不占物理内存，访问时才 Page In）
    _view = MapViewOfFile(_hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!_view) {
        LOG_WARN("FileMapping", "MapViewOfFile 失败");
        Close();
        return false;
    }

    _data = (const uint8_t*)_view;
    LOG_INFO_STREAM("FileMapping") << "已映射: " << _size << " bytes";
    return true;
}

void FileMapping::Close() {
    if (_view)     { UnmapViewOfFile(_view); _view = nullptr; }
    if (_hMapping) { CloseHandle(_hMapping); _hMapping = nullptr; }
    if (_hFile)    { CloseHandle(_hFile);    _hFile = nullptr; }
    _data = nullptr;
    _size = 0;
}
