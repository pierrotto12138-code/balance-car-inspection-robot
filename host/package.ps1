# =============================================================================
#  上位机一键打包（角色 C 的 P5 交付物：可执行程序 + 依赖 + 校验）
#
#  产出：dist/上位机/  —— **便携目录**：整个目录拷到没装 Qt 的电脑上，双击 host.exe 就能跑
#
#  用法：
#    pwsh -File package.ps1                    # 用 build-release（推荐，交付用）
#    pwsh -File package.ps1 -BuildDir build    # 用 Debug 树（开发期快速验证打包流程）
#
#  五条讲究（每一条都对应打包这类事实际会踩的坑）：
#    ① **交付要用 Release 树**：本项目 Debug 的 host.exe 是 41.5 MB，Release 只有 3.2 MB
#       （Debug 还带 -O0，实时曲线/解帧都更慢）—— 用 Debug 交付既笨重又慢；
#    ② **用 windeployqt 而不是手工拷 DLL**：它按 exe 的真实依赖挑 Qt 模块，并带上
#       platforms/ styles/ 这些**插件**（少一个插件就是"双击没反应"）；
#    ③ **SQLite 驱动必须跟着走**（sqldrivers/qsqlite.dll）：少了它程序不崩、但记录功能
#       会**悄悄降级**成"只有 CSV" —— 所以本脚本把它列为**硬校验项**；
#    ④ **校验必须用真实平台插件**：windeployqt 只部署当前平台的 `platforms/qwindows.dll`，
#       **没有** `qoffscreen.dll` ⇒ 在便携目录里跑 `-platform offscreen` 必然失败
#       （本项目第一次打包就踩了：自检报 "Could not find the Qt platform plugin offscreen"）。
#       开发期用 offscreen 做无头自检，交付目录里则用**真实平台插件**验证 —— 那才是"双击"的路径；
#    ⑤ **瘦身要"删了再验"**：dxcompiler/dxil（D3D12 着色器编译器，25 MB）、QML/Network/Svg、
#       用不到的 SQL 驱动都是被 windeployqt 顺手带上的；脚本删完之后**必须再跑一次自检**，
#       确认真的不需要它们（删错一个就是"在我机器上好好的"）。
# =============================================================================
param(
    [string]$QtDir  = "D:/0_APP/QT/6.11.2/mingw_64",
    [string]$Mingw  = "D:/0_APP/QT/Tools/mingw1310_64",
    [string]$BuildDir = "build-release",
    [string]$OutDir = "dist/上位机",
    [int]$SelfTestSeconds = 3,
    [switch]$NoTrim
)

$ErrorActionPreference = 'Stop'
$host_root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $host_root
function Info($m) { Write-Host "  $m" }

Write-Host "=== 上位机打包（$BuildDir → $OutDir）==="
$exe = Join-Path $BuildDir 'host.exe'
if (-not (Test-Path $exe)) {
    throw "找不到 $exe —— 先执行：cmake -S . -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build $BuildDir"
}
if ($BuildDir -notmatch 'release') {
    Write-Host "  ⚠ 注意：$BuildDir 不是 Release 树，交付体积会明显偏大"
}

# ---- 1) 输出目录 ----
if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Path $OutDir | Out-Null
Copy-Item $exe (Join-Path $OutDir 'host.exe')
Info ("已复制 host.exe（{0:N0} KB）" -f ((Get-Item $exe).Length / 1KB))

# ---- 2) windeployqt ----
$env:PATH = "$Mingw/bin;$QtDir/bin;" + $env:PATH
$wq = Join-Path $QtDir 'bin/windeployqt.exe'
if (-not (Test-Path $wq)) { throw "找不到 windeployqt：$wq" }
# 注意：windeployqt 的参数写错时**只打印一行 "Unknown option" 然后什么都不部署**
# （本项目踩过：`--no-system-d3d-compiler:$false` 这种写法不合法 ⇒ 输出目录空空如也，
#  而脚本若不校验就会打出一个"看似成功"的空包）。所以下面立刻做硬校验。
& $wq --release --no-translations --no-opengl-sw (Join-Path $OutDir 'host.exe') | Out-Null
Info "windeployqt 完成"

if (-not (Test-Path (Join-Path $OutDir 'Qt6Core.dll'))) {
    throw "windeployqt 没有部署出任何 Qt DLL —— 检查它的参数是否合法（它遇错只打印一行就退出）"
}

# ---- 3) 瘦身（只删"确定用不到"的；删完还要验）----
if (-not $NoTrim) {
    $trim = @(
        # D3D12 着色器编译器：纯 QWidgets(QPainter) 程序不用 RHI，实测可删（25 MB）
        'dxcompiler.dll', 'dxil.dll',
        # D3D11 着色器编译器：同理（4 MB）—— 实测删掉后 GUI/自检/截图三条路径都正常
        'D3Dcompiler_47.dll',
        # 我们不用 QML/Quick、不用网络、不用 SVG 图标
        'Qt6Network.dll', 'Qt6Svg.dll', 'Qt6Qml.dll', 'Qt6Quick.dll',
        'tls', 'networkinformation', 'iconengines', 'generic',
        # 只连 SQLite，其余驱动是 windeployqt 顺手带的
        'sqldrivers/qsqlibase.dll', 'sqldrivers/qsqlodbc.dll',
        'sqldrivers/qsqlpsql.dll', 'sqldrivers/qsqlmimer.dll', 'sqldrivers/qsqloci.dll',
        # 截图存 PNG（PNG 编解码在 Qt6Gui 里），不需要 imageformats
        'imageformats'
    )
    $removed = 0
    foreach ($t in $trim) {
        $p = Join-Path $OutDir $t
        if (Test-Path $p) { Remove-Item -Recurse -Force $p; $removed++ }
    }
    Info "瘦身：移除 $removed 项（D3D12 编译器 / QML / 网络 / 多余 SQL 驱动 / 图片格式插件）"
}

# ---- 4) 硬校验 ----
$must = @(
    'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6SerialPort.dll', 'Qt6Sql.dll',
    'platforms/qwindows.dll',   # 少了它：双击没反应（"could not find the Qt platform plugin"）
    'sqldrivers/qsqlite.dll'    # 少了它：会话库静默不可用（记录只剩 CSV）
)
$missing = @($must | Where-Object { -not (Test-Path (Join-Path $OutDir $_)) })
if ($missing.Count -gt 0) { throw "打包缺少必需文件：$($missing -join ', ')" }
Info "依赖校验通过（$($must.Count) 项，含 qwindows 平台插件与 qsqlite 驱动）"

# ---- 5) 干净 PATH 下验证（模拟"没装 Qt 的电脑"）----
$cleanPath = ($env:PATH -split ';' | Where-Object { $_ -and ($_ -notmatch 'QT|mingw|Ninja') }) -join ';'
$env:PATH = $cleanPath
Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue  # ★ 用真实平台插件（=双击路径）
$target = Join-Path $OutDir 'host.exe'

Info "① 无头自检（真实平台插件，Qt/MinGW 已从 PATH 移除）…"
$log = Join-Path $OutDir '_selftest.txt'
$p = Start-Process -FilePath $target -ArgumentList @('--selftest', "$SelfTestSeconds") `
                   -NoNewWindow -Wait -PassThru -RedirectStandardOutput $log
if ($p.ExitCode -ne 0) {
    Write-Host (Get-Content $log -Tail 15 | Out-String)
    throw "❌ 便携目录自检失败（退出码 $($p.ExitCode)）—— 还缺依赖"
}
Info "  自检通过（退出码 0）"

Info "② 界面冒烟（真的把窗口起起来，4 s 后结束）…"
$gui = Start-Process -FilePath $target -ArgumentList @('--demo', '--rate', '8') -PassThru
Start-Sleep -Seconds 4
if ($gui.HasExited) {
    throw "❌ 界面启动后提前退出（退出码 $($gui.ExitCode)）—— 插件/依赖不全"
}
$gui.Kill(); $gui.WaitForExit()
Info "  界面前台运行 4 s 未异常退出"

Info "③ 截图链路（窗口渲染 → QWidget::grab → PNG 落盘）…"
$shot = Join-Path $OutDir '_shot.png'
$g2 = Start-Process -FilePath $target -PassThru `
      -ArgumentList @('--demo', '--rate', '8', '--shot', $shot, '--shot-delay', '2000', '--shot-tab', '2')
Start-Sleep -Seconds 6
if (-not $g2.HasExited) { $g2.Kill(); $g2.WaitForExit() }
if (-not (Test-Path $shot) -or (Get-Item $shot).Length -lt 20KB) {
    throw "❌ 截图失败（没生成或文件过小）—— 渲染路径有问题"
}
Info ("  截图正常（{0:N0} KB）" -f ((Get-Item $shot).Length / 1KB))
Remove-Item $shot -ErrorAction SilentlyContinue

# ---- 5b) 记录链路：CSV 有行 + SQLite 会话库真的建起来了 ----
#   ★ 这一步专门验 `sqldrivers/qsqlite.dll`：少了它程序**不报错**，只是会话库静默不可用 ——
#     正是那种"打包漏了插件、现场才发现"的问题。
Info "④ 记录链路（CSV 行数 + SQLite 会话库）…"
$rec = Join-Path $OutDir '_rec.csv'
$db  = Join-Path $OutDir '_rec.db'
$g3 = Start-Process -FilePath $target -PassThru `
      -ArgumentList @('--demo', '--rate', '8', '--record', $rec, '--db', $db)
Start-Sleep -Seconds 5
if (-not $g3.HasExited) { $g3.Kill(); $g3.WaitForExit() }
if (-not (Test-Path $rec)) { throw "❌ 没生成记录文件（录制功能不可用）" }
$rows = (Get-Content $rec | Measure-Object -Line).Lines - 1
if ($rows -lt 10) { throw "❌ 记录文件只有 $rows 行数据（应远大于 10）" }
if (-not (Test-Path $db) -or (Get-Item $db).Length -lt 8KB) {
    throw "❌ 会话库没建起来 —— qsqlite 驱动没随包部署（记录会静默降级成只有 CSV）"
}
Info ("  CSV {0} 行 · 会话库 {1:N0} KB" -f $rows, ((Get-Item $db).Length / 1KB))
Remove-Item $rec, $db -ErrorAction SilentlyContinue

# ---- 6) 汇总 ----
Remove-Item $log -ErrorAction SilentlyContinue
$files = Get-ChildItem -Recurse -File $OutDir
$sizeMb = [math]::Round(($files | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
Write-Host ""
Write-Host "输出目录：$OutDir"
Write-Host "文件数：$($files.Count) · 合计：$sizeMb MB"
Write-Host "✅ 打包成功：把 $OutDir 整个目录拷到没装 Qt 的电脑，双击 host.exe 即可运行"
Write-Host "   演示：host.exe --demo --rate 5（无硬件跑假源剧本）"
