; Ark Viewer 2 — Inno Setup 安装脚本
; 功能：简体中文界面 / 自定义安装路径 / 干净卸载（注销插件+删配置+删关联）
; 用法：ISCC.exe "packaging\ArkViewer2_setup.iss"

#define MyAppName "Ark Viewer 2"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Ark Viewer Contributors"
#define MyAppExeName "ArkViewer2.exe"
#define MyAppId "{7C9B5E31-4A8F-4C2D-9B1E-5F6A8D0C3B2E}"

[Setup]
; 基本信息
AppId={{#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\ArkViewer2
DefaultGroupName={#MyAppName}
; 用户可自定义安装位置（标准选择目录页）
DisableDirPage=no
; 缩略图插件写 HKCR（HKLM）需要管理员权限 → 安装包请求管理员
PrivilegesRequired=admin
; 简体中文
LanguageDetectionMethod=uilanguage
ShowLanguageDialog=no
; 安装包图标（用程序自身图标）
SetupIconFile=..\resources\app.ico
; 输出
OutputBaseFilename=ArkViewer2_Setup
OutputDir=..\release_pkg
Compression=lzma2
SolidCompression=yes
; 架构
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 安装后启动
CloseApplications=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: unchecked
Name: "registerthumb"; Description: "注册缩略图插件（资源管理器预览 RAW/PSD/HEIF 等）"; GroupDescription: "附加任务："; Flags: checkablealone

[Files]
; 主程序
Source: "..\build_release\ArkViewer2.exe"; DestDir: "{app}"; Flags: ignoreversion
; 解码 DLL（运行时必需）
Source: "..\build_release\turbojpeg.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build_release\libwebp.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build_release\libsharpyuv.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build_release\libraw.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build_release\heif.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build_release\libde265.dll"; DestDir: "{app}"; Flags: ignoreversion
; N 卡硬解（可选，无 N 卡自动回退）
Source: "..\build_release\nvjpeg64_12.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build_release\cudart64_12.dll"; DestDir: "{app}"; Flags: ignoreversion
; 缩略图插件
Source: "..\build_release\ArkThumbProvider.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; 缩略图插件 COM 注册（HKCU，无需管理员）——用 regsvr32 处理更可靠，见 [Run]/[UninstallRun]

[Run]
; 安装后注册缩略图插件（勾选任务时）
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\ArkThumbProvider.dll"""; StatusMsg: "正在注册缩略图插件..."; Flags: runhidden; Tasks: registerthumb
; 安装后启动
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; 卸载时注销缩略图插件（/u 反注册，干干净净）
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\ArkThumbProvider.dll"""; Flags: runhidden; RunOnceId: "unregthumb"

[UninstallDelete]
; 删除配置/日志目录（%LOCALAPPDATA%\ArkViewer2）
Type: filesandordirs; Name: "{localappdata}\ArkViewer2"
; 删除软件目录残余
Type: filesandordirs; Name: "{app}"

[Code]
// 卸载前确认弹窗（提示会删除配置）
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    if MsgBox('卸载将删除软件及全部配置/日志，确定继续？', mbConfirmation, MB_YESNO) = IDNO then
      Abort;
end;
