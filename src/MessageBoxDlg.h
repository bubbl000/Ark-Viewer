#pragma once
#include <Windows.h>
#include <string>

// 自绘消息弹窗（深色主题，与主界面样式统一）
// 替代系统 MessageBoxW：深色背景 + accent 绿按钮 + 微软雅黑
// 支持：OK 提示 / 是-否确认 两种模式
namespace MessageBoxDlg {

// 显示自绘提示弹窗，返回 IDOK / IDYES / IDNO / IDCANCEL
// hwndOwner 可为 nullptr（无主窗口时）
int Show(HWND hwndOwner, const std::wstring& text, const std::wstring& title,
         bool yesNo = false);  // true=是/否；false=确定

}
