# ArkThumbProvider COM 直调功能测试
# 注册到 HKCU 用户层（无需管理员），测完清理
# 用 .NET File.ReadAllText 读源码 + Add-Type -TypeDefinition 绕开 -Path 对中文路径的限制
$dll = "e:\06-xiangmu\处理中2\Ark Novel方舟阅读\Ark Viewer方舟图片浏览器2\build\ArkThumbProvider.dll"
$clsid = "{C4B7E2A1-9F3D-4A6E-8B5C-1D7E9F3A2B48}"
$cs = "e:\06-xiangmu\处理中2\Ark Novel方舟阅读\Ark Viewer方舟图片浏览器2\test_thumb.cs"

# 用 .NET API 读源码（绕开 PowerShell cmdlet 对中文路径的限制）
$src = [System.IO.File]::ReadAllText($cs, [System.Text.Encoding]::UTF8)
Add-Type -TypeDefinition $src

$files = @(
    "C:\Users\aoebc\Desktop\新建文件夹 (5)\_AHY8031.ARW",
    "C:\Users\aoebc\Desktop\新建文件夹 (5)\_AHY8030.ARW",
    "C:\Users\aoebc\Desktop\新建文件夹 (5)\_AHY7018.psd"
    # PSB 文件 2.8GB 太大，跳过；HEIC/SVG/HDR 测试素材缺失，已用 PSD/ARW 验证关键路径
)
[ThumbTest.Tester]::Run($dll, $clsid, $files)
