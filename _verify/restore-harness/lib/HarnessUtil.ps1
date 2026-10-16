# YuaExtract 验证脚手架：无依赖的通用工具（UTF-8 写文件、PE 位数、SHA256、
# 目录快照、进程启动、vcvars 环境导入、断言结果收集）。
#
# 只允许在 _verify\restore-harness 下写东西：本文件里的删除操作都过
# Remove-TreeUnder，会先确认目标在脚手架根目录之下。

Set-StrictMode -Version Latest

# ---------------- 文本 / 文件 ----------------

function Write-Utf8File
{
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Text,
        [switch]$Bom,
        [switch]$NoNewline
    )

    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }

    $encoding = New-Object System.Text.UTF8Encoding($Bom.IsPresent)
    $payload = if ($NoNewline) { $Text } else { $Text + "`r`n" }
    [System.IO.File]::WriteAllText($Path, $payload, $encoding)
}

function Read-TextShared
{
    param([Parameter(Mandatory)][string]$Path, [int]$MaxBytes = 2000000)

    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
    try
    {
        $length = [int][Math]::Min($stream.Length, $MaxBytes)
        $buffer = New-Object byte[] $length
        $read = $stream.Read($buffer, 0, $length)
        return [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
    }
    finally { $stream.Dispose() }
}

function Write-JsonFile
{
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)]$Object)
    Write-Utf8File -Path $Path -Text ($Object | ConvertTo-Json -Depth 12)
}

function Read-JsonFile
{
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    return (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json)
}

# 路径分量安全化：**镜像产品** `ExtensionSet::MakeSafePathComponent`（ExtensionSet.cpp）。
#
# 为什么要镜像它（E-14 路径判据，2026-10-16）：详情面板的「库内目录」给的是**实际落库目录**
#（sanitize 之后的形态），而断言的期望值原来拿 manifest 里的**原始名**去 Join-Path ——
# 夹具用普通名字时 raw == sanitize 后，判据碰巧成立；一旦有人拿 `A:B` 这种会被改写的名字做夹具，
# needle 就指向一个**不存在的路径**：要么假红，要么两边都错成一样而**假绿**。
# 规则（与产品逐条一致）：去掉 0x20 以下的控制字符与 `< > : " / \ | ? *`；再去掉首尾的空格与点。
function ConvertTo-SafePathComponent
{
    param([AllowEmptyString()][string]$Name = '')

    if (-not $Name) { return '' }
    $bad = [char[]]@('<', '>', ':', '"', '/', '\', '|', '?', '*')
    $kept = New-Object System.Text.StringBuilder
    foreach ($ch in $Name.ToCharArray())
    {
        if ([int]$ch -lt 0x20) { continue }
        if ($bad -contains $ch) { continue }
        [void]$kept.Append($ch)
    }
    return $kept.ToString().Trim([char[]]@(' ', '.'))
}

# 安全取属性：结果 JSON 可能来自"提前失败"的分支，字段不一定齐；
# StrictMode 下直接点不存在的属性会抛异常，而那种异常恰恰会盖掉真正的失败原因。
function Get-ObjectValue
{
    param($Object, [Parameter(Mandatory)][string]$Name, $Default = $null)

    if ($null -eq $Object) { return $Default }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    if ($null -eq $property.Value) { return $Default }
    return $property.Value
}

# ---------------- 路径安全 ----------------

# 删除一个目录，但必须先证明它真的在脚手架根目录之下。
function Remove-TreeUnder
{
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$MustBeUnder)

    $full = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetFullPath($MustBeUnder).TrimEnd('\') + '\'
    if (-not $full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase))
    {
        throw "拒绝删除：$full 不在 $root 之下"
    }
    if ($full.TrimEnd('\') -eq $root.TrimEnd('\')) { throw "拒绝删除脚手架根目录本身：$full" }

    if (Test-Path -LiteralPath $full)
    {
        Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
    }
    return (-not (Test-Path -LiteralPath $full))
}

# ---------------- 文件指纹 / PE 位数 ----------------

function Get-Sha256Hex
{
    param([Parameter(Mandatory)][string]$Path)
    $hash = Get-FileHash -LiteralPath $Path -Algorithm SHA256
    return $hash.Hash.ToUpperInvariant()
}

function Get-TextSha256Hex
{
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try
    {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        $digest = $sha.ComputeHash($bytes)
        return ([System.BitConverter]::ToString($digest) -replace '-', '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Get-PeMachine
{
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
    try
    {
        if ($stream.Length -lt 0x40) { return $null }
        $reader = New-Object System.IO.BinaryReader($stream)
        $stream.Position = 0x3C
        $lfanew = $reader.ReadInt32()
        if ($lfanew -le 0 -or ($lfanew + 6) -gt $stream.Length) { return $null }
        $stream.Position = $lfanew
        if ($reader.ReadUInt32() -ne 0x00004550) { return $null }
        return [int]$reader.ReadUInt16()
    }
    finally { $stream.Dispose() }
}

function Format-PeMachine
{
    param($Machine)
    switch ($Machine)
    {
        0x014C { return 'x86 (PE32)' }
        0x8664 { return 'x64 (PE32+)' }
        0xAA64 { return 'ARM64' }
        $null { return '<读不到 / 文件不存在>' }
        default { return ('未知 0x{0:X4}' -f $Machine) }
    }
}

# ---------------- 确定性测试数据 ----------------

function New-DeterministicBytes
{
    param([Parameter(Mandatory)][int]$Seed, [Parameter(Mandatory)][int]$Length)

    $bytes = New-Object byte[] $Length
    if ($Length -gt 0)
    {
        $random = New-Object System.Random($Seed)
        $random.NextBytes($bytes)
    }
    # 逗号很重要：不加的话 0 字节数组会被 PowerShell 拆成"没有输出"，调用方拿到 $null。
    return ,$bytes
}

# ---------------- 目录快照 ----------------

function Get-DirectorySnapshot
{
    param([Parameter(Mandatory)][string]$Path, [switch]$WithHash)

    $entries = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Path)) { return $entries }

    $root = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $files = Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue
    foreach ($file in $files)
    {
        $relative = $file.FullName.Substring($root.Length).TrimStart('\')
        $entry = [ordered]@{
            size  = [long]$file.Length
            mtime = $file.LastWriteTimeUtc.ToString('o')
        }
        if ($WithHash) { $entry['sha256'] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToUpperInvariant() }
        $entries[$relative] = $entry
    }
    return $entries
}

# 快照从 JSON 读回来是 PSCustomObject，内存里是 OrderedDictionary —— 统一成普通哈希表，
# 后面的按键取值才不会因为类型不同而炸（StrictMode 下 PSCustomObject 没有 .Keys）。
function ConvertTo-SnapshotTable
{
    param($Snapshot)

    $table = @{}
    if ($null -eq $Snapshot) { return $table }
    if ($Snapshot -is [System.Collections.IDictionary])
    {
        foreach ($key in $Snapshot.Keys) { $table[[string]$key] = $Snapshot[$key] }
        return $table
    }
    foreach ($property in $Snapshot.PSObject.Properties) { $table[$property.Name] = $property.Value }
    return $table
}

function Compare-DirectorySnapshot
{
    param([Parameter(Mandatory)]$Before, [Parameter(Mandatory)]$After)

    $beforeTable = ConvertTo-SnapshotTable $Before
    $afterTable = ConvertTo-SnapshotTable $After

    $added = @()
    $removed = @()
    $changed = @()

    foreach ($key in $afterTable.Keys)
    {
        if (-not $beforeTable.ContainsKey($key)) { $added += $key; continue }
        if ($beforeTable[$key].size -ne $afterTable[$key].size -or $beforeTable[$key].mtime -ne $afterTable[$key].mtime) { $changed += $key }
    }
    foreach ($key in $beforeTable.Keys)
    {
        if (-not $afterTable.ContainsKey($key)) { $removed += $key }
    }

    return [pscustomobject]@{
        Added   = @($added | Sort-Object)
        Removed = @($removed | Sort-Object)
        Changed = @($changed | Sort-Object)
        Before  = $beforeTable.Count
        After   = $afterTable.Count
        Same    = ($added.Count -eq 0 -and $removed.Count -eq 0 -and $changed.Count -eq 0)
    }
}

# ---------------- 进程 ----------------

function Start-HarnessProcess
{
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = '',
        [hashtable]$Environment = @{},
        [string[]]$RemoveEnvironment = @()
    )

    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $FilePath
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    # 必须重定向子进程的 stdout/stderr：否则它会继承本脚本的 stdout 句柄，
    # 本脚本退出后（子进程还活着）调用方会一直等不到管道 EOF —— 在后台任务里
    # 表现成"命令永远不返回"。这里重定向之后用异步读取把管道排空。
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    if ($WorkingDirectory) { $info.WorkingDirectory = $WorkingDirectory }
    foreach ($argument in $Arguments) { [void]$info.ArgumentList.Add($argument) }
    foreach ($name in $Environment.Keys) { $info.Environment[$name] = [string]$Environment[$name] }
    foreach ($name in $RemoveEnvironment) { [void]$info.Environment.Remove($name) }

    $process = [System.Diagnostics.Process]::Start($info)
    # 把两个管道排空。**不能**用 OutputDataReceived 事件里的 PowerShell 脚本块：
    # 那个回调在 .NET 线程池线程上跑，没有 runspace，会抛
    # "此线程中没有可用于运行脚本的运行空间" 并把整个 pwsh 进程带崩（实测 0xC0000409）。
    # 用 ReadToEndAsync 纯 .NET 异步读，任务对象留着（别让它被回收），收尾时再看。
    try
    {
        if (-not (Get-Variable -Name HarnessOutputTasks -Scope Script -ErrorAction SilentlyContinue))
        {
            $script:HarnessOutputTasks = @{}
        }
        $script:HarnessOutputTasks[[int]$process.Id] = @{
            Out = $process.StandardOutput.ReadToEndAsync()
            Err = $process.StandardError.ReadToEndAsync()
        }
    }
    catch { }
    return $process
}

# 取子进程的 stdout/stderr（失败返回空串）。GUI 程序一般什么都不写，拿不到也无所谓。
function Get-HarnessProcessOutput
{
    param([Parameter(Mandatory)]$Process)

    $result = [ordered]@{ Out = ''; Err = '' }
    try
    {
        if ((Get-Variable -Name HarnessOutputTasks -Scope Script -ErrorAction SilentlyContinue) -and
            $script:HarnessOutputTasks.ContainsKey([int]$Process.Id))
        {
            $tasks = $script:HarnessOutputTasks[[int]$Process.Id]
            if ($tasks.Out.Wait(1500)) { $result.Out = [string]$tasks.Out.Result }
            if ($tasks.Err.Wait(1500)) { $result.Err = [string]$tasks.Err.Result }
        }
    }
    catch { }
    return $result
}

function Stop-HarnessProcess
{
    param([Parameter(Mandatory)]$Process, [int]$GraceMs = 1500)

    try
    {
        if ($Process.HasExited) { return $true }
        $Process.Kill()
        return $Process.WaitForExit($GraceMs)
    }
    catch { return $false }
}

# ---------------- 构建环境（vcvars 导入） ----------------

function Resolve-VcVarsPath
{
    $candidates = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
    )
    foreach ($candidate in $candidates)
    {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

# 把 vcvars32.bat 导出的环境搬进当前 pwsh 会话。
#
# 为什么要曲线救国：本仓库路径里带 '&'，在 cmd 里拼路径会被当成命令分隔符。
# 所以这里只让 cmd 跑一个**放在无空格/无 & 目录下**的临时批处理，
# 之后所有 cl.exe 调用都由 pwsh 直接发起（不经过 cmd 解析）。
function Import-VsBuildEnvironment
{
    param([Parameter(Mandatory)][string]$VcVarsPath)

    $tempRoot = $env:TEMP
    if (-not $tempRoot -or $tempRoot -match '[ &]') { $tempRoot = 'C:\Windows\Temp' }
    if (-not (Test-Path -LiteralPath $tempRoot)) { throw "找不到可用的临时目录：$tempRoot" }

    $cmdPath = Join-Path $tempRoot ('yua-harness-vcvars-{0}.cmd' -f $PID)
    $content = "@echo off`r`ncall `"$VcVarsPath`" >nul 2>&1`r`nif errorlevel 1 exit /b 9`r`nset`r`n"
    [System.IO.File]::WriteAllText($cmdPath, $content, [System.Text.Encoding]::ASCII)

    try
    {
        $lines = & cmd.exe /c $cmdPath
        if ($LASTEXITCODE -ne 0) { throw "vcvars 导入失败（退出码 $LASTEXITCODE）：$VcVarsPath" }
    }
    finally
    {
        Remove-Item -LiteralPath $cmdPath -Force -ErrorAction SilentlyContinue
    }

    $imported = 0
    foreach ($line in $lines)
    {
        if ($line -match '^([A-Za-z_][A-Za-z0-9_()]*)=(.*)$')
        {
            try
            {
                Set-Item -Path ('env:' + $matches[1]) -Value $matches[2] -ErrorAction Stop
                $imported++
            }
            catch { }
        }
    }
    if ($imported -lt 5) { throw "vcvars 环境变量没导进来（只识别到 $imported 条）" }
    return $imported
}

function Read-TextAutoShared
{
    param([Parameter(Mandatory)][string]$Path, [int]$MaxBytes = 400000)

    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
    try
    {
        $length = [int][Math]::Min($stream.Length, $MaxBytes)
        $buffer = New-Object byte[] $length
        $read = $stream.Read($buffer, 0, $length)
        if ($read -le 0) { return '' }

        # 报告既有 UTF-8 BOM 的，也可能有 UTF-16 的；这里按 BOM 猜一次，别读成乱码。
        if ($read -ge 2 -and $buffer[0] -eq 0xFF -and $buffer[1] -eq 0xFE)
        {
            return [System.Text.Encoding]::Unicode.GetString($buffer, 2, $read - 2)
        }
        if ($read -ge 2 -and $buffer[0] -eq 0xFE -and $buffer[1] -eq 0xFF)
        {
            return [System.Text.Encoding]::BigEndianUnicode.GetString($buffer, 2, $read - 2)
        }
        if ($read -ge 3 -and $buffer[0] -eq 0xEF -and $buffer[1] -eq 0xBB -and $buffer[2] -eq 0xBF)
        {
            return [System.Text.Encoding]::UTF8.GetString($buffer, 3, $read - 3)
        }
        return [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
    }
    finally { $stream.Dispose() }
}

# 从还原报告里把"标签 -> 数字"抠出来。
# 宽容处理各种写法：'已还原   6'、'已丢弃：5'、'缺目录 Hash = 3（…）'。
function Get-ReportNumberMap
{
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)

    $map = [ordered]@{}
    $pattern = '^\s*(?<label>[^\d]{1,60}?)\s*[:=：]?\s*(?<num>\d[\d,]*)\s*(?<tail>.*)$'
    foreach ($line in ($Text -split "`r?`n"))
    {
        $match = [regex]::Match($line, $pattern)
        if (-not $match.Success) { continue }
        $label = $match.Groups['label'].Value.Trim()
        if (-not $label) { continue }
        # 只排除"路径/时间"这类表头行。注意别用 '目录' 这种大范围词：
        # "缺目录 Hash" 里也含"目录"，一刀切会把真正的计数字段漏掉（踩过）。
        if ($label -match '时间|方式|本次运行|生成于') { continue }
        if ($label -match '目录$' -or $label -match '路径$') { continue }   # 解包目录 / 映射目录 / 输出目录 / 纯Hash目录
        if ($map.Contains($label)) { continue }
        $map[$label] = [int]($match.Groups['num'].Value -replace ',', '')
    }
    return $map
}

# 按关键词在报告里找一个计数。返回 $null 表示报告里没有这个口径。
function Find-ReportNumber
{
    param([Parameter(Mandatory)]$Map, [Parameter(Mandatory)][string[]]$Keywords, [string[]]$ExcludeKeywords = @())

    foreach ($label in $Map.Keys)
    {
        $hit = $false
        foreach ($keyword in $Keywords) { if ($label.Contains($keyword)) { $hit = $true; break } }
        if (-not $hit) { continue }
        $blocked = $false
        foreach ($keyword in $ExcludeKeywords) { if ($label.Contains($keyword)) { $blocked = $true; break } }
        if ($blocked) { continue }
        return [pscustomobject]@{ Label = $label; Value = $Map[$label] }
    }
    return $null
}

# ---------------- 被测程序快照 ----------------

# 把 Release\ 整个复制到脚手架自己的 Run\App\ 下再跑。
#
# 两个原因：
#   1) 正在跑的程序会锁住 exe，如果直接跑 Release\YuaExtract.exe，
#      队友那边同时按的编译会在链接阶段失败 —— 验证不能干扰实现；
#   2) 每次跑的都是同一份快照，中途被重新编译不会让一次验证前后半程用两个版本。
function Sync-ReleaseSnapshot
{
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$RunRoot,
        [switch]$Force
    )

    $sourceDir = Join-Path $RepoRoot 'Release'
    $targetDir = Join-Path $RunRoot 'App'
    $sourceExe = Join-Path $sourceDir 'YuaExtract.exe'
    $sourceDll = Join-Path $sourceDir 'CxdecExtractordll\CxdecStringDumper.dll'

    if (-not (Test-Path -LiteralPath $sourceExe)) { throw "找不到 $sourceExe；请先编译解决方案（Release|x86）" }

    $copied = $false
    $needCopy = $Force -or -not (Test-Path -LiteralPath (Join-Path $targetDir 'YuaExtract.exe'))
    if (-not $needCopy)
    {
        $snapshotTime = (Get-Item -LiteralPath (Join-Path $targetDir 'YuaExtract.exe')).LastWriteTimeUtc
        $newestSource = (Get-Item -LiteralPath $sourceExe).LastWriteTimeUtc
        if (Test-Path -LiteralPath $sourceDll)
        {
            $dllTime = (Get-Item -LiteralPath $sourceDll).LastWriteTimeUtc
            if ($dllTime -gt $newestSource) { $newestSource = $dllTime }
        }
        if ($newestSource -gt $snapshotTime) { $needCopy = $true }
    }

    if ($needCopy)
    {
        # 先复制到 App.staging 再原子替换：正在编译时源文件可能被占用，
        # 直接往 Run\App 里复制会留下"半份快照"（exe 在、DLL 不在），
        # 之后所有用例都会莫名其妙地报"找不到 DLL"。
        $stagingDir = Join-Path $RunRoot 'App.staging'
        if (Test-Path -LiteralPath $stagingDir) { [void](Remove-TreeUnder -Path $stagingDir -MustBeUnder $RunRoot) }
        New-Item -ItemType Directory -Path $stagingDir -Force | Out-Null
        try
        {
            Copy-Item -LiteralPath $sourceExe -Destination $stagingDir -Force
            Copy-Item -LiteralPath (Join-Path $sourceDir 'CxdecExtractordll') -Destination $stagingDir -Recurse -Force

            if (-not (Test-Path -LiteralPath (Join-Path $stagingDir 'YuaExtract.exe')))
            {
                throw '快照里没有 YuaExtract.exe'
            }
            if (-not (Test-Path -LiteralPath (Join-Path $stagingDir 'CxdecExtractordll\CxdecStringDumper.dll')))
            {
                throw '快照里没有 CxdecStringDumper.dll（链接可能正在进行）'
            }

            if (Test-Path -LiteralPath $targetDir) { [void](Remove-TreeUnder -Path $targetDir -MustBeUnder $RunRoot) }
            Move-Item -LiteralPath $stagingDir -Destination $targetDir
            $copied = $true
        }
        catch
        {
            [void](Remove-TreeUnder -Path $stagingDir -MustBeUnder $RunRoot -ErrorAction SilentlyContinue)
            # 旧快照留着还能用；把原因抛出去让调用方写进日志/结果，别静默吞掉。
            throw ("复制 Release 快照失败：{0}（正在编译时会出现；沿用上一份快照）" -f $_.Exception.Message)
        }
    }

    $exe = Join-Path $targetDir 'YuaExtract.exe'
    $dll = Join-Path $targetDir 'CxdecExtractordll\CxdecStringDumper.dll'
    return [pscustomobject]@{
        Exe           = $exe
        Dll           = $dll
        SnapshotDir   = $targetDir
        Copied        = $copied
        ExeSha256     = (Get-Sha256Hex -Path $exe)
        ExeTime       = (Get-Item -LiteralPath $exe).LastWriteTime
        DllSha256     = $(if (Test-Path -LiteralPath $dll) { (Get-Sha256Hex -Path $dll) } else { '' })
        DllTime       = $(if (Test-Path -LiteralPath $dll) { (Get-Item -LiteralPath $dll).LastWriteTime } else { $null })
        ExeMachine    = Format-PeMachine (Get-PeMachine -Path $exe)
        DllMachine    = $(if (Test-Path -LiteralPath $dll) { Format-PeMachine (Get-PeMachine -Path $dll) } else { '<缺>' })
    }
}

# ---------------- 截图转 PNG ----------------

# GDI 抓下来的是 BMP（手写头，不依赖 System.Drawing）。转 PNG 交给自带的 Python + Pillow，
# 转不了就保留 BMP，不因为是"锦上添花"的功能把验证弄失败。
function Convert-BmpToPng
{
    param(
        [Parameter(Mandatory)][string]$BmpPath,
        [Parameter(Mandatory)][string]$PngPath,
        [string]$PythonPath = 'C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
    )

    if (-not (Test-Path -LiteralPath $BmpPath)) { return $false }

    $python = $null
    if (Test-Path -LiteralPath $PythonPath) { $python = $PythonPath }
    else
    {
        $found = Get-Command python -ErrorAction SilentlyContinue
        if ($found) { $python = $found.Source }
    }
    if (-not $python) { return $false }

    $code = 'import sys;from PIL import Image;Image.open(sys.argv[1]).save(sys.argv[2])'
    try
    {
        & $python @('-c', $code, $BmpPath, $PngPath) 2>$null | Out-Null
    }
    catch { return $false }
    return (Test-Path -LiteralPath $PngPath)
}

# ---------------- 大规模用例：TSV、目录摘要、快速清单 ----------------
#
# 19 个文件的小用例随便怎么写都行；6 万个文件时 ConvertTo-Json / 逐个文件走 cmdlet
# 会慢到没法用（一次几十分钟）。所以大用例：
#   - 清单与输出列表落 TSV（一行一条，读写都是纯字符串）
#   - 目录快照改存"摘要"（文件数 + 排序后行的 sha256），前后比摘要
#   - 遍历与哈希走 C# 批量接口（Harness.Batch，见 HarnessNative.ps1）

function Write-TsvFile
{
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines
    )

    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($Path, $Lines, $encoding)
}

function Read-TsvFile
{
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $lines = [System.IO.File]::ReadAllLines($Path, [System.Text.Encoding]::UTF8)
    if ($lines.Count -eq 0) { return @() }
    $rows = New-Object System.Collections.ArrayList
    for ($index = 1; $index -lt $lines.Count; $index++)   # 第 0 行是表头
    {
        if ([string]::IsNullOrWhiteSpace($lines[$index])) { continue }
        [void]$rows.Add(($lines[$index] -split "`t"))
    }
    # 返回值约定：普通数组。调用方一律写 @(Read-TsvFile ...) 取值。
    # （曾经写成 return ,$rows.ToArray()：调用方再用 @() 包一层时会被套成
    #   "1 个元素的数组、元素是数组"，Where-Object 就只看到一条，很难查。）
    return $rows.ToArray()
}

# 目录摘要：文件数 + "相对路径|大小|mtime" 排序后的 sha256。
# 任何文件被改名/增删/改时间都会让摘要变化，够支撑"源目录未被改动"的判定，
# 又不必把 6 万条明细塞进结果 JSON。
# 树摘要（可排除若干目录名）：E-15/E-16/E-18/E-19 判「本地一个字节都不动」用它。
#
# **N-S1（2026-10-16 独立验证发现）**：原来只枚举 `-File`，于是对「新建/删除**空目录**」完全盲 ——
# 产品在失败路径上留下 `ExtensionLibrary\CON\`、`AB\T\` 这种空目录时，同一句提示还说「本地库未被改动」。
# 也就是说：所有靠这个摘要判「本地没动」的断言，在「空目录」这一类改动上是**假绿**。
# 现在同时枚举**目录**（含空目录，相对路径带 `D:` 前缀）并折进 Digest。
#   · Digest    = 文件行 + 目录行（**默认口径**，现在能看见空目录）
#   · FileDigest= 只有文件行（旧口径，留作对照/需要「只比文件」时用）
#   · Count = 文件数；DirCount = 目录数（含空目录）
# `-FilesOnly` 可以显式退回旧口径（谁要「只比文件」，就必须自己说出来，不许默认偷偷弱化）。
#
# **为什么 -Path 必须允许空串**（S-2）：「扩展集库还不存在」是首次安装的正常状态，那时
# Get-LibraryInfo 返回的 Root 就是空串；旧签名会先抛 ParameterBindingValidationException ——
# 那不是「断言红了」，是**驱动在写任何结果文件之前就崩了**。
function Get-TreeDigestExcluding
{
    param([AllowEmptyString()][string]$Path = '', [string[]]$ExcludeNames = @(), [switch]$FilesOnly)

    if (-not $Path -or -not (Test-Path -LiteralPath $Path))
    {
        return [pscustomobject]@{ Count = 0; DirCount = 0; Digest = ''; FileDigest = ''; Exists = $false }
    }
    $root = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $isExcluded = {
        param([string]$Relative)
        foreach ($segment in ($Relative -split '\\')) { if ($ExcludeNames -contains $segment) { return $true } }
        return $false
    }
    $fileLines = New-Object System.Collections.ArrayList
    foreach ($file in @(Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue))
    {
        $relative = $file.FullName.Substring($root.Length).TrimStart('\')
        if (& $isExcluded $relative) { continue }
        [void]$fileLines.Add(('{0}|{1}|{2}' -f $relative, $file.Length, $file.LastWriteTimeUtc.ToString('o')))
    }
    $dirLines = New-Object System.Collections.ArrayList
    foreach ($dir in @(Get-ChildItem -LiteralPath $root -Recurse -Directory -Force -ErrorAction SilentlyContinue))
    {
        $relative = $dir.FullName.Substring($root.Length).TrimStart('\')
        if (& $isExcluded $relative) { continue }
        # 目录只记相对路径：**空目录与新建的空目录都因此可见**（时间戳不入摘要，避免「只碰了 mtime」的噪声）
        [void]$dirLines.Add(('D:{0}' -f $relative))
    }
    $sortedFiles = @($fileLines | Sort-Object)
    $sortedDirs = @($dirLines | Sort-Object)
    $allLines = if ($FilesOnly) { $sortedFiles } else { @($sortedFiles) + @($sortedDirs) }
    return [pscustomobject]@{
        Count      = $sortedFiles.Count
        DirCount   = $sortedDirs.Count
        Digest     = (Get-TextSha256Hex -Text ($allLines -join "`n"))
        FileDigest = (Get-TextSha256Hex -Text ($sortedFiles -join "`n"))
        Exists     = $true
    }
}

function Get-DirectoryDigest
{
    param(
        [Parameter(Mandatory)][string]$Path,
        [switch]$WithHash,
        # 内容哈希是"加分项"：6 万文件时逐个开文件读一遍会被杀软拖成好几分钟
        # （实测 10 万次 open ≈ 5 分钟），而 D-1 的验收口径是"文件数与时间戳未变"，
        # 用 路径|大小|mtime 的摘要就够。超阈值时自动不做内容哈希。
        [int]$MaxHashFiles = 5000
    )

    if (-not (Test-Path -LiteralPath $Path)) { return [pscustomobject]@{ Count = 0; Digest = ''; HashDigest = '' } }

    # `@(...)` 不能省：目录里**恰好一个文件**时，[Harness.Batch]::Rows 的返回会被 PowerShell
    # 解包成单个字符串，紧接着的 `$rows.Count` 在 StrictMode 下直接抛
    # "在此对象上找不到属性 Count"（RawGame/OtherGame 的 Extractor_Output 就只有一个文件，踩过）。
    $rows = @(if ('Harness.Batch' -as [type]) { [Harness.Batch]::Rows($Path) } else { @() })
    $digest = Get-TextSha256Hex -Text ($rows -join "`n")

    $hashDigest = ''
    if ($WithHash -and $rows.Count -le $MaxHashFiles -and $rows.Count -gt 0 -and ('Harness.Batch' -as [type]))
    {
        $full = @($rows | ForEach-Object { Join-Path $Path (($_ -split '\|')[0]) })
        $hashes = [Harness.Batch]::Sha256Many([string[]]$full)
        $hashDigest = Get-TextSha256Hex -Text ((($rows) -join "`n") + '#' + (($hashes | Sort-Object) -join ','))
    }
    return [pscustomobject]@{ Count = $rows.Count; Digest = $digest; HashDigest = $hashDigest }
}

function Get-FastFileCount
{
    param([Parameter(Mandatory)][string]$Path)

    if ('Harness.Batch' -as [type]) { return [Harness.Batch]::Count($Path) }
    if (-not (Test-Path -LiteralPath $Path)) { return 0 }
    return @(Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue).Count
}

# 目录的**内容**摘要：文件数 + 排序后 `相对路径|大小|sha256` 的 sha256。
#
# 与 Get-DirectoryDigest 的区别：后者含 mtime，用来判"源目录有没有被改动"；
# 这个**不含 mtime**，用来判"两份目录的内容是不是同一份"——例如
# "模式1 与传统模式共用同一份预置 Extractor_Output"这种跨工作区等价性证据：
# 两次复制出来的 mtime 必然不同，含 mtime 的摘要没法比。
function Get-DirectoryContentDigest
{
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) { return [pscustomobject]@{ Count = 0; Digest = '' } }
    $rows = @(if ('Harness.Batch' -as [type]) { [Harness.Batch]::Rows($Path) } else { @() })
    if ($rows.Count -eq 0) { return [pscustomobject]@{ Count = 0; Digest = (Get-TextSha256Hex -Text '') } }

    $lines = New-Object System.Collections.ArrayList
    $full = @($rows | ForEach-Object { Join-Path $Path (($_ -split '\|')[0]) })
    $hashes = @([Harness.Batch]::Sha256Many([string[]]$full))
    for ($index = 0; $index -lt $rows.Count; $index++)
    {
        $parts = $rows[$index] -split '\|'
        [void]$lines.Add(('{0}|{1}|{2}' -f $parts[0], $parts[1], $hashes[$index]))
    }
    $sorted = @($lines | Sort-Object)
    return [pscustomobject]@{ Count = $rows.Count; Digest = (Get-TextSha256Hex -Text ($sorted -join "`n")) }
}

# 输出目录清单（相对路径 + 大小 + sha256），走 C# 批量哈希。
function Get-OutputListing
{
    param([Parameter(Mandatory)][string]$Path)

    $items = New-Object System.Collections.ArrayList
    if (-not (Test-Path -LiteralPath $Path)) { return $items.ToArray() }

    if ('Harness.Batch' -as [type])
    {
        $rows = [Harness.Batch]::Rows($Path)
        $full = @($rows | ForEach-Object { Join-Path $Path (($_ -split '\|')[0]) })
        $hashes = [Harness.Batch]::Sha256Many([string[]]$full)
        for ($index = 0; $index -lt $rows.Count; $index++)
        {
            $parts = $rows[$index] -split '\|'
            [void]$items.Add([pscustomobject]@{ rel = $parts[0]; size = [long]$parts[1]; sha256 = $hashes[$index] })
        }
        return $items.ToArray()
    }

    foreach ($file in (Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue | Sort-Object FullName))
    {
        $relative = $file.FullName.Substring([System.IO.Path]::GetFullPath($Path).TrimEnd('\').Length).TrimStart('\')
        [void]$items.Add([pscustomobject]@{ rel = $relative; size = [long]$file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToUpperInvariant() })
    }
    return $items.ToArray()
}

# 期望值清单：小用例读 manifest.json 的 files 数组，大用例读 manifest.tsv。
function Get-ManifestFiles
{
    param([Parameter(Mandatory)][string]$JsonPath, [string]$TsvPath = '')

    if (-not $TsvPath) { $TsvPath = [System.IO.Path]::ChangeExtension($JsonPath, '.tsv') }
    if (Test-Path -LiteralPath $TsvPath)
    {
        $rows = Read-TsvFile -Path $TsvPath
        $items = New-Object System.Collections.ArrayList
        foreach ($row in $rows)
        {
            if ($row.Count -lt 8) { continue }
            [void]$items.Add([pscustomobject]@{
                    category    = $row[0]
                    sourceRel   = $row[1]
                    expectedRel = $(if ($row[2]) { $row[2] } else { $null })
                    knownDir    = $(if ($row[3]) { $row[3] } else { $null })
                    knownFile   = $(if ($row[4]) { $row[4] } else { $null })
                    size        = [long]$row[5]
                    sha256      = $row[6]
                    note        = $row[7]
                    # 扩展集夹具多写了 3 列（采集来源 / 目录哈希 / 文件哈希）。
                    # 这里是**追加**的可选列：既有断言只读前 8 个字段，不受影响；
                    # 不写这三列的旧清单读出来就是 $null（StrictMode 下用 Get-ObjectValue 取）。
                    source      = $(if ($row.Count -gt 8) { $row[8] } else { $null })
                    dirHash     = $(if ($row.Count -gt 9 -and $row[9]) { $row[9] } else { $null })
                    fileHash    = $(if ($row.Count -gt 10 -and $row[10]) { $row[10] } else { $null })
                })
        }
        return $items.ToArray()
    }

    $manifest = Read-JsonFile -Path $JsonPath
    if (-not $manifest) { return @() }
    return @(Get-ObjectValue -Object $manifest -Name 'files' -Default @())
}

# ---------------- 断言结果收集 ----------------

$script:HarnessChecks = New-Object System.Collections.ArrayList

function Reset-HarnessChecks
{
    $script:HarnessChecks = New-Object System.Collections.ArrayList
}

function Add-Check
{
    param(
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$Title,
        [Parameter(Mandatory)][bool]$Ok,
        [string[]]$Details = @(),
        [ValidateSet('fail', 'warn', 'info')][string]$Severity = 'fail'
    )

    [void]$script:HarnessChecks.Add([pscustomobject]@{
        Id       = $Id
        Title    = $Title
        Ok       = $Ok
        Severity = $Severity
        Details  = @($Details)
    })
}

function Show-HarnessChecks
{
    param([string]$Title = '断言结果', [string]$ChecksJson = '')

    Write-Host ''
    Write-Host ("=== {0} ===" -f $Title)
    foreach ($check in $script:HarnessChecks)
    {
        $tag = if ($check.Ok) { '[PASS]' } elseif ($check.Severity -eq 'fail') { '[FAIL]' } else { '[WARN]' }
        Write-Host ("{0} {1}  {2}" -f $tag, $check.Id, $check.Title)
        foreach ($detail in $check.Details)
        {
            Write-Host ("        {0}" -f $detail)
        }
    }

    $failed = @($script:HarnessChecks | Where-Object { -not $_.Ok -and $_.Severity -eq 'fail' })
    $warned = @($script:HarnessChecks | Where-Object { -not $_.Ok -and $_.Severity -ne 'fail' })

    if ($ChecksJson)
    {
        Write-JsonFile -Path $ChecksJson -Object ([ordered]@{
                title     = $Title
                failed    = $failed.Count
                warned    = $warned.Count
                total     = $script:HarnessChecks.Count
                checks    = $script:HarnessChecks
                writtenUtc = (Get-Date).ToUniversalTime().ToString('o')
            })
    }

    Write-Host ''
    if ($failed.Count -eq 0)
    {
        Write-Host ("结论：全部通过（{0} 项；另有 {1} 条提示）" -f $script:HarnessChecks.Count, $warned.Count)
        return 0
    }
    Write-Host ("结论：不通过 —— {0} 项失败 / 共 {1} 项" -f $failed.Count, $script:HarnessChecks.Count)
    return 1
}
