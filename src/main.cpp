// Ark Viewer 2
// 纯 C++ · Win32 · Direct2D · DirectUI · 原生解码
// 架构方案: （Win32 + DirectUI + Direct2D + native decoder）
// 多窗口：单实例 + IPC 路由（双击图片转发到已有进程开新窗口）+ 拖入分流

#include "PlatformWindow.h"
#include "WindowManager.h"
#include "ActivityLog.h"
#include "Config.h"
#include "ImageDecoder.h"
#include "DecoderFactory.h"
#include "decoders/HdrDecoder.h"
#include "decoders/HeifDecoder.h"
#include "decoders/JpegDecoder.h"
#include "decoders/PngDecoder.h"
#include "decoders/PsdDecoder.h"
#include "decoders/RawDecoder.h"
#include "decoders/SvgDecoder.h"
#include "decoders/WebpDecoder.h"
#include "decoders/WicDecoder.h"
#include "decoders/NvjpegDecoder.h"
// 注：PngDecoder 当前为 stub（DecodeFull/DecodeLevel 返回空），暂不注册
// PNG 由 WicDecoder 兜底解码，避免 stub 拦截后无图可显示
#include <Windows.h>
#include <shellapi.h>  // CommandLineToArgvW
#include <ole2.h>      // OleInitialize / OleUninitialize（OLE 拖拽 DoDragDrop 必需）
#include <commctrl.h>  // InitCommonControlsEx（Tab/ListView 公共控件）
#include "Logger.h"

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

// 单实例内核对象名（跨进程检测是否已有实例运行）
static const wchar_t* SINGLE_INSTANCE_MUTEX = L"ArkViewer2_SingleInstance";
// 主窗口类名（IPC FindWindow 用，须与 PlatformWindow::Create 中 CLASS_NAME 一致）
static const wchar_t* MAIN_WINDOW_CLASS = L"ArkViewer2Window";

// 解析命令行首个文件路径（文件关联双击启动时为 argv[1]）
static std::wstring ParseCommandLinePath() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return L"";
    std::wstring path = (argc >= 2) ? argv[1] : L"";
    LocalFree(argv);
    return path;
}

// 注册全部解码器（顺序即优先级：专用格式优先，native 次之，WIC 兜底）
static void RegisterAllDecoders() {
    RegisterDecoder(+[]() -> std::unique_ptr<ImageDecoder> { return std::make_unique<HeifDecoder>(); });
    RegisterDecoder(+[]() -> std::unique_ptr<ImageDecoder> { return std::make_unique<RawDecoder>(); });
    RegisterDecoder(+[]() -> std::unique_ptr<ImageDecoder> { return std::make_unique<PsdDecoder>(); });
    RegisterDecoder(+[]() -> std::unique_ptr<ImageDecoder> { return std::make_unique<SvgDecoder>(); });
    RegisterDecoder(+[]() -> std::unique_ptr<ImageDecoder> { return std::make_unique<HdrDecoder>(); });
    RegisterDecoder(+[]() -> std::unique_ptr<ImageDecoder> { return std::make_unique<JpegDecoder>(); });
    // N卡 JPEG 硬解：硬解不可用时 Open 返回 nullopt，自动落到 WicDecoder
    RegisterDecoder(+[]() -> std::unique_ptr<ImageDecoder> { return std::make_unique<NvjpegDecoder>(); });
    RegisterDecoder(+[]() -> std::unique_ptr<ImageDecoder> { return std::make_unique<WebpDecoder>(); });
    RegisterDecoder(+[]() -> std::unique_ptr<ImageDecoder> { return std::make_unique<WicDecoder>(); });
}

// 已有实例运行时：把文件路径通过 WM_COPYDATA 发给主窗口，由其新建窗口显示
// 启动竞态：首个实例主窗口可能尚未创建，短暂重试
static void SendPathToRunningInstance(const std::wstring& path) {
    for (int i = 0; i < 10; ++i) {
        HWND hWnd = FindWindowW(MAIN_WINDOW_CLASS, nullptr);
        if (hWnd) {
            COPYDATASTRUCT cds = {};
            cds.dwData = IPC_OPEN_FILE_ID;
            cds.cbData = (DWORD)((path.size() + 1) * sizeof(wchar_t));  // 含结尾 \0
            cds.lpData = (PVOID)path.c_str();
            SendMessageW(hWnd, WM_COPYDATA, 0, (LPARAM)&cds);
            return;
        }
        Sleep(100);  // 首实例窗口尚未创建，等待
    }
    LOG_WARN("Main", "IPC: 未找到运行实例窗口，路径未传递");
}

// ─── 入口点 ───
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int cmdShow) {
    LOG_INFO("Main", "Ark Viewer 2 启动");
    // OLE 初始化：DoDragDrop（OLE 拖拽源）要求主线程通过 OleInitialize 初始化 OLE 子系统，
    // 仅 CoInitializeEx 不够。OleInitialize 内部调用 CoInitializeEx(STA)，
    // WicDecoder/ExifParser 后续的 CoInitializeEx(STA) 会幂等返回 S_FALSE，无冲突。
    HRESULT oleHr = OleInitialize(nullptr);
    if (FAILED(oleHr)) {
        LOG_WARN("Main", "OleInitialize 失败（拖拽功能可能不可用）");
    }

    // 公共控件初始化：设置弹窗用 Tab + ListView（checkbox 风格）
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    // libraw 使用 OpenMP，限制线程数避免卡死
    SetEnvironmentVariableW(L"OMP_NUM_THREADS", L"4");

    // 单实例检测：命名 Mutex，已存在说明已有进程运行
    CreateMutexW(nullptr, FALSE, SINGLE_INSTANCE_MUTEX);
    bool alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);

    std::wstring cmdPath = ParseCommandLinePath();

    if (alreadyRunning) {
        // 已有实例：将命令行路径转发给已有进程开新窗口，本进程退出
        if (!cmdPath.empty()) SendPathToRunningInstance(cmdPath);
        OleUninitialize();
        return 0;
    }

    // 首实例：注册解码器 + 创建主窗口 + 消息循环
    RegisterAllDecoders();
    Config::Instance().Load();  // 加载配置（失败用默认值，不阻塞启动）
    ActivityLog::Instance().Init(GetModuleHandleW(nullptr));
    ActivityLog::SetThreadName("UI");  // 主线程角色名（供性能遥测 JSONL thread_name 字段）
    WindowManager::Instance().CreateNewWindow(cmdPath, cmdShow);
    int ret = WindowManager::Instance().RunLoop();
    OleUninitialize();
    return ret;
}
