# x4_live_demo 运行脚本
# 作用: 把 CameraSDK/MediaSDK 的 bin 目录注入 PATH（运行时依赖 DLL 都在这两个目录），
#       然后启动 x4_live_demo.exe
# 用法: .\run.ps1 [--duration 30] [--res low|high] [--stitch template|dynamic] ...
#       推流验证: .\run.ps1 --rdk-stream   （PC 监听 9999, 等 RDK 连入, 见 rdk/yolo_pose_client.py）

$ErrorActionPreference = "Stop"
# 程序输出为 UTF-8，控制台切到 UTF-8 避免中文乱码
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$sdkRoot = Join-Path $root "..\赛事SDK包（Windows+Linux）\Windows_CameraSDK-2.1.1_MediaSDK-3.1.3\Windows_CameraSDK-2.1.1_MediaSDK-3.1.3"
$mediaBin  = Join-Path $sdkRoot "MediaSDK-3.1.3-20260128-win64_1769600100370\MediaSDK-3.1.3-20260128-win64\MediaSDK\bin"
$cameraBin = Join-Path $sdkRoot "CameraSDK-20250812_192505-2.1.1-win64_1754998240815\CameraSDK-20250812_192505-2.1.1-win64\bin"

foreach ($dir in @($mediaBin, $cameraBin)) {
    if (-not (Test-Path $dir)) {
        Write-Error "SDK bin 目录不存在: $dir"
        exit 1
    }
}

# MediaSDK/bin 在前（其中自带配套版本的 CameraSDK.dll）
$env:PATH = "$mediaBin;$cameraBin;$env:PATH"

# 优先找 Release，其次 Debug
$exe = Join-Path $root "build\Release\x4_live_demo.exe"
if (-not (Test-Path $exe)) {
    $exe = Join-Path $root "build\Debug\x4_live_demo.exe"
}
if (-not (Test-Path $exe)) {
    Write-Error "未找到 x4_live_demo.exe，请先构建: cmake -B build && cmake --build build --config Release"
    exit 1
}

Write-Host "运行: $exe $args"
& $exe @args
exit $LASTEXITCODE
