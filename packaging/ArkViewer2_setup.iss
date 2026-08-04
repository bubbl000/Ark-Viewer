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
Name: "registerassoc"; Description: "注册文件关联（双击 JPG/PNG/PSD/RAW 等图片用本软件打开）"; GroupDescription: "附加任务："; Flags: checkablealone

[Files]
; 主程序
Source: "..\build\ArkViewer2.exe"; DestDir: "{app}"; Flags: ignoreversion
; 解码 DLL（运行时必需）
Source: "..\build\turbojpeg.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\libwebp.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\libsharpyuv.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\libraw.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\heif.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\libde265.dll"; DestDir: "{app}"; Flags: ignoreversion
; N 卡硬解（可选，无 N 卡自动回退）
Source: "..\build\nvjpeg64_12.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\cudart64_12.dll"; DestDir: "{app}"; Flags: ignoreversion
; 缩略图插件
Source: "..\build\ArkThumbProvider.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; 缩略图插件 COM 注册（HKCU，无需管理员）——用 regsvr32 处理更可靠，见 [Run]/[UninstallRun]

[Run]
; 安装后注册缩略图插件（勾选任务时）
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\ArkThumbProvider.dll"""; StatusMsg: "正在注册缩略图插件..."; Flags: runhidden; Tasks: registerthumb
; 安装后注册文件关联（勾选任务时）——静默注册全部支持格式后退出
Filename: "{app}\{#MyAppExeName}"; Parameters: "--assoc"; StatusMsg: "正在注册文件关联..."; Flags: runhidden; Tasks: registerassoc
; 安装后启动
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; 卸载时注销缩略图插件（/u 反注册，干干净净）
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\ArkThumbProvider.dll"""; Flags: runhidden; RunOnceId: "unregthumb"
; 卸载时取消文件关联（静默反注册全部格式后退出）
Filename: "{app}\{#MyAppExeName}"; Parameters: "--unassoc"; Flags: runhidden; RunOnceId: "unregassoc"

[UninstallDelete]
; 删除配置/日志目录（%LOCALAPPDATA%\ArkViewer2）
Type: filesandordirs; Name: "{localappdata}\ArkViewer2"
; 注意：安装目录 {app} 的删除放在 [Code] 的 CurUninstallStepChanged(usPostUninstall) 里，
; 带"目录名必须是 ArkViewer2"安全护栏，防止用户把安装路径选成父目录时误删其他软件。
; 这里不再写 filesandordirs {app}（否则无条件递归删除会绕过护栏）。

[Code]
// 删除单个注册表值（忽略不存在）
procedure DeleteRegValueIfExists(RootKey: Integer; SubKey, ValueName: String);
begin
  if RegValueExists(RootKey, SubKey, ValueName) then
    RegDeleteValue(RootKey, SubKey, ValueName);
end;

// 从 FileExts\.ext\OpenWithList 中移除 ArkViewer2.exe 条目（含 MRUList 清理）
procedure CleanOpenWithList(Ext: String);
var
  BaseKey, MruList, NewMru, Letter: String;
  I: Integer;
begin
  BaseKey := 'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts' + Ext + '\OpenWithList';
  // 删除名为 a/b/c... 且值为 ArkViewer2.exe 的条目
  MruList := '';
  if RegQueryStringValue(HKCU, BaseKey, 'MRUList', MruList) then
  begin
    NewMru := '';
    for I := 1 to Length(MruList) do
    begin
      Letter := Copy(MruList, I, 1);
      if RegQueryStringValue(HKCU, BaseKey, Letter, Letter) then
      begin
        // Letter 现在被覆盖为值；若指向 ArkViewer2 则删除该条目
        if CompareText(Letter, 'ArkViewer2.exe') = 0 then
          DeleteRegValueIfExists(HKCU, BaseKey, Copy(MruList, I, 1))
        else
          NewMru := NewMru + Copy(MruList, I, 1);
      end
      else
        NewMru := NewMru + Copy(MruList, I, 1);
    end;
    if NewMru <> MruList then
      RegWriteStringValue(HKCU, BaseKey, 'MRUList', NewMru);
  end;
end;

// 卸载后清理：文件关联残留（ProgId + OpenWithList + UserChoiceLatest + OpenWithProgids）
procedure CleanupAssocResidue();
var
  Exts: array of String;
  I: Integer;
  Ext, FeKey: String;
begin
  SetArrayLength(Exts, 29);
  Exts[0] := '.arw'; Exts[1] := '.cr2'; Exts[2] := '.cr3'; Exts[3] := '.nef'; Exts[4] := '.dng';
  Exts[5] := '.raf'; Exts[6] := '.x3f'; Exts[7] := '.pef'; Exts[8] := '.rw2'; Exts[9] := '.orf';
  Exts[10] := '.psd'; Exts[11] := '.psb'; Exts[12] := '.heic'; Exts[13] := '.heif'; Exts[14] := '.hif';
  Exts[15] := '.svg'; Exts[16] := '.svgz'; Exts[17] := '.hdr'; Exts[18] := '.pic';
  Exts[19] := '.jpg'; Exts[20] := '.jpeg'; Exts[21] := '.png'; Exts[22] := '.webp'; Exts[23] := '.bmp';
  Exts[24] := '.gif'; Exts[25] := '.tif'; Exts[26] := '.tiff'; Exts[27] := '.ico'; Exts[28] := '.raw';

  // 1. 删除 ProgId 整树
  if RegKeyExists(HKCU, 'Software\Classes\ArkViewer2.Image') then
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\ArkViewer2.Image');

  for I := 0 to GetArrayLength(Exts) - 1 do
  begin
    Ext := Exts[I];
    FeKey := 'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts' + Ext;

    // 2. OpenWithList 移除 ArkViewer2.exe
    CleanOpenWithList(Ext);
    // 3. OpenWithProgids 移除 ArkViewer2.Image
    DeleteRegValueIfExists(HKCU, FeKey + '\OpenWithProgids', 'ArkViewer2.Image');
    // 4. UserChoiceLatest 若指向 ArkViewer2.Image 则删 ProgId 值
    DeleteRegValueIfExists(HKCU, FeKey + '\UserChoiceLatest\ProgId', 'ProgId');
    // 5. HKCR 侧 OpenWithProgids
    DeleteRegValueIfExists(HKCU, 'Software\Classes' + Ext + '\OpenWithProgids', 'ArkViewer2.Image');
  end;
end;

// 卸载钩子：确认弹窗 + 卸载完成后清理关联残留 + 安全删除安装目录
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  AppDir, DirName: String;
begin
  if CurUninstallStep = usUninstall then
    if MsgBox('卸载将删除软件及全部配置/日志，确定继续？', mbConfirmation, MB_YESNO) = IDNO then
      Abort;
  // 卸载真正完成后清残留（usPostUninstall：文件已删，注册表还在）
  if CurUninstallStep = usPostUninstall then
  begin
    CleanupAssocResidue();
    // 安全删除安装目录：仅当文件夹名是 ArkViewer2 才连根删（防止用户把安装路径
    // 直接选成某个已有父目录时误删父目录里的其他软件）
    AppDir := ExpandConstant('{app}');
    DirName := ExtractFileName(RemoveBackslash(AppDir));
    if Pos('ArkViewer2', DirName) > 0 then
      DelTree(AppDir, True, True, True)
    else
      MsgBox('安装目录不是 ArkViewer2 命名（' + DirName + '），为安全起见未删除整个文件夹，请手动清理。',
        mbInformation, MB_OK);
  end;
end;
