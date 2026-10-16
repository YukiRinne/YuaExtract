# 在线更新用例编排（task-3）：把"远端仓库"搭在本地夹具上，逐个用例驱动真实界面。
#
#   pwsh -File run-extension-online.ps1
#   pwsh -File run-extension-online.ps1 -SkipSetup      # 复用已准备好的库与仓库
#
# **一个用例一个仓库根、一个夹具服务**：坏法按目录隔离（坏 sha256 的索引不会污染别的用例），
# 也不会出现"上一条用例的服务还在跑、这一条连到了它"这种谜一样的失败。
# 服务在本脚本里起、在本脚本里停（finally 兜底），**不留后台进程**。
#
# 它产出（断言 assert-extension.ps1 读这些）：
#   Run\out\extension-online-summary.json         每个用例的 名称 / 端点 / 驱动结果 / 服务日志 / 退出码
#   Run\out\extension-set-empty.json              空库（列表 + 计数）
#   Run\out\extension-set-one.json                1 条（列表 + 计数 + 详情面板 + 快照）
#   Run\out\extension-set-multi.json              多会社多作品（在线更新装了第二条之后）
#   Run\out\extension-set-open-{none,ok,missing}.json   打开选中扩展集的三种路径
#   Run\out\extension-set-note.json               留言弹窗
#   Run\out\extension-set-check-{clean,bad}.json  运行环境自检（正常 / 故意造坏）
#   Run\out\extension-online-<case>.json          在线更新的每个用例
#
# 为什么 E-18 的"新增那条"要用**第三个游戏**的档案：分类口径（OnlineUpdate.cpp 的 Classify）
# 是"同一会社/作品槽位 **或** 同一目标 exe 摘要"就算本地已有 —— 夹具里 OldGame / RawGame /
# Mode1Game 的 exe 是同一份字节（都借 EmptyHost.exe 当假游戏 exe），摘要相同，用它们会让
# "远端这条是新的"变成"本地已有"。所以这里把 OtherGame（exe 摘要不同）整份拷成一个
# **独立的游戏目录** Run\OnlineGame，在副本里生成档案 —— 不动夹具里任何一个既有工作区。

[CmdletBinding()]
param(
    [string]$ManifestPath = '',
    [string]$RepoRoot = '',
    [string]$OutJson = '',
    # 「本地已有、且要考 Same」那一份的安装用例结果（默认 run-all 生成的 extension-old.json）
    [string]$InstallRunJson = '',
    [string]$FixtureRoot = '',
    # 慢响应用例拖多久（必须 > 产品接收超时 30s）
    [double]$HangSeconds = 40.0,
    # 夹具服务空闲多久自杀（保险：万一本脚本崩了也不留后台进程）
    [int]$ServerIdleTimeoutSec = 120,
    # 每个用例的驱动超时（秒）——超时即杀，记 timeout，不假装通过
    [int]$DriverTimeoutSec = 300,
    # 第三个游戏的会社 / 作品（E-18 的"新增"那一条）
    [string]$OnlinePublisher = 'YuaHarnessOnline',
    [string]$OnlineTitle = 'OnlineWork',
    [switch]$SkipSetup,
    [switch]$SkipBadCases,
    [switch]$SkipBrowserCases,
    # 保留现有库（默认复位成"只有旧作那一份"）
    [switch]$KeepLibrary,
    # 只跑指定的用例（逗号分隔；调试用。给了它就不再复位库、不再重建仓库，
    # 否则"只重跑一条"会把别的用例的前提条件弄没）
    [string]$OnlyCases = '',
    [switch]$KeepServers
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
if (-not $ManifestPath) { $ManifestPath = Join-Path $runRoot 'extension-manifest.json' }
if (-not $RepoRoot) { $RepoRoot = Join-Path $runRoot 'ExtensionRepo' }
if (-not $OutJson) { $OutJson = Join-Path $outRoot 'extension-online-summary.json' }
if (-not $InstallRunJson) { $InstallRunJson = Join-Path $outRoot 'extension-old.json' }
$driverPath = Join-Path $root 'run-extension-ui.ps1'
foreach ($directory in @($runRoot, $outRoot)) { if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null } }

$fixture = Read-JsonFile -Path $ManifestPath
if (-not $fixture) { throw ("缺少扩展集夹具清单：{0}；请先运行 make-extension-fixture.ps1" -f $ManifestPath) }
if (-not $FixtureRoot) { $FixtureRoot = [string](Get-ObjectValue -Object $fixture -Name 'fixtureRoot' -Default '') }
if (-not $FixtureRoot) { throw '夹具清单里没有 fixtureRoot' }

$events = New-Object System.Collections.ArrayList
function Add-Event([string]$Text)
{
    if ($events.Count -lt 3000) { [void]$events.Add(("[{0:HH:mm:ss}] {1}" -f (Get-Date), $Text)) }
    Write-Host ("[online] {0}" -f $Text)
}

# ==================================================================
# 驱动一次 UI 用例（**参数一个数组一次性传**：分两次 splat 会把 -Switch 拆成字符，README 踩坑 #15）
# ==================================================================

function Invoke-Driver
{
    param([string]$Label, [string[]]$Arguments, [int]$TimeoutSeconds = 0)

    if ($TimeoutSeconds -le 0) { $TimeoutSeconds = $DriverTimeoutSec }
    Add-Event ("驱动「{0}」：{1}" -f $Label, ($Arguments -join ' '))
    $stdout = Join-Path $outRoot ('steplog-online-{0}.txt' -f ($Label -replace '[^\w\-]', '_'))
    $stderr = Join-Path $outRoot ('steplog-online-{0}.err.txt' -f ($Label -replace '[^\w\-]', '_'))

    # Start-Process 的 -ArgumentList 传数组是**用空格拼**的，本仓库路径里有空格和 `&`，
    # 所以必须自己给每个参数加引号（run-all.ps1 里踩过这条）。
    function Quote-Argument([string]$Value)
    {
        if ($Value -match '[\s"&]') { return ('"' + ($Value -replace '"', '\"') + '"') }
        return $Value
    }
    $argumentList = @()
    foreach ($item in (@('-NoProfile', '-File', $driverPath) + $Arguments)) { $argumentList += (Quote-Argument ([string]$item)) }

    $process = Start-Process -FilePath 'pwsh' -ArgumentList ($argumentList -join ' ') -PassThru -NoNewWindow `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $code = 0
    if (-not $process.WaitForExit($TimeoutSeconds * 1000))
    {
        Add-Event ("驱动「{0}」超过 {1} 秒：强制结束（记 timeout）" -f $Label, $TimeoutSeconds)
        try { $process.Kill($true) } catch { }
        $code = 124
    }
    else { $code = $process.ExitCode }

    # 收尾：被测程序必须走了（README 踩坑 #16）
    Get-Process YuaExtract -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Add-Event ("驱动「{0}」退出码 {1}" -f $Label, $code)
    return [pscustomobject]@{ ExitCode = $code; StdOut = $stdout; StdErr = $stderr }
}

# ==================================================================
# 夹具服务
# ==================================================================

$script:serverProcess = $null
$script:activeServer = $null

function Start-FixtureServer
{
    param([string]$CaseRoot, [string]$Tag)

    $portFile = Join-Path $outRoot ('online-port-{0}.json' -f $Tag)
    $logFile = Join-Path $outRoot ('online-serve-{0}.log' -f $Tag)
    foreach ($path in @($portFile, $logFile)) { if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force } }

    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = 'pwsh'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($item in @('-NoProfile', '-File', (Join-Path $root 'lib\serve-fixtures.ps1'),
            '-Root', $CaseRoot, '-PortFile', $portFile, '-LogFile', $logFile,
            '-IdleTimeoutSec', "$ServerIdleTimeoutSec", '-Quiet'))
    {
        [void]$info.ArgumentList.Add($item)
    }
    $process = [System.Diagnostics.Process]::Start($info)
    try
    {
        if (-not (Get-Variable -Name OnlineServerOutput -Scope Script -ErrorAction SilentlyContinue)) { $script:OnlineServerOutput = @{} }
        $script:OnlineServerOutput[[int]$process.Id] = @{
            Out = $process.StandardOutput.ReadToEndAsync(); Err = $process.StandardError.ReadToEndAsync()
        }
    }
    catch { }

    # 等服务把端口写出来（证书查找 + 监听一般 1–3 秒）
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $portFile))
    {
        if ($process.HasExited)
        {
            $err = ''
            try { if ($script:OnlineServerOutput[[int]$process.Id].Err.Wait(1000)) { $err = [string]$script:OnlineServerOutput[[int]$process.Id].Err.Result } } catch { }
            throw ("夹具服务启动失败（退出码 {0}）：{1}" -f $process.ExitCode, $err)
        }
        Start-Sleep -Milliseconds 200
    }
    if (-not (Test-Path -LiteralPath $portFile)) { throw ("夹具服务 30 秒内没写出端口文件：{0}（服务日志 {1}）" -f $portFile, $logFile) }
    $info2 = Read-JsonFile -Path $portFile
    if (-not $info2) { throw ("端口文件读不出来：{0}" -f $portFile) }

    $script:serverProcess = $process
    $script:activeServer = [pscustomobject]@{
        Process = $process; Port = [int]$info2.port; Base = [string]$info2.base
        PortFile = $portFile; LogFile = $logFile; Root = $CaseRoot; CertThumbprint = [string]$info2.certThumbprint
    }
    Add-Event ("夹具服务已就绪：{0}（根 {1}）" -f $script:activeServer.Base, $CaseRoot)
    return $script:activeServer
}

function Stop-FixtureServer
{
    if (-not $script:serverProcess) { return }
    $process = $script:serverProcess
    $script:serverProcess = $null
    try
    {
        if (-not $process.HasExited) { $process.Kill(); [void]$process.WaitForExit(5000) }
    }
    catch { }
    # 只确认"我们自己起的那个"已经退出；绝不做无差别的 pwsh 清理（会把别人正在跑的脚本杀掉）
    $exited = $false
    try { $exited = $process.HasExited } catch { $exited = $true }
    Add-Event ("夹具服务已停止（pid {0}，退出={1}）" -f $process.Id, $exited)
}

# ==================================================================
# 搭台：库、第三个游戏、远端仓库
# ==================================================================

$setup = [ordered]@{ performed = $false; steps = @(); libraryArchive = ''; thirdArchive = ''; thirdWorkspace = ''; repoSummary = '' }

if (-not $SkipSetup)
{
    $setup.performed = $true

    # ---------- ⓪ 库复位：**这一组用例要的是确定的起点** ----------
    # 库里必须恰好是"旧作那一份"（E-13 的"1 条"、E-18 的"库清单多一条"都以此为前提）。
    # 不复位的话上一轮跑剩的第二条会让"新增"变成"已是同一份"，断言看起来"过了"其实什么都没测。
    $toolDirForSetup = Join-Path $runRoot 'App'
    $libraryForSetup = Join-Path $toolDirForSetup 'ExtensionLibrary'
    if (-not $KeepLibrary -and (Test-Path -LiteralPath $libraryForSetup))
    {
        [void](Remove-TreeUnder -Path $libraryForSetup -MustBeUnder $runRoot)
        Add-Event ("已复位扩展集库（-KeepLibrary 可保留）：{0}" -f $libraryForSetup)
        $setup.steps += 'reset-library'
    }

    # ---------- ① 本地库里要有"旧作那一份" ----------
    $libraryArchive = ''
    $installRun = Read-JsonFile -Path $InstallRunJson
    if ($installRun -and -not ($setup.steps -contains 'reset-library')) { $libraryArchive = [string](Get-ObjectValue -Object $installRun -Name 'libraryArchive' -Default '') }
    if (-not $libraryArchive -or -not (Test-Path -LiteralPath $libraryArchive))
    {
        Add-Event '库里还没有已安装的那一份：先跑一次「生成 + 安装到扩展集库」'
        # **写到自己的结果文件里**，绝不覆盖 -InstallRunJson（那是 run-all 的 E-3/E-7 用例结果：
        # 它带着 -Step dryrun 与幂等那一轮的记录，覆盖掉会让 E-3 变成"没有干跑文本"）。
        $ownInstallJson = Join-Path $outRoot 'extension-online-install.json'
        $install = Invoke-Driver -Label 'setup-install' -Arguments @('-Step', 'meta,draft,install', '-Game', 'old',
            '-OutJson', $ownInstallJson) -TimeoutSeconds 900
        if ($install.ExitCode -ne 0) { throw ("准备库的前提步骤失败（退出码 {0}）：{1}" -f $install.ExitCode, $install.StdOut) }
        $installRun = Read-JsonFile -Path $ownInstallJson
        $libraryArchive = [string](Get-ObjectValue -Object $installRun -Name 'libraryArchive' -Default '')
        $setup.steps += 'install-old'
    }
    if (-not $libraryArchive -or -not (Test-Path -LiteralPath $libraryArchive))
    {
        throw ("安装用例没有给出库档案路径：{0}（先确认「安装到扩展集库」跑通了）" -f $InstallRunJson)
    }
    $setup.libraryArchive = $libraryArchive
    Add-Event ("本地库档案：{0}" -f $libraryArchive)

    # ---------- ② 第三个游戏（OtherGame 的整份副本）：用来当"远端新增"那一条 ----------
    $thirdWorkspace = Join-Path $runRoot 'OnlineGame'
    $otherWorkspace = [string]$fixture.otherGame.workspace
    if (-not (Test-Path -LiteralPath $otherWorkspace)) { throw ("夹具里没有 OtherGame 工作区：{0}" -f $otherWorkspace) }
    if (Test-Path -LiteralPath $thirdWorkspace) { [void](Remove-TreeUnder -Path $thirdWorkspace -MustBeUnder $runRoot) }
    Copy-Item -LiteralPath $otherWorkspace -Destination $thirdWorkspace -Recurse -Force
    if (Test-Path -LiteralPath (Join-Path $thirdWorkspace 'ExtensionDraft')) { [void](Remove-TreeUnder -Path (Join-Path $thirdWorkspace 'ExtensionDraft') -MustBeUnder $runRoot) }
    $thirdExe = [string]$fixture.otherGame.gameExe
    if (-not (Test-Path -LiteralPath $thirdExe)) { throw ("OtherGame 的 exe 不存在：{0}" -f $thirdExe) }
    $thirdExeCopy = Join-Path $thirdWorkspace (Split-Path -Leaf $thirdExe)
    $setup.thirdWorkspace = $thirdWorkspace
    Add-Event ("第三个游戏目录：{0}（exe {1}）" -f $thirdWorkspace, $thirdExeCopy)

    $sourceRunJson = Join-Path $outRoot 'extension-online-source.json'
    $source = Invoke-Driver -Label 'setup-third-draft' -Arguments @('-Step', 'meta,draft', '-Game', 'other',
        '-WorkspaceOverride', $thirdWorkspace, '-GameExeOverride', $thirdExeCopy,
        '-Publisher', $OnlinePublisher, '-Title', $OnlineTitle,
        '-OutJson', $sourceRunJson) -TimeoutSeconds 900
    if ($source.ExitCode -ne 0) { throw ("第三个游戏的档案生成失败（退出码 {0}）：{1}（{2}）" -f $source.ExitCode, $sourceRunJson, $source.StdOut) }
    $thirdArchive = Join-Path $thirdWorkspace 'ExtensionDraft\rules.int'
    if (-not (Test-Path -LiteralPath $thirdArchive)) { throw ("第三个游戏没有生成出档案：{0}" -f $thirdArchive) }
    $setup.thirdArchive = $thirdArchive
    $setup.steps += 'third-draft'

    # ---------- ③ 自检用例要用的"坏档案" ----------
    # **本编排自己造**，不依赖别的脚本先跑过（run-extension-bad-archives.ps1 也会造一份，
    # 但它属于 E-4 的编排；两条路各自独立，谁先跑都不影响谁）。
    # 用 lib\rules-int.py 的 mutate（它按 §3 规格重算 chunk 表与 CRC），从**库里那份真档案**派生。
    $badDir = Join-Path $outRoot 'online-bad-archives'
    if (Test-Path -LiteralPath $badDir) { [void](Remove-TreeUnder -Path $badDir -MustBeUnder $runRoot) }
    New-Item -ItemType Directory -Path $badDir -Force | Out-Null
    $repoPython = ''
    foreach ($candidate in @('C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'))
    {
        if (Test-Path -LiteralPath $candidate) { $repoPython = $candidate; break }
    }
    if (-not $repoPython)
    {
        $found = Get-Command python -ErrorAction SilentlyContinue
        if ($found) { $repoPython = $found.Source }
    }
    if (-not $repoPython) { throw '找不到 Python 运行时（造坏档案需要它）' }
    & $repoPython (Join-Path $root 'lib\rules-int.py') mutate --in $libraryArchive --outdir $badDir `
        --report (Join-Path $badDir 'mutate-report.json') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw ("rules-int.py mutate 失败（退出码 {0}）" -f $LASTEXITCODE) }
    $setup['badArchives'] = $badDir
    $setup.steps += 'bad-archives'
    Add-Event ("自检用的坏档案已生成：{0}" -f $badDir)

    # ---------- ④ 远端仓库（索引列由 make-online-repo.ps1 独立算）----------
    $repoOut = Join-Path $outRoot 'extension-online-repo.json'
    # 单独调 make-online-repo.ps1（它不是 UI 驱动，不该走 Invoke-Driver 的"收 YuaExtract 进程"逻辑）
    $repoArgs = @('-NoProfile', '-File', (Join-Path $root 'make-online-repo.ps1'),
        '-ManifestPath', $ManifestPath, '-RepoRoot', $RepoRoot, '-OutJson', $repoOut,
        '-OldArchive', $libraryArchive, '-OtherArchive', $thirdArchive, '-HangSeconds', "$HangSeconds")
    Add-Event ('搭建远端仓库：' + ($repoArgs -join ' '))
    $repoLog = Join-Path $outRoot 'steplog-online-repo.txt'
    $repoProcess = Start-Process -FilePath 'pwsh' -ArgumentList (($repoArgs | ForEach-Object {
                if ($_ -match '[\s"&]') { '"' + $_ + '"' } else { $_ } }) -join ' ') -PassThru -NoNewWindow `
        -RedirectStandardOutput $repoLog -RedirectStandardError ($repoLog + '.err')
    if (-not $repoProcess.WaitForExit(300000) -or $repoProcess.ExitCode -ne 0)
    {
        Get-Content -LiteralPath $repoLog -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ('  | ' + $_) }
        throw ("搭建远端仓库失败（退出码 {0}）：{1}" -f $repoProcess.ExitCode, $repoLog)
    }
    Get-Content -LiteralPath $repoLog -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ('  | ' + $_) }
    $setup.repoSummary = $repoOut
    $setup.steps += 'repo'
    Add-Event ("远端仓库就绪：{0}" -f $RepoRoot)
}

$repoInfo = Read-JsonFile -Path (Join-Path $outRoot 'extension-online-repo.json')
if (-not $repoInfo) { throw '读不到远端仓库汇总（Run\out\extension-online-repo.json）；先跑 make-online-repo.ps1' }

# ==================================================================
# 用例清单
# ==================================================================

$cases = New-Object System.Collections.ArrayList

function Add-UiCase
{
    param([string]$Name, [string]$Description, [string[]]$Arguments, [string]$ServerRoot = '', [switch]$NoServer,
        [int]$TimeoutSeconds = 0, [string]$Expected = '', [switch]$HttpBase, [switch]$HideLibrary,
        [string]$ExplicitBase = '')
    [void]$cases.Add([pscustomobject]@{
            Name = $Name; Description = $Description; Arguments = $Arguments
            ServerRoot = $ServerRoot; NoServer = [bool]$NoServer; TimeoutSeconds = $TimeoutSeconds; Expected = $Expected
            HttpBase = [bool]$HttpBase; HideLibrary = [bool]$HideLibrary; ExplicitBase = $ExplicitBase
        })
}

if (-not $SkipBrowserCases)
{
    Add-UiCase -Name 'set-empty' -Description '空库：把库挪开再刷列表' -NoServer -Expected 'publishers=0 / titles=0' `
        -Arguments @('-Step', 'settab,setbrowse', '-Game', 'old', '-LibraryHide',
            '-OutJson', (Join-Path $outRoot 'extension-set-empty.json'))
    Add-UiCase -Name 'set-one' -Description '1 条：库里只有旧作那一份' -NoServer -Expected 'publishers=1 / titles=1' `
        -Arguments @('-Step', 'settab,setbrowse', '-Game', 'old', '-SnapshotTag', 'one',
            '-OutJson', (Join-Path $outRoot 'extension-set-one.json'))
    Add-UiCase -Name 'set-open-none' -Description '打开选中扩展集：什么都没选' -NoServer -Expected '提示先选会社/作品' `
        -Arguments @('-Step', 'settab,setopen', '-Game', 'old', '-SetOpenMode', 'none',
            '-OutJson', (Join-Path $outRoot 'extension-set-open-none.json'))
    Add-UiCase -Name 'set-open-ok' -Description '打开选中扩展集：正常打开（只判路径文本）' -NoServer -Expected '面板「已打开 <路径>」+ 日志同' `
        -Arguments @('-Step', 'settab,setbrowse,setopen', '-Game', 'old', '-SetOpenMode', 'ok', '-SnapshotTag', 'open-ok',
            '-OutJson', (Join-Path $outRoot 'extension-set-open-ok.json'))
    Add-UiCase -Name 'set-open-missing' -Description '打开选中扩展集：库内目录被删' -NoServer -Expected '明确报"目录不存在"' `
        -Arguments @('-Step', 'settab,setbrowse,setopen', '-Game', 'old', '-SetOpenMode', 'missing', '-SnapshotTag', 'open-missing',
            '-OutJson', (Join-Path $outRoot 'extension-set-open-missing.json'))
    Add-UiCase -Name 'set-note' -Description '扩展集留言（只读弹窗）' -NoServer -Expected '框里有 META.comment' `
        -Arguments @('-Step', 'settab,setbrowse,setnote', '-Game', 'old', '-SnapshotTag', 'note',
            '-OutJson', (Join-Path $outRoot 'extension-set-note.json'))
    Add-UiCase -Name 'set-check-clean' -Description '运行环境自检：正常环境' -NoServer -Expected '问题数 0' `
        -Arguments @('-Step', 'settab,setbrowse,setcheck', '-Game', 'old', '-SetSelfCheckVariants', 'clean', '-SnapshotTag', 'check-clean',
            '-OutJson', (Join-Path $outRoot 'extension-set-check-clean.json'))
}

$badArchive = Join-Path $outRoot 'online-bad-archives\version-high.rules.int'
if (-not (Test-Path -LiteralPath $badArchive))
{
    # 兜底：夹具目录里那份（别的编排造出来的）
    $fixtureBad = Join-Path $FixtureRoot 'BadArchives\version-high.rules.int'
    if (Test-Path -LiteralPath $fixtureBad) { $badArchive = $fixtureBad }
    else
    {
        Add-Event ("没有可用的坏档案（{0} / {1}）—— 自检的 bad-archive 变体将报错（记录在 mutationError 里，不会静默）" -f $badArchive, $fixtureBad)
    }
}
if (-not $SkipBrowserCases)
{
    Add-UiCase -Name 'set-check-bad' -Description '运行环境自检：故意造坏（坏档案 / 改一字节 / 清单坏行 / 库不可写 / 缺 DLL）' -NoServer `
        -Expected '每一项都必须报出；自检前后目录摘要全等' `
        -Arguments @('-Step', 'settab,setbrowse,setcheck', '-Game', 'old',
            '-SetSelfCheckVariants', 'bad-archive,byte-flip,manifest-malformed,manifest-bad-digest,library-readonly,missing-dll',
            '-BadArchivePath', $badArchive, '-SnapshotTag', 'check-bad',
            '-OutJson', (Join-Path $outRoot 'extension-set-check-bad.json'))
}

# ---------- 在线更新：**先跑"必须失败"的用例，再跑正例** ----------
#
# 顺序是有讲究的：正例会把远端那一条**装进本地库**，之后任何针对"这一条"的坏用例都会命中
# 「本地已经是同一份 → 无需下载」而根本不走下载路径（实测踩过：截断 / 短读 / 乱字节三个用例
# 全变成了"没测到"，看起来还是绿的）。所以负例必须在库还是干净的时候跑。
# 断言侧另有一道前置复核（E-18）：坏用例的记录里如果出现"无需下载"，直接判用例无效。

if (-not $SkipBadCases)
{
    foreach ($entry in @(
            @{ Name = 'bad-sha256'; Desc = '坏 sha256：索引里的档案摘要错一位' ; Expect = '档案摘要不符' }
            @{ Name = 'truncate'; Desc = '截断：只发前一半' ; Expect = '结构 / 摘要不符' }
            @{ Name = 'short-body'; Desc = '短读：Content-Length 报全长只发一半' ; Expect = '连接被中断 / 读不满' }
            @{ Name = 'garbage'; Desc = '乱字节：200 + 正确长度' ; Expect = '解码失败' }
            @{ Name = 'truncate-declared'; Desc = '截断且索引摘要与截断后一致（sha256 过得去）' ; Expect = '结构 / 自洽性必须拦下' }
            @{ Name = 'salt-mismatch'; Desc = '盐不一致：位置上摆的是别的游戏的档案（索引名与档案 META 一致）' ; Expect = '内嵌 exe 摘要与索引不符' }
            @{ Name = 'index-name-mismatch'; Desc = '索引名与档案 META 名不一致（其余列全对）' ; Expect = '按"名字对不上"拒，且点名两边' }
            @{ Name = 'index-cols'; Desc = '索引列数不对' ; Expect = '索引格式不对（点名行号）' }
            @{ Name = 'index-empty-publisher'; Desc = '索引会社列为空' ; Expect = '索引里有不合规的行' }
            @{ Name = 'index-bad-digest'; Desc = '索引摘要不是 hex' ; Expect = '索引格式不对（整份不可信）' }
            @{ Name = 'index-empty-digest'; Desc = '索引档案摘要为空' ; Expect = '缺档案摘要，拒绝校验' }
            @{ Name = 'missing-archive'; Desc = '索引指向的档案在仓库里不存在' ; Expect = 'HTTP 404' }
        ))
    {
        Add-UiCase -Name ('online-' + $entry.Name) -Description ($entry.Desc + ' → 必须被拒、本地不动') `
            -ServerRoot (Join-Path $RepoRoot $entry.Name) -Expected $entry.Expect `
            -Arguments @('-Step', 'settab,setupdate', '-Game', 'old', '-OnlineInstall',
                '-OutJson', (Join-Path $outRoot ('extension-online-{0}.json' -f $entry.Name))) -TimeoutSeconds 300
    }
}

# ---------- 在线更新正例（本地夹具）：本地库这之前必须还是"只有旧作那一份" ----------
Add-UiCase -Name 'online-good' -Description '在线更新闭环：索引 → 分类 → 下载 → 校验 → 入库 → 幂等' `
    -ServerRoot (Join-Path $RepoRoot 'good') -Expected '库清单 1 → 2 行；第二轮 Same 不重复' `
    -Arguments @('-Step', 'settab,setupdate', '-Game', 'old', '-OnlineInstall', '-OnlineRuns', '2',
        '-OutJson', (Join-Path $outRoot 'extension-online-good.json')) -TimeoutSeconds 400

Add-UiCase -Name 'set-multi' -Description '多会社多作品：在线更新装了第二条之后再刷列表' -NoServer -Expected 'publishers=2 / titles=2' `
    -Arguments @('-Step', 'settab,setbrowse', '-Game', 'old', '-SnapshotTag', 'multi',
        '-OutJson', (Join-Path $outRoot 'extension-set-multi.json'))

# **首次安装**（S-2）：把扩展集库整个挪开（= 从来没装过任何东西的状态）再走在线更新。
# 这一条以前**测不到**：库不存在时 Get-LibraryInfo 返回的 Root 是空串，驱动在
# Get-TreeDigestExcluding 的空串绑定校验上直接崩（未捕获异常、连结果文件都没写出来）。
# 它要证明的是：产品会把库建出来、清单写对（"在线更新第一次把库建出来"这条产品路径）。
Add-UiCase -Name 'online-first-install' -Description '库目录不存在（首次安装）：在线更新应当把库建出来并写对清单' `
    -ServerRoot (Join-Path $RepoRoot 'good') -Expected '库被建出来 + 清单 1 行且与远端索引一致' -HideLibrary `
    -Arguments @('-Step', 'settab,setupdate', '-Game', 'old', '-OnlineInstall',
        '-OutJson', (Join-Path $outRoot 'extension-online-first-install.json')) -TimeoutSeconds 400

# ---------- E-19：端点不可达 / 404 / 超时 ----------
Add-UiCase -Name 'online-no-index' -Description '仓库根下没有 index.txt（真 404）' `
    -ServerRoot (Join-Path $RepoRoot 'no-index') -Expected 'HTTP 404' `
    -Arguments @('-Step', 'settab,setupdate', '-Game', 'old', '-OnlineInstall',
        '-OutJson', (Join-Path $outRoot 'extension-online-no-index.json')) -TimeoutSeconds 300

Add-UiCase -Name 'online-slow' -Description ('index.txt 拖住 {0} 秒不回应（打接收超时 30s）' -f $HangSeconds) `
    -ServerRoot (Join-Path $RepoRoot 'slow') -Expected '超时（连接/发送/接收各自的毫秒数）' `
    -Arguments @('-Step', 'settab,setupdate', '-Game', 'old', '-OnlineInstall',
        '-OutJson', (Join-Path $outRoot 'extension-online-slow.json')) -TimeoutSeconds 300

# 明文 http 基址：产品只走 https（§14.4 第 1 条），必须在**发请求之前**就拒掉。
# 这一条不需要夹具服务（连上都不会连），但要真的有一个"能解析的 http 地址"。
# ---------- 名字会被改写的条目（E-14-b / P-4）：会社『A:B』、作品『Yuzu/Soft』 ----------
# 落库目录会被 sanitize 成 `AB\YuzuSoft`，而 manifest 里记的是原始名 —— 详情面板的「库内目录」
# 必须给**磁盘上真实存在的那一级**并说明"改写过了"（拿原始名拼出来的路径根本不存在）。
# 放在最后：它会（按 exe 摘要）替换掉在线更新装进来的那一条，不该影响前面任何用例。
Add-UiCase -Name 'hostile-install' -Description '安装一个名字会被改写的扩展集（会社 A:B / 作品 Yuzu/Soft）' -NoServer `
    -Expected '落库目录被 sanitize 成 AB\YuzuSoft' `
    -Arguments @('-Step', 'meta,draft,install', '-Game', 'other', '-Publisher', 'A:B', '-Title', 'Yuzu/Soft',
        '-OutJson', (Join-Path $outRoot 'extension-hostile-install.json')) -TimeoutSeconds 600

Add-UiCase -Name 'set-hostile' -Description '浏览改名条目：面板要给实际落库目录 + 改写说明' -NoServer `
    -Expected '库内目录 = 磁盘真实目录（sanitize 后）' `
    -Arguments @('-Step', 'settab,setbrowse', '-Game', 'old', '-SetPublisherName', 'A:B', '-SnapshotTag', 'hostile',
        '-OutJson', (Join-Path $outRoot 'extension-set-hostile.json'))

# N-P2 的第二层：前缀**是** https:// 但缺主机名 → 与"不是 https"是**两句不同的措辞**（判据要能区分）
Add-UiCase -Name 'online-https-no-host' -Description '基址前缀是 https:// 但缺主机名（N-P2 第二层措辞）' -NoServer `
    -ExplicitBase 'https:///' -Expected '基址不完整 / 缺少主机名（**不是**"不是 https"那句）' `
    -Arguments @('-Step', 'settab,setupdate', '-Game', 'old', '-OnlineInstall',
        '-OutJson', (Join-Path $outRoot 'extension-online-https-no-host.json')) -TimeoutSeconds 300

Add-UiCase -Name 'online-http-base' -Description '基址是明文 http（§14.4 第 1 条：只走 https）' -NoServer `
    -Expected '只允许 https 地址' -HttpBase `
    -Arguments @('-Step', 'settab,setupdate', '-Game', 'old', '-OnlineInstall',
        '-OutJson', (Join-Path $outRoot 'extension-online-http-base.json')) -TimeoutSeconds 300

Add-UiCase -Name 'online-unreachable' -Description '端点连不上（端口上什么都没有）' -NoServer -Expected '连不上服务器' `
    -Arguments @('-Step', 'settab,setupdate', '-Game', 'old', '-OnlineInstall',
        '-OutJson', (Join-Path $outRoot 'extension-online-unreachable.json')) -TimeoutSeconds 300

# ==================================================================
# 逐个用例跑
# ==================================================================

$results = New-Object System.Collections.ArrayList
$startedAll = Get-Date
if ($OnlyCases)
{
    $wanted = @(($OnlyCases -split ',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $filtered = @($cases | Where-Object { $_.Name -in $wanted })
    $missing = @($wanted | Where-Object { $_ -notin @($cases | ForEach-Object { $_.Name }) })
    if ($missing.Count -gt 0) { throw ("-OnlyCases 里有未知用例：{0}" -f ($missing -join ', ')) }
    $cases = $filtered
    Add-Event ("-OnlyCases 生效：只跑 {0} 个用例（不改库、不重建仓库）" -f $filtered.Count)
}
foreach ($case in $cases)
{
    Write-Host ''
    Write-Host ('------------------------------------------------------------------')
    Write-Host (' >> {0}：{1}' -f $case.Name, $case.Description)
    Write-Host ('------------------------------------------------------------------')
    Add-Event ("用例 {0} 开始：{1}" -f $case.Name, $case.Description)
    $caseStarted = Get-Date

    $endpoint = ''
    $serverInfo = $null
    $failure = ''
    $code = 0
    $arguments = @($case.Arguments)
    # 库挪开的用例：跑之前挪走、跑完**无论成败**都挪回来（否则后面的用例都没有库了）
    $libraryPath = Join-Path (Join-Path $runRoot 'App') 'ExtensionLibrary'
    $hiddenLibrary = ''

    try
    {
        if ($case.HideLibrary)
        {
            if (Test-Path -LiteralPath $libraryPath)
            {
                $hiddenLibrary = ('{0}.harness-hidden-{1}' -f $libraryPath, $PID)
                if (Test-Path -LiteralPath $hiddenLibrary) { [void](Remove-TreeUnder -Path $hiddenLibrary -MustBeUnder $runRoot) }
                Move-Item -LiteralPath $libraryPath -Destination $hiddenLibrary
                Add-Event ("已把扩展集库暂时挪开（首次安装现场）：{0} → {1}" -f $libraryPath, $hiddenLibrary)
            }
            else { Add-Event '扩展集库本来就不存在（正好是首次安装的现场）' }
        }

        if (-not $case.NoServer)
        {
            $serverInfo = Start-FixtureServer -CaseRoot $case.ServerRoot -Tag $case.Name
            $endpoint = [string]$serverInfo.Base
            $arguments += @('-Endpoint', $endpoint)
        }
        elseif ($case.ExplicitBase)
        {
            # 显式给一个"病态基址"（不需要服务：连都不会连）
            $endpoint = [string]$case.ExplicitBase
            $arguments += @('-Endpoint', $endpoint)
        }
        elseif ($case.HttpBase)
        {
            # 明文 http 的基址（随便挑一个端口，反正连都不会连）
            $listener2 = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Parse('127.0.0.1'), 0)
            $listener2.Start()
            $httpPort = ([System.Net.IPEndPoint]$listener2.LocalEndpoint).Port
            $listener2.Stop()
            $endpoint = ('http://127.0.0.1:{0}/' -f $httpPort)
            $arguments += @('-Endpoint', $endpoint)
        }
        elseif ($case.Name -eq 'online-unreachable')
        {
            # "连不上"要一个**真的没人监听**的端口：借系统挑一个、立刻放掉。
            $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Parse('127.0.0.1'), 0)
            $listener.Start()
            $deadPort = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
            $listener.Stop()
            $endpoint = ('https://127.0.0.1:{0}/' -f $deadPort)
            $arguments += @('-Endpoint', $endpoint)
        }

        $result = Invoke-Driver -Label $case.Name -Arguments $arguments -TimeoutSeconds $case.TimeoutSeconds
        $code = $result.ExitCode
    }
    catch
    {
        $failure = $_.Exception.Message
        Add-Event ("用例 {0} 出错：{1}" -f $case.Name, $failure)
    }
    finally
    {
        if (-not $KeepServers) { Stop-FixtureServer }
        if ($hiddenLibrary)
        {
            # 产品刚建出来的那份库先删掉，再把原来的挪回来（不然两份会混在一起）
            if (Test-Path -LiteralPath $libraryPath) { [void](Remove-TreeUnder -Path $libraryPath -MustBeUnder $runRoot) }
            if (Test-Path -LiteralPath $hiddenLibrary) { Move-Item -LiteralPath $hiddenLibrary -Destination $libraryPath }
            Add-Event ("扩展集库已挪回：{0}" -f $libraryPath)
        }
    }

    # 把每个用例的结果记下来（断言只读这份汇总 + 各自的 run JSON + 服务日志）
    $runJson = ''
    for ($index = 0; $index -lt $arguments.Count - 1; $index++)
    {
        if ($arguments[$index] -eq '-OutJson') { $runJson = [string]$arguments[$index + 1] }
    }
    $record = [pscustomobject]@{
        Name         = $case.Name
        Description  = $case.Description
        Expected     = $case.Expected
        ExitCode     = $code
        Failure      = $failure
        Endpoint     = $endpoint
        ServerRoot   = [string]$case.ServerRoot
        ServerLog    = $(if ($serverInfo) { [string]$serverInfo.LogFile } else { '' })
        ServerPort   = $(if ($serverInfo) { [int]$serverInfo.Port } else { 0 })
        CertThumbprint = $(if ($serverInfo) { [string]$serverInfo.CertThumbprint } else { '' })
        RunJson      = $runJson
        StdOut       = $(if (Test-Path -LiteralPath (Join-Path $outRoot ('steplog-online-{0}.txt' -f ($case.Name -replace '[^\w\-]', '_')))) { Join-Path $outRoot ('steplog-online-{0}.txt' -f ($case.Name -replace '[^\w\-]', '_')) } else { '' })
        Arguments    = @($arguments)
        DurationSec  = [int]((Get-Date) - $caseStarted).TotalSeconds
    }
    [void]$results.Add($record)
    Write-Host ("   → {0}（退出码 {1}{2}）" -f $case.Name, $code, $(if ($failure) { '，出错：' + $failure } else { '' }))
}

# -OnlyCases 时**合并**进已有汇总：只重跑一条不能把别的用例的结论冲掉
# （断言读同一份汇总，覆盖式写法会让"只重跑一条"看起来像"其他用例消失了"）。
$mergedCases = @($results)
if ($OnlyCases -and (Test-Path -LiteralPath $OutJson))
{
    $previous = Read-JsonFile -Path $OutJson
    if ($previous)
    {
        $byName = [ordered]@{}
        foreach ($case in @(Get-ObjectValue -Object $previous -Name 'cases' -Default @()))
        {
            $name = [string](Get-ObjectValue -Object $case -Name 'name' -Default '')
            if ($name) { $byName[$name] = $case }
        }
        foreach ($case in $results) { $byName[$case.Name] = $case }
        $mergedCases = @($byName.Values)
        Add-Event ("合并进上一份汇总：共 {0} 个用例" -f $mergedCases.Count)
    }
}

$summary = [ordered]@{
    generator    = 'run-extension-online.ps1'
    startedUtc   = $startedAll.ToUniversalTime().ToString('o')
    finishedUtc  = (Get-Date).ToUniversalTime().ToString('o')
    manifestPath = $ManifestPath
    repoRoot     = $RepoRoot
    repoSummary  = (Join-Path $outRoot 'extension-online-repo.json')
    hangSeconds  = $HangSeconds
    onlinePublisher = $OnlinePublisher
    onlineTitle  = $OnlineTitle
    setup        = $setup
    cases        = @($mergedCases)
    events       = @($events)
}
Write-JsonFile -Path $OutJson -Object $summary
Write-Host ''
Write-Host ("[online] 本轮跑 {0} 个用例；汇总里共 {1} 个 → {2}" -f $results.Count, $mergedCases.Count, $OutJson)
$failed = @($results | Where-Object { $_.ExitCode -ne 0 })
if ($failed.Count -gt 0)
{
    Write-Host ("[online] 有 {0} 个用例的驱动非 0 退出：{1}" -f $failed.Count, (($failed | ForEach-Object { $_.Name }) -join ', '))
}
# 驱动非 0 不等于断言失败（"必须被拒"的用例本来就会走失败分支），但**绝不能静默**：
# 退出码只反映"编排本身有没有跑完"，判据在 assert-extension.ps1 那侧。
exit 0
