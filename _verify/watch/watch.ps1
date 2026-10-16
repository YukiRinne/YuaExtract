# 哈希名恢复的长时间监控：定期把"映射表规模 + 还原进度 + 游戏进程是否还在"记进 TSV。
# 产物：_verify\watch\hashwatch.tsv（制表符分隔，一行一次采样）
# 列：时间 / 目录映射行数 / 文件映射行数 / 恢复映射表行数 / 游戏进程数 / HashRestore.log 最后一条 SCAN

param(
    [int]$IntervalSeconds = 30,
    [int]$MaxMinutes = 180
)

$watchDir = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $watchDir)
$logDir = Join-Path $root 'Release\Log'
$gameDir = 'E:\GAME\GALGAME\LimeLight Lemonade Jam\ライムライト・レモネードジャム\ライムライト・レモネードジャム'
$hashDir = Join-Path $gameDir 'StringHashDumper_Output'
$hashRestoreLog = Join-Path $logDir 'HashRestore.log'
$out = Join-Path $watchDir 'hashwatch.tsv'
$gameExe = 'limelight_lj_crack'

# 这些文件被游戏进程以共享写方式打开着，必须用 FileShare.ReadWrite 才读得到。
function Get-SharedLineCount([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return -1 }
    try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $count = 0
        $buffer = New-Object byte[] 262144
        while (($read = $fs.Read($buffer, 0, $buffer.Length)) -gt 0) {
            for ($i = 0; $i -lt $read; $i++) { if ($buffer[$i] -eq 10) { $count++ } }
        }
        $fs.Close()
        return $count
    } catch { return -2 }
}

if (-not (Test-Path $out)) {
    "时间`t目录映射`t文件映射`t恢复映射表`t游戏进程`t最后一条SCAN" | Set-Content -Path $out -Encoding UTF8
}

$deadline = (Get-Date).AddMinutes($MaxMinutes)
while ((Get-Date) -lt $deadline) {
    $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    $dirLines = Get-SharedLineCount (Join-Path $hashDir 'DirectoryHash.log')
    $fileLines = Get-SharedLineCount (Join-Path $hashDir 'FileNameHash.log')
    $lstLines = Get-SharedLineCount (Join-Path $hashDir 'HashRestore_RecoveredNames.lst')
    $gameCount = @(Get-Process -Name $gameExe -ErrorAction SilentlyContinue).Count
    $scan = ''
    if (Test-Path -LiteralPath $hashRestoreLog) {
        $scan = (Get-Content -LiteralPath $hashRestoreLog -Tail 300 -Encoding UTF8 -ErrorAction SilentlyContinue |
                 Where-Object { $_ -match '\| SCAN \|' } | Select-Object -Last 1)
        if ($null -eq $scan) { $scan = '' }
    }
    "$stamp`t$dirLines`t$fileLines`t$lstLines`t$gameCount`t$scan" | Add-Content -Path $out -Encoding UTF8
    Start-Sleep -Seconds $IntervalSeconds
}
