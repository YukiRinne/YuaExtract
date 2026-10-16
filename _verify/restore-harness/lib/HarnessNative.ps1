# YuaExtract 验证脚手架：跨进程窗口/控件访问（C# 内联，Add-Type 一次）。
#
# 为什么要这一层：
#   产品界面是**另一个进程**（YuaExtract.exe / 空宿主 EXE），PowerShell 自带的
#   GetWindowText / Get-Process 拿不到子控件文字，也不能读 ListView 内容。
#   这里的做法和 Spy++ 一样：
#     - 顶层窗口的标题：GetWindowTextW 走内核里的标题缓存，跨进程可读；
#     - 子控件文字：在目标进程里 VirtualAllocEx 一块内存，发 WM_GETTEXT
#       把文字写进去，再 ReadProcessMemory 读回来（不能直接传本进程指针）；
#     - ListView：在目标进程里拼 x86 版 LVITEMW（pszText 在偏移 20，
#       漏掉 stateMask 会让对方崩在 0xC000041D），发 LVM_GETITEMTEXTW；
#     - 所有会阻塞的 SendMessage 一律用 SendMessageTimeoutW + SMTO_ABORTIFHUNG，
#       避免对方界面线程卡住时把验证脚本一起拖死。
#
# 只依赖 user32/kernel32，不需要 UI Automation，pwsh 7 / Windows PowerShell 5.1 都能跑。

if (-not ('Harness.Win' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace Harness
{
    public class WinInfo
    {
        public long Hwnd;
        public string HwndHex { get { return "0x" + Hwnd.ToString("X8"); } }
        public uint Pid;
        public int CtrlId;
        public string Class = "";
        public string Text = "";
        public bool Visible;
        public bool Enabled;
        public bool TextReadable = true;
        public int ChildCount;
    }

    public static class Batch
    {
        // 大规模用例专用：几万个文件时，PowerShell 层的 Get-ChildItem / Get-FileHash
        // 每个文件都要走一遍 cmdlet，几十万次下来要几分钟；这里在 C# 里一把过。
        //
        // Rows 的格式："相对路径|大小|修改时间UtcTicks"，按相对路径排序保证可比。
        public static string[] Rows(string root)
        {
            if (!System.IO.Directory.Exists(root)) { return new string[0]; }
            string full = System.IO.Path.GetFullPath(root).TrimEnd('\\', '/');
            List<string> rows = new List<string>();
            foreach (string path in System.IO.Directory.EnumerateFiles(full, "*", System.IO.SearchOption.AllDirectories))
            {
                System.IO.FileInfo info = new System.IO.FileInfo(path);
                string relative = path.Substring(full.Length).TrimStart('\\', '/');
                rows.Add(relative + "|" + info.Length + "|" + info.LastWriteTimeUtc.Ticks);
            }
            rows.Sort(StringComparer.OrdinalIgnoreCase);
            return rows.ToArray();
        }

        public static int Count(string root)
        {
            if (!System.IO.Directory.Exists(root)) { return 0; }
            int count = 0;
            foreach (string path in System.IO.Directory.EnumerateFiles(root, "*", System.IO.SearchOption.AllDirectories)) { count++; }
            return count;
        }

        // 便宜的"目录变化"签名：文件数 | 总字节 | 最新修改时间。
        // 轮询时用它代替拼 6 万行的字符串（后者每轮要几 MB 的字符串分配）。
        public static string Signature(string root)
        {
            if (!System.IO.Directory.Exists(root)) { return "0|0|0"; }
            long count = 0;
            long bytes = 0;
            long newest = 0;
            foreach (string path in System.IO.Directory.EnumerateFiles(root, "*", System.IO.SearchOption.AllDirectories))
            {
                try
                {
                    System.IO.FileInfo info = new System.IO.FileInfo(path);
                    count++;
                    bytes += info.Length;
                    long ticks = info.LastWriteTimeUtc.Ticks;
                    if (ticks > newest) { newest = ticks; }
                }
                catch { }
            }
            return count + "|" + bytes + "|" + newest;
        }

        public static string[] Sha256Many(string[] paths)
        {
            string[] hashes = new string[paths.Length];
            using (System.Security.Cryptography.SHA256 sha = System.Security.Cryptography.SHA256.Create())
            {
                for (int index = 0; index < paths.Length; index++)
                {
                    try
                    {
                        using (System.IO.FileStream stream = new System.IO.FileStream(paths[index], System.IO.FileMode.Open, System.IO.FileAccess.Read, System.IO.FileShare.ReadWrite))
                        {
                            byte[] digest = sha.ComputeHash(stream);
                            StringBuilder text = new StringBuilder(64);
                            for (int i = 0; i < digest.Length; i++) { text.Append(digest[i].ToString("X2")); }
                            hashes[index] = text.ToString();
                        }
                    }
                    catch
                    {
                        hashes[index] = "";
                    }
                }
            }
            return hashes;
        }
    }

    public static class Win
    {
        public const uint WM_GETTEXT = 0x000D;
        public const uint WM_GETTEXTLENGTH = 0x000E;
        public const uint WM_SETTEXT = 0x000C;
        public const uint WM_NULL = 0x0000;
        public const uint WM_CLOSE = 0x0010;
        public const uint WM_COMMAND = 0x0111;
        public const uint BM_CLICK = 0x00F5;
        public const uint BM_GETCHECK = 0x00F0;
        public const uint BM_SETCHECK = 0x00F1;
        public const uint LVM_GETITEMCOUNT = 0x1004;
        public const uint LVM_GETITEMTEXTW = 0x1000 + 115;

        public const uint SMTO_ABORTIFHUNG = 0x0002;
        public const uint SMTO_BLOCK = 0x0001;

        const uint PROCESS_VM_OPERATION = 0x0008;
        const uint PROCESS_VM_READ = 0x0010;
        const uint PROCESS_VM_WRITE = 0x0020;
        const uint PROCESS_QUERY_INFORMATION = 0x0400;
        const uint MEM_COMMIT = 0x1000;
        const uint MEM_RESERVE = 0x2000;
        const uint MEM_RELEASE = 0x8000;
        const uint PAGE_READWRITE = 0x04;

        public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);

        [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr lParam);
        [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr lParam);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hwnd, StringBuilder buffer, int count);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hwnd, StringBuilder buffer, int count);
        [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
        [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);

        [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint pid);
        [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll")] public static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, UIntPtr size, uint allocationType, uint protect);
        [DllImport("kernel32.dll")] public static extern bool VirtualFreeEx(IntPtr process, IntPtr address, UIntPtr size, uint freeType);
        [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, UIntPtr size, out UIntPtr read);
        [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, UIntPtr size, out UIntPtr written);

        // ---------- 基础读取 ----------

        public static string ClassOf(IntPtr hwnd)
        {
            StringBuilder buffer = new StringBuilder(256);
            GetClassNameW(hwnd, buffer, buffer.Capacity);
            return buffer.ToString();
        }

        public static string CaptionOf(IntPtr hwnd)
        {
            StringBuilder buffer = new StringBuilder(1024);
            GetWindowTextW(hwnd, buffer, buffer.Capacity);
            return buffer.ToString();
        }

        public static uint PidOf(IntPtr hwnd)
        {
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            return pid;
        }

        // 界面线程卡住时 SendMessage 会把调用方一起拖死，所以统一走超时版本。
        public static IntPtr SendTimed(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam, uint timeoutMs)
        {
            IntPtr result;
            IntPtr ok = SendMessageTimeoutW(hwnd, message, wParam, lParam, SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, out result);
            return ok == IntPtr.Zero ? new IntPtr(-1) : result;
        }

        public static bool IsHung(IntPtr hwnd)
        {
            return SendTimed(hwnd, WM_NULL, IntPtr.Zero, IntPtr.Zero, 800).ToInt64() == -1;
        }

        // 界面往返延迟：给目标窗口发一个 WM_NULL，量它多久被界面线程处理完。
        // 这是"界面有没有被后台活儿卡住"的量化依据（T-1 靠它）。
        // 超时返回 -1（调用方自己决定算不算卡死）。
        public static int ProbeLatency(IntPtr hwnd, uint timeoutMs)
        {
            System.Diagnostics.Stopwatch watch = System.Diagnostics.Stopwatch.StartNew();
            IntPtr ok = SendMessageTimeoutW(hwnd, WM_NULL, IntPtr.Zero, IntPtr.Zero, SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, out IntPtr result);
            watch.Stop();
            if (ok == IntPtr.Zero) { return -1; }
            return (int)watch.ElapsedMilliseconds;
        }

        // ---------- 远程内存 ----------

        public static IntPtr OpenForRemote(uint pid)
        {
            return OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, false, pid);
        }

        static IntPtr Alloc(IntPtr process, int bytes)
        {
            return VirtualAllocEx(process, IntPtr.Zero, (UIntPtr)(uint)bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }

        // 读子控件文字：远端 WM_GETTEXT + ReadProcessMemory。target 必须是 32 位进程，
        // 指针按 4 字节写（本脚本自身是 64 位，不能直接传 IntPtr）。
        public static string RemoteText(long hwndValue, uint pid, int maxChars)
        {
            IntPtr hwnd = new IntPtr(hwndValue);
            if (!IsWindow(hwnd)) { return null; }

            IntPtr process = OpenForRemote(pid);
            if (process == IntPtr.Zero) { return null; }

            IntPtr buffer = IntPtr.Zero;
            try
            {
                long length = SendTimed(hwnd, WM_GETTEXTLENGTH, IntPtr.Zero, IntPtr.Zero, 1500).ToInt64();
                if (length < 0) { return null; }
                int want = (int)Math.Min(length, (long)maxChars);
                int allocChars = want + 8;
                buffer = Alloc(process, allocChars * 2);
                if (buffer == IntPtr.Zero) { return null; }

                IntPtr copied = SendTimed(hwnd, WM_GETTEXT, (IntPtr)(want + 1), (IntPtr)buffer.ToInt32(), 3000);
                if (copied.ToInt64() < 0) { return null; }

                byte[] raw = new byte[allocChars * 2];
                UIntPtr read;
                if (!ReadProcessMemory(process, buffer, raw, (UIntPtr)(uint)raw.Length, out read)) { return null; }

                int chars = (int)Math.Min((long)allocChars, (long)(read.ToUInt64() / 2));
                StringBuilder text = new StringBuilder(chars);
                for (int index = 0; index < chars; index++)
                {
                    char ch = (char)(raw[index * 2] | (raw[index * 2 + 1] << 8));
                    if (ch == '\0') { break; }
                    text.Append(ch);
                }
                return text.ToString();
            }
            finally
            {
                if (buffer != IntPtr.Zero) { VirtualFreeEx(process, buffer, UIntPtr.Zero, MEM_RELEASE); }
                CloseHandle(process);
            }
        }

        // ---------- 系统消息（WM_GETTEXT / WM_SETTEXT）----------
        //
        // 关键区别：WM_GETTEXT / WM_SETTEXT 属于"会被系统编组(marshal)"的消息，
        // 窗口管理器会把字符串在两边进程的地址空间之间搬一次。所以这里必须传
        // **本进程**的缓冲区指针，由系统搬过去；反过来传远端指针是不行的
        // （实测：传远端指针时文本读不到、也写不进去）。
        // 而 LVM_* 这类 ≥ WM_USER 的自定义消息不会被编组，必须用远端缓冲区，
        // 所以下面 RemoteListViewText 走的是另一条路。

        static IntPtr g_readBuffer = IntPtr.Zero;
        static int g_readChars = 0;
        static IntPtr g_writeBuffer = IntPtr.Zero;
        static int g_writeBytes = 0;

        static IntPtr EnsureLocalBuffer(ref IntPtr buffer, ref int capacity, int units)
        {
            if (buffer != IntPtr.Zero && capacity >= units) { return buffer; }
            int want = Math.Max(units, 8192);
            if (buffer != IntPtr.Zero) { Marshal.FreeHGlobal(buffer); }
            buffer = Marshal.AllocHGlobal(want * 2);
            capacity = want;
            return buffer;
        }

        // 读控件文字（编组方式）。读不到返回 null。
        public static string LocalText(long hwndValue, int maxChars)
        {
            IntPtr hwnd = new IntPtr(hwndValue);
            if (!IsWindow(hwnd)) { return null; }
            IntPtr buffer = EnsureLocalBuffer(ref g_readBuffer, ref g_readChars, maxChars + 8);
            Marshal.WriteInt16(buffer, 0, 0);   // 清掉上一次的内容，失败时不会读到旧值
            IntPtr copied = SendTimed(hwnd, WM_GETTEXT, (IntPtr)(maxChars + 1), buffer, 3000);
            if (copied.ToInt64() < 0) { return null; }
            return Marshal.PtrToStringUni(buffer);
        }

        // 写控件文字（编组方式）。写没写进去由调用方回读确认。
        public static bool LocalSetText(long hwndValue, string text)
        {
            IntPtr hwnd = new IntPtr(hwndValue);
            if (!IsWindow(hwnd)) { return false; }
            byte[] raw = Encoding.Unicode.GetBytes((text ?? "") + "\0");
            IntPtr buffer = EnsureLocalBuffer(ref g_writeBuffer, ref g_writeBytes, raw.Length / 2 + 2);
            Marshal.Copy(raw, 0, buffer, raw.Length);
            IntPtr result = SendTimed(hwnd, WM_SETTEXT, IntPtr.Zero, buffer, 5000);
            return result.ToInt64() != -1;
        }

        // 两条路都试：编组的先来，读不出内容再退回远端缓冲区那套。
        public static string SmartText(long hwndValue, uint pid, int maxChars)
        {
            string local = LocalText(hwndValue, maxChars);
            if (!string.IsNullOrEmpty(local)) { return local; }
            string remote = RemoteText(hwndValue, pid, maxChars);
            return remote ?? local;
        }

        // 写子控件文字（例如主界面的"游戏 exe"编辑框）。同样要写到目标进程里去。
        public static bool SetRemoteText(long hwndValue, uint pid, string text)
        {
            IntPtr hwnd = new IntPtr(hwndValue);
            if (!IsWindow(hwnd)) { return false; }

            IntPtr process = OpenForRemote(pid);
            if (process == IntPtr.Zero) { return false; }

            IntPtr buffer = IntPtr.Zero;
            try
            {
                byte[] raw = Encoding.Unicode.GetBytes(text + "\0");
                buffer = Alloc(process, raw.Length);
                if (buffer == IntPtr.Zero) { return false; }

                UIntPtr written;
                if (!WriteProcessMemory(process, buffer, raw, (UIntPtr)(uint)raw.Length, out written)) { return false; }

                IntPtr result = SendTimed(hwnd, WM_SETTEXT, IntPtr.Zero, (IntPtr)buffer.ToInt32(), 3000);
                return result.ToInt64() != -1;
            }
            finally
            {
                if (buffer != IntPtr.Zero) { VirtualFreeEx(process, buffer, UIntPtr.Zero, MEM_RELEASE); }
                CloseHandle(process);
            }
        }

        // x86 LVITEMW 布局：mask(0) iItem(4) iSubItem(8) state(12) stateMask(16)
        //                    pszText(20) cchTextMax(24)
        public static string RemoteListViewText(long listValue, uint pid, int row, int subItem)
        {
            IntPtr list = new IntPtr(listValue);
            IntPtr process = OpenForRemote(pid);
            if (process == IntPtr.Zero) { return null; }

            IntPtr structBuffer = IntPtr.Zero;
            IntPtr textBuffer = IntPtr.Zero;
            try
            {
                const int textChars = 1024;
                structBuffer = Alloc(process, 64);
                textBuffer = Alloc(process, textChars * 2);
                if (structBuffer == IntPtr.Zero || textBuffer == IntPtr.Zero) { return null; }

                byte[] item = new byte[64];
                BitConverter.GetBytes(1).CopyTo(item, 0);                       // mask = LVIF_TEXT
                BitConverter.GetBytes(row).CopyTo(item, 4);                     // iItem
                BitConverter.GetBytes(subItem).CopyTo(item, 8);                 // iSubItem
                BitConverter.GetBytes(0).CopyTo(item, 12);                      // state
                BitConverter.GetBytes(0).CopyTo(item, 16);                      // stateMask
                BitConverter.GetBytes(textBuffer.ToInt32()).CopyTo(item, 20);    // pszText
                BitConverter.GetBytes(textChars).CopyTo(item, 24);              // cchTextMax

                UIntPtr written;
                if (!WriteProcessMemory(process, structBuffer, item, (UIntPtr)(uint)item.Length, out written)) { return null; }

                IntPtr copied = SendTimed(list, LVM_GETITEMTEXTW, (IntPtr)row, (IntPtr)structBuffer.ToInt32(), 2000);
                if (copied.ToInt64() < 0) { return null; }

                byte[] raw = new byte[textChars * 2];
                UIntPtr read;
                if (!ReadProcessMemory(process, textBuffer, raw, (UIntPtr)(uint)raw.Length, out read)) { return null; }

                StringBuilder text = new StringBuilder(textChars);
                for (int index = 0; index < textChars; index++)
                {
                    char ch = (char)(raw[index * 2] | (raw[index * 2 + 1] << 8));
                    if (ch == '\0') { break; }
                    text.Append(ch);
                }
                return text.ToString();
            }
            finally
            {
                if (structBuffer != IntPtr.Zero) { VirtualFreeEx(process, structBuffer, UIntPtr.Zero, MEM_RELEASE); }
                if (textBuffer != IntPtr.Zero) { VirtualFreeEx(process, textBuffer, UIntPtr.Zero, MEM_RELEASE); }
                CloseHandle(process);
            }
        }

        // ---------- 枚举 ----------

        public static WinInfo[] TopLevel(uint pid)
        {
            List<WinInfo> found = new List<WinInfo>();
            EnumWindows(delegate(IntPtr hwnd, IntPtr lParam)
            {
                if (pid != 0)
                {
                    uint owner;
                    GetWindowThreadProcessId(hwnd, out owner);
                    if (owner != pid) { return true; }
                }
                found.Add(Describe(hwnd, false));
                return true;
            }, IntPtr.Zero);
            return found.ToArray();
        }

        public static WinInfo[] Children(long parentValue)
        {
            IntPtr parent = new IntPtr(parentValue);
            List<WinInfo> found = new List<WinInfo>();
            EnumChildWindows(parent, delegate(IntPtr hwnd, IntPtr lParam)
            {
                found.Add(Describe(hwnd, true));
                return true;
            }, IntPtr.Zero);
            return found.ToArray();
        }

        // 顶层窗口 + 它的直接子控件，一次性描述（供诊断输出用）。
        public static WinInfo[] DescribeTree(long hwndValue, uint pid)
        {
            IntPtr hwnd = new IntPtr(hwndValue);
            List<WinInfo> found = new List<WinInfo>();
            found.Add(Describe(hwnd, true));
            EnumChildWindows(hwnd, delegate(IntPtr child, IntPtr lParam)
            {
                WinInfo info = Describe(child, true);
                found.Add(info);
                return true;
            }, IntPtr.Zero);
            return found.ToArray();
        }

        static WinInfo Describe(IntPtr hwnd, bool readText)
        {
            WinInfo info = new WinInfo();
            info.Hwnd = hwnd.ToInt64();
            info.Pid = PidOf(hwnd);
            info.CtrlId = GetDlgCtrlID(hwnd);
            info.Class = ClassOf(hwnd);
            info.Text = CaptionOf(hwnd);
            info.Visible = IsWindowVisible(hwnd);
            info.Enabled = IsWindowEnabled(hwnd);
            if (readText && info.Text.Length == 0)
            {
                string child = SmartText(info.Hwnd, info.Pid, 4000);
                if (child == null)
                {
                    info.TextReadable = false;
                }
                else
                {
                    info.Text = child;
                }
            }
            return info;
        }

        // ---------- 控件操作 ----------

        public static bool PostClick(long hwndValue)
        {
            return PostMessageW(new IntPtr(hwndValue), BM_CLICK, IntPtr.Zero, IntPtr.Zero);
        }

        public static bool SendClick(long hwndValue)
        {
            IntPtr result = SendTimed(new IntPtr(hwndValue), BM_CLICK, IntPtr.Zero, IntPtr.Zero, 5000);
            return result.ToInt64() != -1;
        }

        public static int CheckState(long hwndValue)
        {
            return (int)SendMessageW(new IntPtr(hwndValue), BM_GETCHECK, IntPtr.Zero, IntPtr.Zero).ToInt64();
        }

        public static int SetCheck(long hwndValue, int state)
        {
            return (int)SendMessageW(new IntPtr(hwndValue), BM_SETCHECK, (IntPtr)state, IntPtr.Zero).ToInt64();
        }

        public static int ListCount(long hwndValue)
        {
            return (int)SendMessageW(new IntPtr(hwndValue), LVM_GETITEMCOUNT, IntPtr.Zero, IntPtr.Zero).ToInt64();
        }

        // ---------- 列表框文字：走 MSAA（oleacc）----------
        //
        // **为什么不用 LB_GETTEXT**（2026-10-15 实测，脚手架自身的缺陷）：它的 lParam 是跨进程指针，
        // 远端缓冲区这条路实测**一个字节都收不到** —— SendMessageW 直接回 LB_ERR(-1)，
        // SendMessageTimeoutW 回"复制了 N 个字符"但缓冲区里原封不动（预填 0xAA 读回来还是 0xAA）。
        // 后果是"列表里明明有会社"却读到空串，连带 Set-ListSelection 的回读也是空 ——
        // 只看计数（LB_GETCOUNT 正常）会以为一切正常。
        // MSAA 的 accName 是**编组的 COM 调用**，跨 32/64 位稳定（实测 accChildCount=2、
        // accName(1)='YuaHarness'）。列表项是 1..accChildCount 的简单子元素（0 是控件自身）。
        [DllImport("oleacc.dll")]
        static extern int AccessibleObjectFromWindow(IntPtr hwnd, uint dwObjectID, ref Guid riid, out IntPtr ppvObject);

        public static object AccessibleClient(long hwndValue)
        {
            IntPtr hwnd = new IntPtr(hwndValue);
            if (!IsWindow(hwnd)) { return null; }
            Guid iid = new Guid("00020400-0000-0000-C000-000000000046"); // IID_IDispatch
            IntPtr pointer;
            int hr = AccessibleObjectFromWindow(hwnd, 0xFFFFFFFCu /*OBJID_CLIENT*/, ref iid, out pointer);
            if (hr != 0 || pointer == IntPtr.Zero) { return null; }
            try { return Marshal.GetObjectForIUnknown(pointer); }
            finally { Marshal.Release(pointer); }
        }

        // ---------- ListBox（扩展集的 4083/4084「会社/作品」是 ListBox，不是 ListView）----------
        //
        // 踩过的坑：拿 ListView 的 LVM_GETITEMCOUNT(0x1004) 去问 ListBox 只会得到 0，
        // 于是"列表里明明有会社"却报"列表是空的"，后面选作品全落空。
        // ListBox 的计数是 LB_GETCOUNT(0x018B)。
        public static int ListBoxCount(long hwndValue)
        {
            return (int)SendMessageW(new IntPtr(hwndValue), 0x018B /*LB_GETCOUNT*/, IntPtr.Zero, IntPtr.Zero).ToInt64();
        }

        // LB_GETTEXT(0x0189) 的 lParam 是**指针**，不会被系统编组 —— 必须在目标进程里分配缓冲区，
        // 照 RemoteText 那套做（x86 远端指针按 4 字节传）。
        public static string ListBoxText(long hwndValue, uint pid, int index, int maxChars)
        {
            IntPtr list = new IntPtr(hwndValue);
            if (!IsWindow(list)) { return null; }

            int length = (int)SendTimed(list, 0x018A /*LB_GETTEXTLEN*/, (IntPtr)index, IntPtr.Zero, 1500).ToInt64();
            if (length < 0) { return null; }
            int want = Math.Min(length, maxChars);

            IntPtr process = OpenForRemote(pid);
            if (process == IntPtr.Zero) { return null; }

            IntPtr buffer = IntPtr.Zero;
            try
            {
                int allocChars = want + 8;
                buffer = Alloc(process, allocChars * 2);
                if (buffer == IntPtr.Zero) { return null; }

                IntPtr copied = SendTimed(list, 0x0189 /*LB_GETTEXT*/, (IntPtr)index, (IntPtr)buffer.ToInt32(), 3000);
                if (copied.ToInt64() < 0) { return null; }

                byte[] raw = new byte[allocChars * 2];
                UIntPtr read;
                if (!ReadProcessMemory(process, buffer, raw, (UIntPtr)(uint)raw.Length, out read)) { return null; }

                int chars = (int)Math.Min((long)allocChars, (long)(read.ToUInt64() / 2));
                StringBuilder text = new StringBuilder(chars);
                for (int i = 0; i < chars; i++)
                {
                    char ch = (char)(raw[i * 2] | (raw[i * 2 + 1] << 8));
                    if (ch == '\0') { break; }
                    text.Append(ch);
                }
                return text.ToString();
            }
            finally
            {
                if (buffer != IntPtr.Zero) { VirtualFreeEx(process, buffer, UIntPtr.Zero, MEM_RELEASE); }
                CloseHandle(process);
            }
        }

        // LB_SETCURSEL(0x0186)：选中第 index 项（跨进程安全，不带指针）。
        public static int SelectListBoxItem(long hwndValue, int index)
        {
            return (int)SendMessageW(new IntPtr(hwndValue), 0x0186, (IntPtr)index, IntPtr.Zero).ToInt64();
        }

        // 页签控件里有几项（TCM_GETITEMCOUNT，不带指针，跨进程安全）。
        // 参考图里"实时日志"上是两个页签（FileNameHash / DirectoryHash），
        // 类名是 SysTabControl32；条目数为 0 就说明页签没插进去。
        public static int TabItemCount(long hwndValue)
        {
            return (int)SendMessageW(new IntPtr(hwndValue), 0x1300 + 4 /*TCM_GETITEMCOUNT*/, IntPtr.Zero, IntPtr.Zero).ToInt64();
        }

        // 切页签（扩展集驱动的「制作扩展集」页签靠它）。
        //
        // 只发 TCM_SETCURSEL 是不够的：产品通常在 TCN_SELCHANGE 里才真正切页，
        // 而 WM_NOTIFY 的 lParam 是**指针、不会被系统编组** —— 传本进程指针对方会读到垃圾。
        // 所以这里在目标进程里放一个 NMHDR{hwndFrom,idFrom,code=TCN_SELCHANGE}，
        // 用 **SendMessageTimeout 同步**发出去（PostMessage 是异步的，回来就 VirtualFreeEx
        // 会把对方还没读的内存释放掉），再释放。
        // 返回 "" 表示成功，否则是失败原因。
        public static string SelectTabItem(long tabValue, long parentValue, int index)
        {
            IntPtr tab = new IntPtr(tabValue);
            if (!IsWindow(tab)) { return "页签控件不存在"; }

            int count = TabItemCount(tabValue);
            if (index < 0 || index >= count) { return ("页签索引越界：" + index + " / 共 " + count + " 项"); }

            SendTimed(tab, 0x1300 + 12 /*TCM_SETCURSEL*/, (IntPtr)index, IntPtr.Zero, 3000);

            IntPtr parent = parentValue != 0 ? new IntPtr(parentValue) : GetParent(tab);
            uint pid = PidOf(parent);
            IntPtr process = OpenForRemote(pid);
            if (process == IntPtr.Zero) { return "OpenProcess 失败（读不到页签宿主进程）"; }

            IntPtr buffer = IntPtr.Zero;
            try
            {
                buffer = Alloc(process, 64);
                if (buffer == IntPtr.Zero) { return "VirtualAllocEx 失败"; }

                byte[] header = new byte[64];
                BitConverter.GetBytes(tab.ToInt32()).CopyTo(header, 0);            // NMHDR.hwndFrom
                BitConverter.GetBytes(GetDlgCtrlID(tab)).CopyTo(header, 4);        // NMHDR.idFrom
                BitConverter.GetBytes(-551).CopyTo(header, 8);                     // TCN_SELCHANGE

                UIntPtr written;
                if (!WriteProcessMemory(process, buffer, header, (UIntPtr)(uint)header.Length, out written))
                {
                    return "WriteProcessMemory 失败";
                }

                SendTimed(parent, 0x004E /*WM_NOTIFY*/, (IntPtr)GetDlgCtrlID(tab), (IntPtr)buffer.ToInt32(), 5000);
                return "";
            }
            finally
            {
                if (buffer != IntPtr.Zero) { VirtualFreeEx(process, buffer, UIntPtr.Zero, MEM_RELEASE); }
                CloseHandle(process);
            }
        }

        // 给主窗口发 WM_COMMAND（例如点主界面上的按钮，或伪造编辑框的 EN_KILLFOCUS 通知）。
        public static bool PostCommand(long parentValue, int controlId, int notifyCode)
        {
            IntPtr wParam = (IntPtr)((notifyCode << 16) | (controlId & 0xFFFF));
            return PostMessageW(new IntPtr(parentValue), WM_COMMAND, wParam, IntPtr.Zero);
        }

        public static bool PostClose(long hwndValue)
        {
            return PostMessageW(new IntPtr(hwndValue), WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        }

        // ---------- 对话框默认按钮 ----------
        //
        // DM_GETDEFID：低 16 位是默认按钮的控件 ID（IDYES=6 / IDNO=7 / IDOK=1），
        // 高 16 位是 DC_HASDEFID(0x534B) 才表示"确实有默认按钮"。
        // 用来验证"清空目录文件"的确认框默认落在「否」上。
        public static int DialogDefaultButtonId(long hwndValue)
        {
            IntPtr result = SendMessageW(new IntPtr(hwndValue), 0x0400 /*DM_GETDEFID*/, IntPtr.Zero, IntPtr.Zero);
            return (int)(result.ToInt64() & 0xFFFF);
        }

        public static bool DialogHasDefaultButton(long hwndValue)
        {
            IntPtr result = SendMessageW(new IntPtr(hwndValue), 0x0400 /*DM_GETDEFID*/, IntPtr.Zero, IntPtr.Zero);
            return ((result.ToInt64() >> 16) & 0xFFFF) == 0x534B;
        }

        // ---------- 控件矩形（布局自检用）----------

        [StructLayout(LayoutKind.Sequential)]
        public struct RECT
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
        [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
        [DllImport("user32.dll")] public static extern int MapWindowPoints(IntPtr from, IntPtr to, ref RECT rect, int count);
        [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr hwnd);

        // 屏幕坐标下的窗口矩形：{left, top, right, bottom}；读不到返回 null。
        public static int[] WindowRect(long hwndValue)
        {
            RECT rect;
            if (!GetWindowRect(new IntPtr(hwndValue), out rect)) { return null; }
            return new int[] { rect.Left, rect.Top, rect.Right, rect.Bottom };
        }

        // 客户区在屏幕坐标下的矩形。
        public static int[] ClientRectOnScreen(long hwndValue)
        {
            IntPtr hwnd = new IntPtr(hwndValue);
            RECT rect;
            if (!GetClientRect(hwnd, out rect)) { return null; }
            MapWindowPoints(hwnd, IntPtr.Zero, ref rect, 2);
            return new int[] { rect.Left, rect.Top, rect.Right, rect.Bottom };
        }

        // ---------- 页签像素回归（task-7）----------
        //
        // 客户区尺寸固定 + 只抓客户区：像素比对要求"两次抓图几何完全一致"，
        // 所以先把客户区调成固定尺寸（AdjustWindowRectEx 算外框、SetWindowPos 应用），
        // 再用 PrintWindow(PW_CLIENTONLY) 只渲染客户区 —— 不含标题栏/边框，热区坐标能直接与控件矩形对上。

        [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int width, int height, uint flags);
        [DllImport("user32.dll")] public static extern uint GetWindowLongW(IntPtr hwnd, int index);
        [DllImport("user32.dll")] public static extern bool AdjustWindowRectEx(ref RECT rect, uint style, bool menu, uint extendedStyle);

        public static string ResizeClientArea(long hwndValue, int clientWidth, int clientHeight)
        {
            IntPtr hwnd = new IntPtr(hwndValue);
            if (!IsWindow(hwnd)) { return "窗口不存在"; }
            uint style = GetWindowLongW(hwnd, -16);    // GWL_STYLE
            uint exStyle = GetWindowLongW(hwnd, -20);  // GWL_EXSTYLE
            RECT rect = new RECT { Left = 0, Top = 0, Right = clientWidth, Bottom = clientHeight };
            if (!AdjustWindowRectEx(ref rect, style, false, exStyle)) { return "AdjustWindowRectEx 失败"; }
            // SWP_NOZORDER(0x0004) | SWP_NOACTIVATE(0x0010)
            if (!SetWindowPos(hwnd, IntPtr.Zero, 0, 0, rect.Right - rect.Left, rect.Bottom - rect.Top, 0x0004 | 0x0010)) { return "SetWindowPos 失败"; }
            return "";
        }

        // ---------- 抓图内核（客户区 / 整窗）----------
        //
        // 为什么要有"超时 + 重试 + 换方法"（全是实测踩出来的）：
        //   1) GetDIBits 要求 hbmp **没有被选入任何 DC**。原写法在位图还选着的时候就取像素，
        //      会**间歇性**返回 0 —— 实测"改尺寸/切页签后连抓 3 次失败 2 次"，报出来就是
        //      "GetDIBits 失败"，看起来像"抓图坏了"。现在取像素前先 SelectObject 把位图摘出来。
        //   2) 光摘出来还不够：PrintWindow 之后立刻 GetDIBits 会跟挂起的 GDI 批处理赛跑，
        //      所以补一次 GdiFlush()。
        //   3) **PrintWindow 本身会无限期阻塞**：它在目标窗口的界面线程里渲染。实测连续切页签时
        //      有一次 PrintWindow 再也不返回，整个验证脚本跟着挂死（CPU 烧着、进程不退出）。
        //      所以 PrintWindow 一律放到**工作线程**上跑 Join(timeout)，超时就判这次尝试作废、
        //      改走兜底方法（后台线程会自己收尾，产物写临时文件，超时那次的文件一律不采信）。
        //   4) 兜底 = "从窗口 DC 直接 BitBlt"：纯同步 GDI，**不依赖目标进程的消息循环**，
        //      不会阻塞。实测两者抓出来的像素一致（同一现场的差异像素数分毫不差：
        //      12,762 / 86,487 / 53,716 px 三组完全相同）。
        //
        // 注意：下面这一整块 P/Invoke + 结构体声明就在本区域内（原来散在"窗口截图"小节里），
        // 删改本区域时**必须连它一起保留**，否则整份 Add-Type 会 CS0103 编译不过（踩过）。

        [DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
        [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
        [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr dc);
        [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int width, int height);
        [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
        [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr obj);
        [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr dc);
        [DllImport("gdi32.dll")] public static extern int GetDIBits(IntPtr dc, IntPtr bitmap, uint start, uint lines, byte[] bits, ref BITMAPINFO info, uint usage);
        [DllImport("gdi32.dll")] public static extern bool GdiFlush();
        [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr dstDc, int x, int y, int width, int height, IntPtr srcDc, int srcX, int srcY, uint rop);

        [StructLayout(LayoutKind.Sequential)]
        public struct BITMAPINFOHEADER
        {
            public int biSize;
            public int biWidth;
            public int biHeight;
            public short biPlanes;
            public short biBitCount;
            public int biCompression;
            public int biSizeImage;
            public int biXPelsPerMeter;
            public int biYPelsPerMeter;
            public int biClrUsed;
            public int biClrImportant;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct BITMAPINFO
        {
            public BITMAPINFOHEADER bmiHeader;
            public int bmiColors;
        }

        const uint SRCCOPY = 0x00CC0020;

        // 上一次成功的抓图用了哪种方法（"printwindow1" / "printwindow2" / "bitblt"）——
        // 断言把这个写进详情，免得"兜底抓的图"被当成"PrintWindow 抓的图"。
        static string g_lastCaptureMethod = "（没抓过）";
        static string g_captureError = "";
        // 默认优先 BitBlt：它不依赖目标进程的消息循环，**不会阻塞**。
        // PrintWindow 实测会无限期挂住（连切页签的那种场景），只能当最后手段。
        static string g_capturePreference = "bitblt";

        public static string LastCaptureMethod()
        {
            return g_lastCaptureMethod;
        }

        // "bitblt"（默认）或 "printwindow"。改它只影响下一次抓图的尝试顺序。
        public static void PreferCaptureMethod(string preference)
        {
            g_capturePreference = (preference == "printwindow") ? "printwindow" : "bitblt";
        }

        public static string CapturePreference()
        {
            return g_capturePreference;
        }

        // 抓一块矩形到 BMP。clientOnly=true 抓客户区（含 BitBlt 兜底时的源偏移换算）。
        // 返回 "" 表示成功，否则是最后一次失败的原因。
        static string CaptureRegionToBmp(IntPtr hwnd, bool clientOnly, string path)
        {
            RECT client;
            RECT window;
            if (!GetClientRect(hwnd, out client)) { return "GetClientRect 失败"; }
            if (!GetWindowRect(hwnd, out window)) { return "GetWindowRect 失败"; }
            int width = clientOnly ? (client.Right - client.Left) : (window.Right - window.Left);
            int height = clientOnly ? (client.Bottom - client.Top) : (window.Bottom - window.Top);
            if (width <= 0 || height <= 0) { return ("窗口/客户区尺寸无效：" + width + "x" + height); }

            // 1 = PW_CLIENTONLY，2 = PW_RENDERFULLCONTENT，3 = BitBlt（不阻塞）
            int primary = clientOnly ? 1 : 2;
            int[] attempts = (g_capturePreference == "printwindow")
                ? new int[] { primary, primary, 3, 3 }
                : new int[] { 3, 3, primary };

            string last = "抓图没有成功过";
            for (int index = 0; index < attempts.Length; index++)
            {
                int mode = attempts[index];
                last = (mode == 3)
                    ? CaptureRegionOnce(hwnd, clientOnly, width, height, path, 3)
                    : CaptureWithTimeout(hwnd, clientOnly, width, height, path, mode, 2500);
                if (last == "") { return ""; }
                System.Threading.Thread.Sleep(70);
            }
            g_lastCaptureMethod = "（全部失败：" + last + "）";
            return last;
        }

        // 在工作线程上跑 PrintWindow（带硬超时）。产物先写临时文件，**只有按时返回**才搬到目标路径：
        // 超时那次的后台线程会继续跑完并写那个临时文件，绝不能让它事后覆盖掉兜底结果。
        static string CaptureWithTimeout(IntPtr hwnd, bool clientOnly, int width, int height, string path, int mode, int timeoutMs)
        {
            string tempPath = path + ".partial" + mode;
            try { if (System.IO.File.Exists(tempPath)) { System.IO.File.Delete(tempPath); } } catch { }

            g_captureError = "抓图线程没有启动";
            System.Threading.Thread worker = new System.Threading.Thread(delegate()
            {
                try { g_captureError = CaptureRegionOnce(hwnd, clientOnly, width, height, tempPath, mode); }
                catch (Exception ex) { g_captureError = ("抓图线程异常：" + ex.Message); }
            });
            worker.IsBackground = true;
            worker.Start();
            if (!worker.Join(timeoutMs))
            {
                return ("PrintWindow(flags=" + mode + ") 超过 " + timeoutMs + " ms 没返回（目标界面线程被占住）—— 本次尝试作废，改走兜底");
            }
            if (g_captureError != "") { return g_captureError; }
            try
            {
                System.IO.File.Copy(tempPath, path, true);
                System.IO.File.Delete(tempPath);
            }
            catch (Exception ex)
            {
                return ("抓图临时文件搬运失败：" + ex.Message);
            }
            return "";
        }

        static string CaptureRegionOnce(IntPtr hwnd, bool clientOnly, int width, int height, string path, int mode)
        {
            IntPtr windowDc = GetWindowDC(hwnd);
            if (windowDc == IntPtr.Zero) { return "GetWindowDC 失败"; }
            IntPtr memoryDc = IntPtr.Zero;
            IntPtr bitmap = IntPtr.Zero;
            IntPtr previous = IntPtr.Zero;
            bool selected = false;
            try
            {
                memoryDc = CreateCompatibleDC(windowDc);
                if (memoryDc == IntPtr.Zero) { return "CreateCompatibleDC 失败"; }
                bitmap = CreateCompatibleBitmap(windowDc, width, height);
                if (bitmap == IntPtr.Zero) { return "CreateCompatibleBitmap 失败"; }
                previous = SelectObject(memoryDc, bitmap);
                selected = true;

                if (mode == 3)
                {
                    // 从窗口 DC 直接拷贝（窗口自己的表面，不受遮挡影响，也不会阻塞）。
                    // 客户区抓图时源起点要减掉边框/标题栏在窗口里的偏移。
                    int offsetX = 0;
                    int offsetY = 0;
                    if (clientOnly)
                    {
                        RECT windowRect;
                        RECT screenRect;
                        if (!GetWindowRect(hwnd, out windowRect)) { return "GetWindowRect 失败"; }
                        if (!GetClientRect(hwnd, out screenRect)) { return "GetClientRect 失败"; }
                        MapWindowPoints(hwnd, IntPtr.Zero, ref screenRect, 2);
                        offsetX = screenRect.Left - windowRect.Left;
                        offsetY = screenRect.Top - windowRect.Top;
                    }
                    if (!BitBlt(memoryDc, 0, 0, width, height, windowDc, offsetX, offsetY, SRCCOPY)) { return "BitBlt 失败"; }
                }
                else
                {
                    bool rendered = PrintWindow(hwnd, memoryDc, (uint)mode);
                    if (!rendered && mode != 0) { rendered = PrintWindow(hwnd, memoryDc, 0u); }
                    if (!rendered) { return ("PrintWindow 失败（flags=" + mode + "）"); }
                }

                GdiFlush();                          // 把挂起的 GDI 批处理刷出去（见上面第 2 条）
                SelectObject(memoryDc, previous);     // GetDIBits 要求 hbmp 不在任何 DC 里
                selected = false;

                // **每行按 4 字节对齐**：GetDIBits 写回的 DIB 行宽是 ((width*3+3)/4)*4，
                // 而缓冲区原先按 width*height*3 分配 —— 只要 width*3 % 4 != 0（比如宽度 1150：
                // 1150*3=3450，对齐后 3452），GetDIBits 每行就会多写 2 字节，
                // **整幅图溢出 1,590 字节，直接踩烂 CLR 堆**：表现为抓几张之后
                // "Fatal error. Internal CLR error. (0x80131506)" 然后进程挂死、CPU 烧着。
                // 这是本脚手架最隐蔽的一个坑（宽度 1164 时 1164*3=3492 恰好对齐，所以一直没暴露）。
                int stride = ((width * 3 + 3) / 4) * 4;
                BITMAPINFO info = new BITMAPINFO();
                info.bmiHeader.biSize = Marshal.SizeOf(typeof(BITMAPINFOHEADER));
                info.bmiHeader.biWidth = width;
                // 高度用正数（自下而上）—— 写负数会让这台机器的驱动回填上下颠倒（README 踩坑 #8）
                info.bmiHeader.biHeight = height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 24;
                info.bmiHeader.biCompression = 0;
                info.bmiHeader.biSizeImage = stride * height;

                byte[] pixels = new byte[stride * height];
                if (GetDIBits(memoryDc, bitmap, 0u, (uint)height, pixels, ref info, 0u) == 0) { return "GetDIBits 失败"; }

                string writeError = WriteBmpFile(width, height, pixels, stride, path);
                if (writeError != "") { return writeError; }
                g_lastCaptureMethod = (mode == 3) ? "bitblt" : ("printwindow" + mode);
                return "";
            }
            finally
            {
                if (selected && previous != IntPtr.Zero) { SelectObject(memoryDc, previous); }
                if (bitmap != IntPtr.Zero) { DeleteObject(bitmap); }
                if (memoryDc != IntPtr.Zero) { DeleteDC(memoryDc); }
                ReleaseDC(hwnd, windowDc);
            }
        }

        // 手写 24 位 BMP（不依赖 System.Drawing：pwsh 7 上不一定有那个程序集）。
        // pixels 是 GetDIBits 写出来的 DIB：**每行 stride（已按 4 字节对齐）字节、自下而上**，
        // 与 BMP 文件里的行布局一致，所以整块拷过去就行（这里也正是原实现漏掉 padding 的地方）。
        static string WriteBmpFile(int width, int height, byte[] pixels, int stride, string path)
        {
            byte[] file = new byte[54 + stride * height];
            file[0] = (byte)'B';
            file[1] = (byte)'M';
            BitConverter.GetBytes(file.Length).CopyTo(file, 2);
            BitConverter.GetBytes(54).CopyTo(file, 10);
            BitConverter.GetBytes(40).CopyTo(file, 14);
            BitConverter.GetBytes(width).CopyTo(file, 18);
            BitConverter.GetBytes(height).CopyTo(file, 22);
            BitConverter.GetBytes((short)1).CopyTo(file, 26);
            BitConverter.GetBytes((short)24).CopyTo(file, 28);
            BitConverter.GetBytes(stride * height).CopyTo(file, 34);
            Array.Copy(pixels, 0, file, 54, stride * height);
            try
            {
                string directory = System.IO.Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(directory) && !System.IO.Directory.Exists(directory)) { System.IO.Directory.CreateDirectory(directory); }
                System.IO.File.WriteAllBytes(path, file);
            }
            catch (Exception ex) { return ("写 BMP 失败：" + ex.Message); }
            return "";
        }

        // 抓**客户区**（PrintWindow PW_CLIENTONLY，带硬超时 + 重试 + BitBlt 兜底）。返回 "" 表示成功。
        public static string CaptureClientToBmp(long hwndValue, string path)
        {
            return CaptureRegionToBmp(new IntPtr(hwndValue), true, path);
        }

        // 抓**整窗**（PrintWindow PW_RENDERFULLCONTENT，带硬超时 + 重试 + BitBlt 兜底）。返回 "" 表示成功。
        // run-host-load.ps1 的"控制台截图"用它。
        public static string CaptureWindowToBmp(long hwndValue, string path)
        {
            return CaptureRegionToBmp(new IntPtr(hwndValue), false, path);
        }

    }
}
'@
}

# ---------------- 列表框条目文字（MSAA） ----------------
#
# 跨进程读列表框文字**只能走这条路**（原因见类里 AccessibleClient 的注释：LB_GETTEXT 的
# 远端缓冲区实测收不到字节）。返回 @{ Ok; Count; Items; Error }：
#   Ok=$false 表示"这条读法起不来"，调用方要如实标出来，不能当成"列表是空的"。
function Get-MsaaListItems
{
    param([Parameter(Mandatory)][IntPtr]$Hwnd, [int]$MaxItems = 2000)

    $result = [ordered]@{ Ok = $false; Count = 0; Items = @(); Error = '' }
    if ($Hwnd -eq [IntPtr]::Zero) { $result.Error = '窗口句柄是 0'; return [pscustomobject]$result }

    $accessible = $null
    try { $accessible = [Harness.Win]::AccessibleClient($Hwnd.ToInt64()) }
    catch { $result.Error = ('AccessibleClient 抛异常：{0}' -f $_.Exception.Message); return [pscustomobject]$result }
    if (-not $accessible) { $result.Error = 'AccessibleObjectFromWindow 没给出对象'; return [pscustomobject]$result }

    $count = 0
    try { $count = [int]$accessible.accChildCount }
    catch { $result.Error = ('读 accChildCount 失败：{0}' -f $_.Exception.Message); return [pscustomobject]$result }

    $items = New-Object System.Collections.ArrayList
    for ($index = 1; $index -le $count -and $index -le $MaxItems; $index++)
    {
        $name = ''
        try { $name = [string]$accessible.accName($index) } catch { $name = '' }
        if ($null -eq $name) { $name = '' }
        [void]$items.Add($name)
    }
    $result.Ok = $true
    $result.Count = $count
    $result.Items = @($items)
    return [pscustomobject]$result
}
