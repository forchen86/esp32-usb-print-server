# 一键发布 GitHub Release（推送代码 + 建版本 + 传固件）
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\publish_release.ps1 -Token ghp_xxx [-Version v1.5.1] [-NotesFile 发布说明.md]
# Token 需要 repo 权限；用完建议立即到 GitHub 设置里撤销。
param(
  [Parameter(Mandatory=$true)][string]$Token,
  [string]$Version = "",
  [string]$NotesFile = ""
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
[System.Net.WebRequest]::DefaultWebProxy = $null   # 绕过失效的系统代理
$Repo = "forchen86/esp32-usb-print-server"
$H = @{ Authorization = "Bearer $Token"; "User-Agent" = "esp32-print-server"; Accept = "application/vnd.github+json" }

# 1. 推送代码（直连失败自动走本机代理端口探测）
$ok = $false
foreach ($mode in @("direct", "proxy7897", "proxy7890")) {
  try {
    if ($mode -eq "direct") { git -c credential.helper= -c http.https://github.com.proxy= push "https://forchen86:${Token}@github.com/$Repo.git" main:main 2>$null }
    else { $port = $mode -replace 'proxy',''; git -c credential.helper= -c "http.https://github.com.proxy=http://127.0.0.1:$port" push "https://forchen86:${Token}@github.com/$Repo.git" main:main 2>$null }
    if ($LASTEXITCODE -eq 0) { Write-Output "[OK] 代码已推送（$mode）"; $ok = $true; break }
  } catch {}
}
if (-not $ok) { Write-Error "代码推送失败：检查网络或 token"; exit 1 }

# 2. 版本号：默认取 src/config.h 里的 FW_VERSION
if (-not $Version) {
  $fw = (Select-String -Path src/config.h -Pattern 'FW_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
  $Version = "v$fw"
}
Write-Output "[i] 版本：$Version"

# 3. 创建 Release（已存在则复用）
$base = "https://api.github.com/repos/$Repo"
$notes = if ($NotesFile -and (Test-Path $NotesFile)) { Get-Content $NotesFile -Raw -Encoding UTF8 } else { "$Version 发布" }
try {
  $rel = Invoke-RestMethod -Uri "$base/releases/tags/$Version" -Headers $H -TimeoutSec 20
  Write-Output "[i] Release $Version 已存在，复用 id=$($rel.id)"
} catch {
  $body = @{ tag_name = $Version; target_commitish = "main"; name = "$Version"; body = $notes; draft = $false; prerelease = $false } | ConvertTo-Json
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($body)
  $rel = Invoke-RestMethod -Method Post -Uri "$base/releases" -Headers $H -Body $bytes -ContentType "application/json; charset=utf-8" -TimeoutSec 30
  Write-Output "[OK] Release 已创建：$($rel.html_url)"
}

# 4. 上传固件资产（release 与 debug 两套 merged 镜像 + 校验和）
$up = "https://uploads.github.com/repos/$Repo/releases/$($rel.id)/assets"
foreach ($f in @("release", "debug")) {
  $bin = "firmware/$f/merged.bin"
  $name = "esp32s3-usb-print-server-$Version-$f.bin"
  $a = Invoke-RestMethod -Method Post -Uri ($up + "?name=" + $name) -Headers $H -InFile $bin -ContentType "application/octet-stream" -TimeoutSec 180
  Write-Output ("[OK] 上传 " + $a.name + "  " + $a.size + " B")
}
$sum = Invoke-RestMethod -Method Post -Uri ($up + "?name=SHA256SUMS.txt") -Headers $H -InFile "firmware/release/SHA256SUMS.txt" -ContentType "text/plain" -TimeoutSec 60
Write-Output ("[OK] 上传 " + $sum.name)
Write-Output ("[完成] " + $rel.html_url)
