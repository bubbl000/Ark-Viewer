#pragma once
#include <string>

// 文件格式关联模块：将图片格式注册为 ArkViewer2 默认打开程序
// 全部写 HKCU（用户级，无需管理员）；主路径用 IApplicationAssociationRegistration::SetAppAsDefault
namespace FileAssoc {
    // ProgId 名（HKCU\Software\Classes\ArkViewer2.Image）
    inline constexpr const wchar_t* PROG_ID = L"ArkViewer2.Image";

    // 确保 ProgId 已注册（幂等）：
    //   ArkViewer2.Image\(默认) = "Ark Viewer 2 Image"
    //   ArkViewer2.Image\DefaultIcon = "<exe>,0"
    //   ArkViewer2.Image\shell\open\command = "\"<exe>\" \"%1\""
    void EnsureProgId();

    // 查询 .ext 当前是否已关联到本程序（查 OpenWithProgids 含 PROG_ID）
    // ext 须含点，如 L".jpg"
    bool IsAssociated(const std::wstring& ext);

    // 关联 .ext：EnsureProgId + SetAppAsDefault；UserChoice hash 拦截时兜底写 OpenWithProgids
    bool Associate(const std::wstring& ext);

    // 取消关联：删除 OpenWithProgids 中的 PROG_ID 值
    bool Unassociate(const std::wstring& ext);
}
