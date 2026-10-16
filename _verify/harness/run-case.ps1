# 驱动真实的 CxdecExtractorUI.dll（配假引擎 CxdecExtractor.dll），验证进度投递与收尾兜底。
#
# 用法：
#   pwsh -File run-case.ps1                 # 常规：假引擎会发完成通知
#   pwsh -File run-case.ps1 -SkipCompleted  # 复刻"收尾通知丢了"：假引擎故意不发 state=4
#
# 判定标准：
#   1) 列表里那一行的"状态"必须变成 已完成（这是用户报的"卡在最后一项"的直接反面）
#   2) 窗口标题里的 完成 计数必须变成 1
#   3) 进度投递次数（日志行数）应被节流到几十条量级，而不是 14196 条

param([switch]$SkipCompleted)

$ErrorActionPreference = 'Stop'
$harness = Split-Path -Parent $MyInvocation.MyCommand.Path
$run = Join-Path $harness 'Run'
$dllDir = Join-Path $run 'CxdecExtractordll'
$xp3 = Join-Path $run 'test.xp3'
$logPath = Join-Path $run 'Log\ExtractorUI.log'

Remove-Item (Join-Path $run 'Log') -Recurse -Force -ErrorAction SilentlyContinue
if ($SkipCompleted) { $env:YUA_FAKE_SKIP_COMPLETED = '1' } else { Remove-Item env:YUA_FAKE_SKIP_COMPLETED -ErrorAction SilentlyContinue }

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class Native {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr LoadLibraryW(string path);
  [DllImport("kernel32.dll")] public static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);
  [DllImport("kernel32.dll")] public static extern IntPtr GlobalLock(IntPtr h);
  [DllImport("kernel32.dll")] public static extern bool GlobalUnlock(IntPtr h);

  public static string ClassOf(IntPtr h) { var s = new StringBuilder(128); GetClassNameW(h, s, 128); return s.ToString(); }
  public static string TitleOf(IntPtr h) { var s = new StringBuilder(512); GetWindowTextW(h, s, 512); return s.ToString(); }

  public static IntPtr FindDialog(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h,l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && ClassOf(h) == "#32770" && TitleOf(h).Contains("Cxdec Batch Extractor")) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }

  // DROPFILES{ pFiles; pt; fNC; fWide } + 宽字符文件名（双 0 结尾）
  public static IntPtr MakeDrop(string file) {
    byte[] name = Encoding.Unicode.GetBytes(file + "\0\0");
    int header = 20;
    IntPtr h = GlobalAlloc(0x0042 /*GMEM_MOVEABLE|GMEM_ZEROINIT*/, (UIntPtr)(header + name.Length));
    IntPtr p = GlobalLock(h);
    Marshal.WriteInt32(p, 0, header);   // pFiles
    Marshal.WriteInt32(p, 16, 1);       // fWide = TRUE
    Marshal.Copy(name, 0, new IntPtr(p.ToInt64() + header), name.Length);
    GlobalUnlock(h);
    return h;
  }

  public static string ListItem(IntPtr list, int row, int sub) {
    const uint LVM_FIRST = 0x1000, LVM_GETITEMTEXTW = LVM_FIRST + 115;
    IntPtr buf = Marshal.AllocHGlobal(1024);
    IntPtr item = Marshal.AllocHGlobal(64);
    for (int i = 0; i < 64; i++) Marshal.WriteByte(item, i, 0);
    // x86 的 LVITEMW 布局：mask(0) iItem(4) iSubItem(8) state(12) stateMask(16)
    //                      pszText(20) cchTextMax(24)
    // 漏掉 stateMask 会把缓冲区指针写进 stateMask、把长度写进 pszText，
    // ListView 会拿 512 当指针去写 → 进程直接崩在 0xC000041D。
    Marshal.WriteInt32(item, 0, 1);              // mask = LVIF_TEXT
    Marshal.WriteInt32(item, 4, row);            // iItem
    Marshal.WriteInt32(item, 8, sub);            // iSubItem
    Marshal.WriteInt32(item, 20, buf.ToInt32()); // pszText（同进程，直接给本进程地址）
    Marshal.WriteInt32(item, 24, 512);           // cchTextMax
    SendMessageW(list, LVM_GETITEMTEXTW, (IntPtr)row, item);
    string text = Marshal.PtrToStringUni(buf) ?? "";
    Marshal.FreeHGlobal(buf); Marshal.FreeHGlobal(item);
    return text;
  }

  public static int ListCount(IntPtr list) { return (int)SendMessageW(list, 0x1004 /*LVM_GETITEMCOUNT*/, IntPtr.Zero, IntPtr.Zero); }

  [DllImport("shell32.dll", CharSet=CharSet.Unicode)] public static extern uint DragQueryFileW(IntPtr hDrop, uint iFile, StringBuilder buf, uint cch);
  public static uint DropCount(IntPtr hDrop) { return DragQueryFileW(hDrop, 0xFFFFFFFF, null, 0); }
}
"@

$uiDll = Join-Path $dllDir 'CxdecExtractorUI.dll'
Write-Host "[case] SkipCompleted=$SkipCompleted  加载 $uiDll"
if ([Native]::LoadLibraryW($uiDll) -eq [IntPtr]::Zero) {
    Write-Host "  !! LoadLibrary 失败，错误码 $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; exit 1
}

$dlg = [IntPtr]::Zero
for ($i = 0; $i -lt 60 -and $dlg -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $dlg = [Native]::FindDialog([uint32]$PID) }
if ($dlg -eq [IntPtr]::Zero) { Write-Host "  !! 没找到解包窗口"; exit 1 }
Write-Host "  窗口: $([Native]::TitleOf($dlg))"

$hdrop = [Native]::MakeDrop($xp3)
Write-Host "  HDROP 自检: 含 $([Native]::DropCount($hdrop)) 个文件"
[void][Native]::PostMessageW($dlg, 0x0233, $hdrop, [IntPtr]::Zero)   # 0x0233 = WM_DROPFILES

$list = [Native]::GetDlgItem($dlg, 1009)
Write-Host "  ListView 句柄: $list"
$final = $null
for ($i = 0; $i -lt 100; $i++) {
    Start-Sleep -Milliseconds 300
    $count = [Native]::ListCount($list)
    if ($i % 5 -eq 0) { Write-Host "    [$i] 行数=$count 标题=$([Native]::TitleOf($dlg))" }
    if ($count -lt 1) { continue }
    $state = [Native]::ListItem($list, 0, 1)
    $progress = [Native]::ListItem($list, 0, 2)
    $detail = [Native]::ListItem($list, 0, 3)
    $final = [pscustomobject]@{State=$state;Progress=$progress;Detail=$detail}
    if ($state -eq '已完成' -or $state -eq '失败') { break }
}
$title = [Native]::TitleOf($dlg)
if ($final) {
    Write-Host "  结果 → 状态='$($final.State)' 进度='$($final.Progress)' 明细='$($final.Detail)'"
} else {
    Write-Host "  结果 → 列表里始终没有任务行"
}
Write-Host "  标题 → $title"

if (Test-Path $logPath) {
    $lines = Get-Content $logPath -Encoding UTF8
    $upd = @($lines | Where-Object { $_ -match 'Task update:' })
    $state4 = @($upd | Where-Object { $_ -match 'state=4' })
    Write-Host "  日志 → 总行 $($lines.Count) / Task update $($upd.Count) 条 / state=4 $($state4.Count) 条"
    Write-Host "  投递量对照：不节流应为 14196 条"
} else { Write-Host "  !! 没有日志文件 $logPath" }

$ok = ($final.State -eq '已完成') -and ($title -match '完成 1')
Write-Host "  >>> 判定: $(if($ok){'通过'}else{'不通过'})"
