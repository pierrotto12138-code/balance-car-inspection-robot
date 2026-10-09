# 通过 easyeda-bridge 在 EDA 进程内执行 JS
#
# 用法:
#   pwsh -File tools/eda_exec.ps1 -CodeFile tools/xxx.js
#   pwsh -File tools/eda_exec.ps1 -Code "return 1+1;"
#
# code 会被包成 async function(eda){ ... }，必须用 return 返回结果。
# 用 curl.exe 发请求，以便拿到完整状态码与响应体（PowerShell 的 Invoke-RestMethod 会吞掉错误体）。
param(
    [string]$Code,
    [string]$CodeFile,
    [string]$Bridge = "http://127.0.0.1:49620"
)

if ($CodeFile) { $Code = Get-Content -Raw -Encoding UTF8 $CodeFile }
if (-not $Code) { Write-Error "需要 -Code 或 -CodeFile"; exit 1 }

$body = @{ code = $Code } | ConvertTo-Json -Compress -Depth 5

$tmp = [System.IO.Path]::GetTempFileName()
[System.IO.File]::WriteAllText($tmp, $body, (New-Object System.Text.UTF8Encoding($false)))

try {
    $raw = & curl.exe -s -X POST "$Bridge/execute" `
        -H "Content-Type: application/json; charset=utf-8" `
        --data-binary "@$tmp" `
        -w "`n___HTTP_STATUS___%{http_code}" 2>&1
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}

$text = ($raw | Out-String)
$status = ''
if ($text -match '___HTTP_STATUS___(\d+)') {
    $status = $Matches[1]
    $text = $text -replace '___HTTP_STATUS___\d+', ''
}
$text = $text.Trim()

if ($status -ne '200') { Write-Output "HTTP $status" }

if ($text -match '^\s*\{') {
    try {
        $o = $text | ConvertFrom-Json
        if ($o.success) {
            if ($null -eq $o.result) { Write-Output "(null)" } else { Write-Output $o.result }
            exit 0
        } else {
            Write-Output "EDA ERROR: $($o.error)"
            exit 1
        }
    } catch { }
}
Write-Output $text
