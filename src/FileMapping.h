#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <string>

// RAII 封装 Windows 内存映射文件（Memory-Mapped File）
// 大文件按需分页加载，避免全量读入 vector<uint8_t> 导致内存爆炸
class FileMapping {
public:
    FileMapping() = default;
    explicit FileMapping(const std::wstring& path);
    ~FileMapping();

    FileMapping(const FileMapping&) = delete;
    FileMapping& operator=(const FileMapping&) = delete;

    // 打开文件并建立映射，失败返回 false
    bool Open(const std::wstring& path);
    void Close();

    // 映射数据指针（失败时为 nullptr）
    const uint8_t* Data() const { return _data; }
    size_t Size() const { return _size; }

private:
    HANDLE _hFile    = nullptr;
    HANDLE _hMapping = nullptr;
    void*  _view     = nullptr;
    const uint8_t* _data = nullptr;
    size_t _size     = 0;
};
