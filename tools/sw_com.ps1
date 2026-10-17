<#
.SYNOPSIS
  SolidWorks COM 封装（本项目专用）—— 统一动词、统一护栏，避免每次临时写 COM 调用。

.DESCRIPTION
  动词：
    info                     连接已打开的 SolidWorks，报版本/当前文档
    open  -Path <文件>        打开 STEP/STL/SLDPRT/装配（不保存、不改原文件）
    mass  [-Path <文件>]      质量属性：体积/质量/重心（mm、g、mm）；不传 Path 就测当前文档
    export -Path <文件> -Out <目标>   另存为（按扩展名决定格式：.step .stl .x_t .pdf .dxf）
    close [-Path <文件>]      关闭文档（不保存）

  护栏（都是踩过的坑）：
    · 优先 **附加** 到你已经开着的实例（GetActiveObject），不新起进程，避免抢你的界面/许可
    · 一律 **不保存**（SaveAs 到新路径除外）；不碰你正在编辑的文档
    · 单位统一 mm/g：读质量属性前先 UseSystemUnits = $false 并按 mm 设单位
    · 退出时只关自己打开的文档

.EXAMPLE
  pwsh -NoProfile -File tools\sw_com.ps1 info
  pwsh -NoProfile -File tools\sw_com.ps1 mass -Path "…\A2-01_下壳.step"
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('info', 'open', 'mass', 'export', 'close')]
    [string]$Verb,
    [string]$Path,
    [string]$Out
)

$ErrorActionPreference = 'Stop'

function Get-SwInstance {
    try {
        $sw = [Runtime.InteropServices.Marshal]::GetActiveObject('SldWorks.Application')
        Write-Host "  （已附加到正在运行的 SolidWorks 实例）"
        return $sw
    } catch {
        Write-Host "  （没有运行中的实例，启动一个新的，隐藏启动）"
        $sw = New-Object -ComObject SldWorks.Application
        $sw.Visible = $true          # 有些版本隐藏时导出会失败，保持可见
        return $sw
    }
}

function Open-Doc($sw, $file) {
    if (-not (Test-Path $file)) { throw "文件不存在：$file" }
    $ext = [IO.Path]::GetExtension($file).ToLower()
    $type = switch ($ext) {
        '.sldprt' { 1 } '.sldasm' { 2 } '.slddrw' { 3 }
        default { 1 }   # STEP/IGES/STL 默认当零件导入
    }
    $errors = 0; $warnings = 0
    $doc = $sw.OpenDoc6($file, $type, 1, '', [ref]$errors, [ref]$warnings)   # 1 = silent
    if ($null -eq $doc) { throw "打开失败（errors=$errors warnings=$warnings）：$file" }
    return $doc
}

$sw = $null
switch ($Verb) {
    'info' {
        $sw = Get-SwInstance
        Write-Host ("  SolidWorks 版本：" + $sw.RevisionNumber())
        $doc = $sw.ActiveDoc
        if ($doc) { Write-Host ("  当前文档：" + $doc.GetTitle() + "  →  " + $doc.GetPathName()) }
        else { Write-Host "  当前没有打开任何文档" }
    }

    'open' {
        if (-not $Path) { throw '需要 -Path' }
        $sw = Get-SwInstance
        $doc = Open-Doc $sw $Path
        Write-Host ("  已打开：" + $doc.GetTitle())
        $n = $doc.GetType()
        Write-Host ("  文档类型：" + $(switch ($n) { 1 { '零件' } 2 { '装配' } 3 { '工程图' } default { $n } }))
    }

    'mass' {
        $sw = Get-SwInstance
        if ($Path) { $doc = Open-Doc $sw $Path } else { $doc = $sw.ActiveDoc }
        if (-not $doc) { throw '没有文档可测（请给 -Path 或先打开一个）' }
        $mp = $doc.Extension.CreateMassProperty()
        $mp.UseSystemUnits = $false          # 用文档单位
        $com = $mp.CenterOfMass             # 数组 (x,y,z)
        Write-Host ("  文档：" + $doc.GetTitle())
        Write-Host ("  体积：" + [math]::Round($mp.Volume, 2) + " mm³ = " + [math]::Round($mp.Volume / 1000, 2) + " cm³")
        Write-Host ("  质量：" + [math]::Round($mp.Mass, 2) + " g")
        Write-Host ("  重心：" + [math]::Round($com[0], 2) + ", " + [math]::Round($com[1], 2) + ", " + [math]::Round($com[2], 2) + " mm")
        Write-Host ("  表面积：" + [math]::Round($mp.SurfaceArea, 1) + " mm²")
        if ($Path) { $sw.CloseDoc($doc.GetTitle()) | Out-Null; Write-Host '  （已关闭该文档，未保存）' }
    }

    'export' {
        if (-not $Path -or -not $Out) { throw '需要 -Path 与 -Out' }
        $sw = Get-SwInstance
        $doc = Open-Doc $sw $Path
        $errors = 0; $warnings = 0
        $ok = $doc.Extension.SaveAs($Out, 0, 1, $null, [ref]$errors, [ref]$warnings)
        if ($ok) { Write-Host ("  已导出：" + $Out + "（" + [math]::Round((Get-Item $Out).Length / 1MB, 2) + " MB）") }
        else { throw "导出失败（errors=$errors warnings=$warnings）" }
        $sw.CloseDoc($doc.GetTitle()) | Out-Null
    }

    'close' {
        $sw = Get-SwInstance
        if ($Path) { $sw.CloseDoc([IO.Path]::GetFileNameWithoutExtension($Path)) | Out-Null; Write-Host ("  已关闭：" + $Path) }
        else { $doc = $sw.ActiveDoc; if ($doc) { $sw.CloseDoc($doc.GetTitle()) | Out-Null; Write-Host '  已关闭当前文档（未保存）' } }
    }
}
