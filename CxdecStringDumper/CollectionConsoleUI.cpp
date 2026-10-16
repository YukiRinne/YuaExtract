#include "CollectionConsoleUI.h"

#include <Windows.h>
#include <commctrl.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "directory.h"
#include "util.h"

// 进度条 / 页签在 comctl32，打开输出目录走 shell32。工程里没配 AdditionalDependencies，
// 跟 HashRestoreUI.cpp 一样按 TU 写死。
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

// 步骤2「只收集」控制台。
//
// 设计要点（照着上游 RuntimeHashUI / HashRestoreUI 的窗口写法来，但**一行还原逻辑都没有**）：
//   1. 窗口只读映射日志（DirectoryHash.log / FileNameHash.log），不改任何文件名、不写
//      RuntimeHashRestore.log —— 「只收集」模式的意义就在这里，还原交给步骤3 的离线流程；
//   2. 进度分母在启动时扫一遍纯Hash目录（16 位目录哈希数 D、64 位文件哈希数 F），
//      分子是映射日志里哈希落在该集合内的去重条数（取交集）；
//   3. 实时日志用定时器读两份映射日志的尾部新增行。HashCore 每写一行都 fflush，
//      且日志是 _SH_DENYWR 打开的（允许别人读），所以按字节偏移续读就够，
//      不必改 HashCore 的收集回调，也不怕跨线程投递消息出错。

namespace
{
    constexpr wchar_t ConsoleWindowClassName[] = L"CxdecHashCollectConsole";
    constexpr wchar_t ConsoleWindowTitle[] = L"YuaExtract 动态 Hash 收集控制台";

    // 只收集开关。名字是接口契约（Loader 注入本 DLL 前设置），不要改。
    constexpr wchar_t CollectOnlyEnvName[] = L"CXDEC_HASH_COLLECT_ONLY";
    // 纯Hash目录。取不到就退回 <游戏目录>\Extractor_Output。
    constexpr wchar_t TargetDirectoryEnvName[] = L"CXDEC_RUNTIME_HASH_TARGET_DIR";
    // 映射产物目录名 / 日志名，必须与 HashCore::FolderName、HashCore::DirectoryHashFileName、
    // HashCore::FileNameHashFileName 一致。
    constexpr wchar_t MappingFolderName[] = L"StringHashDumper_Output";
    constexpr wchar_t DirectoryHashLogName[] = L"DirectoryHash.log";
    constexpr wchar_t FileNameHashLogName[] = L"FileNameHash.log";
    // 配对分隔符，必须与 HashCore::Split 一致。
    constexpr wchar_t PairSeparator[] = L"##YSig##";

    constexpr int IdcStartGameButton = 4001;
    constexpr int IdcCloseGameButton = 4002;
    constexpr int IdcClearDirectoryButton = 4003;
    constexpr int IdcOpenOutputButton = 4004;
    constexpr int IdcBackToMainButton = 4005;
    constexpr int IdcDirectoryProgressBar = 4006;
    constexpr int IdcDirectoryProgressText = 4007;
    constexpr int IdcFileNameProgressBar = 4008;
    constexpr int IdcFileNameProgressText = 4009;
    constexpr int IdcRestoreRateText = 4010;
    constexpr int IdcLogTab = 4011;
    constexpr int IdcFileNameLogEdit = 4012;
    constexpr int IdcDirectoryLogEdit = 4013;
    constexpr int IdcStatusLine = 4014;

    constexpr UINT_PTR RefreshTimerId = 1u;
    constexpr UINT RefreshIntervalMs = 500u;

    // 页签里最多留多少行 / 多少字符。映射库动辄几万条，全塞进 EDIT 又卡又看不清；
    // 完整内容在 DirectoryHash.log / FileNameHash.log 里，窗口只当"滚动的尾巴"。
    constexpr int MaxLogLinesShown = 1000;
    constexpr int TrimSlackLines = 300;
    constexpr int MaxLogCharsShown = 150000;   // 单次追加的字符上限，也是内容的截断线
    constexpr int TrimSlackChars = 50000;
    // EDIT 的默认文本上限只有 30000 字符，撞上以后 EM_REPLACESEL 会被整条拒绝，
    // 页签会直接冻在 30000 字（实测）。所以创建后显式抬高；真正的天花板是上面的
    // 1000 行 / 150000 字，这里只是留出余量。
    constexpr int LogEditCharLimit = 1000000;
    // 开窗时先垫上已有映射的末尾若干行，让用户一眼看到映射库现状（也顺便证明尾部读取通了）。
    constexpr size_t FirstScanLinesShown = 200u;
    // 单次刷新每份日志最多读多少字节，避免一次几千万字节日志把 UI 线程卡住。
    constexpr unsigned long long MaxBytesPerPump = 4ull * 1024ull * 1024ull;

    constexpr int ClientWidth = 848;
    constexpr int ClientHeight = 626;

    // 实时日志那一组的位置。页签的"显示区"随字体行高变，编辑框按 TCM_GETITEMRECT
    // 算出来再摆，别写死 —— 写死会在不同 DPI/字体下压住页签或露出一条白边。
    constexpr int LogGroupY = 222;
    constexpr int LogGroupHeight = 310;
    constexpr int LogTabX = 18;
    constexpr int LogTabY = 242;
    constexpr int LogTabWidth = 812;
    constexpr int LogTabHeight = 280;

    struct CollectContext;

    void ReleaseContext(CollectContext* context);
    void ScanTargetDirectory(CollectContext* context);
    LRESULT CALLBACK ConsoleWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    DWORD WINAPI ConsoleUiThreadProc(LPVOID parameter);
    DWORD WINAPI TargetScanThreadProc(LPVOID parameter);
    void RefreshConsole(CollectContext* context);

    //**********小工具**********//

    std::wstring FormatString(const wchar_t* format, ...)
    {
        va_list ap;
        va_start(ap, format);
        const int count = _vscwprintf(format, ap);
        va_end(ap);
        if (count <= 0)
        {
            return std::wstring();
        }

        std::wstring buffer(static_cast<size_t>(count), L'\0');
        va_start(ap, format);
        _vsnwprintf_s(&buffer[0], buffer.size() + 1, _TRUNCATE, format, ap);
        va_end(ap);
        return buffer;
    }

    std::wstring CombinePath(const std::wstring& directory, const std::wstring& fileName)
    {
        if (directory.empty())
        {
            return fileName;
        }
        if (directory.back() == L'\\' || directory.back() == L'/')
        {
            return directory + fileName;
        }
        return directory + L'\\' + fileName;
    }

    std::wstring TrimPathSeparators(std::wstring path)
    {
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        {
            path.pop_back();
        }
        return path;
    }

    std::wstring TrimWhitespace(std::wstring value)
    {
        while (!value.empty() && (value.front() == L' ' || value.front() == L'\t'))
        {
            value.erase(value.begin());
        }
        while (!value.empty() && (value.back() == L' ' || value.back() == L'\t'))
        {
            value.pop_back();
        }
        return value;
    }

    std::wstring GetEnvironmentString(const wchar_t* name)
    {
        const DWORD length = ::GetEnvironmentVariableW(name, nullptr, 0u);
        if (length == 0u)
        {
            return std::wstring();
        }

        std::wstring value(static_cast<size_t>(length), L'\0');
        const DWORD copied = ::GetEnvironmentVariableW(name, &value[0], length);
        if (copied == 0u)
        {
            return std::wstring();
        }
        value.resize(static_cast<size_t>(copied));
        return value;
    }

    bool IsHexOfLength(const std::wstring& value, size_t length)
    {
        if (value.size() != length)
        {
            return false;
        }
        for (const wchar_t ch : value)
        {
            const bool digit = ch >= L'0' && ch <= L'9';
            const bool lower = ch >= L'a' && ch <= L'f';
            const bool upper = ch >= L'A' && ch <= L'F';
            if (!digit && !lower && !upper)
            {
                return false;
            }
        }
        return true;
    }

    // 哈希统一按大写比较：日志里的十六进制来自 BytesToHexStringW，解包目录名是文件系统给的，
    // 两边大小写不保证一致，不归一化就会出现"明明有配对却算 0"。
    std::wstring ToUpperAscii(const std::wstring& value)
    {
        std::wstring upper = value;
        for (wchar_t& ch : upper)
        {
            if (ch >= L'a' && ch <= L'z')
            {
                ch = static_cast<wchar_t>(ch - L'a' + L'A');
            }
        }
        return upper;
    }

    std::wstring FormatDuration(unsigned long long milliseconds)
    {
        const unsigned long long totalSeconds = milliseconds / 1000ull;
        return FormatString(L"%02llu:%02llu:%02llu",
                            totalSeconds / 3600ull,
                            (totalSeconds / 60ull) % 60ull,
                            totalSeconds % 60ull);
    }

    HFONT CreateUiFont(HWND window)
    {
        HDC dc = ::GetDC(window);
        const int dpiY = dc ? ::GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc)
        {
            ::ReleaseDC(window, dc);
        }

        return ::CreateFontW(-::MulDiv(9, dpiY, 72),
                             0,
                             0,
                             0,
                             FW_NORMAL,
                             FALSE,
                             FALSE,
                             FALSE,
                             DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE,
                             L"Microsoft YaHei UI");
    }

    BOOL CALLBACK ApplyFontToChild(HWND child, LPARAM font)
    {
        ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
        return TRUE;
    }

    void ApplyUiFont(CollectContext* context);

    HWND CreateChildControl(const wchar_t* className,
                            const wchar_t* text,
                            DWORD style,
                            int x,
                            int y,
                            int width,
                            int height,
                            HWND parent,
                            int identifier,
                            DWORD extendedStyle = 0u)
    {
        return ::CreateWindowExW(extendedStyle,
                                 className,
                                 text,
                                 style,
                                 x,
                                 y,
                                 width,
                                 height,
                                 parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
                                 nullptr,
                                 nullptr);
    }

    //**********映射日志尾部读取**********//

    // 一份映射日志的读取进度。
    // 文件是 UTF-16LE + BOM，每条配对一行 \r\n；HashCore 每写一行都 fflush，
    // 而且是 _SH_DENYWR 打开的（允许他人读），所以按字节偏移续读就能拿到新增行。
    struct LogTailState
    {
        std::wstring Path;
        unsigned long long Offset = 0;
        std::string Pending;            // 还没凑齐一行的字节（可能停在半个 UTF-16 码元上）
        bool ByteOrderChecked = false;  // BOM 只判断一次
        bool FirstScanDone = false;
    };

    std::wstring DecodeUtf16Le(const std::string& bytes)
    {
        if (bytes.size() < 2u)
        {
            return std::wstring();
        }
        std::wstring text(bytes.size() / 2u, L'\0');
        memcpy(&text[0], bytes.data(), text.size() * sizeof(wchar_t));
        return text;
    }

    // 从映射行里取哈希。行格式固定为「名字##YSig##哈希」。
    std::wstring ExtractHashFromPair(const std::wstring& line)
    {
        const size_t separatorLength = sizeof(PairSeparator) / sizeof(PairSeparator[0]) - 1u;
        const size_t position = line.find(PairSeparator);
        if (position == std::wstring::npos)
        {
            return std::wstring();
        }
        // 取交集前先大写归一化：日志里的十六进制大小写跟解包目录名不保证一致，
        // 不归一化就会出现"页签里明明有这行、进度却不涨"。（显示用的还是原始行。）
        return ToUpperAscii(TrimWhitespace(line.substr(position + separatorLength)));
    }

    void ReadNewLines(LogTailState& state, std::vector<std::wstring>& lines)
    {
        HANDLE file = ::CreateFileW(state.Path.c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size))
        {
            ::CloseHandle(file);
            return;
        }

        const unsigned long long fileSize = static_cast<unsigned long long>(size.QuadPart);
        // 文件被清空或重建（「清空目录文件」、用户手删）时旧偏移量作废，从头读。
        if (fileSize < state.Offset)
        {
            state.Offset = 0;
            state.Pending.clear();
            state.ByteOrderChecked = false;
            state.FirstScanDone = false;
        }

        const unsigned long long available = fileSize - state.Offset;
        const unsigned long long request = available > MaxBytesPerPump ? MaxBytesPerPump : available;
        if (request > 0ull)
        {
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(state.Offset);
            std::string chunk(static_cast<size_t>(request), '\0');
            if (::SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
            {
                DWORD read = 0u;
                if (::ReadFile(file, &chunk[0], static_cast<DWORD>(chunk.size()), &read, nullptr) && read > 0u)
                {
                    chunk.resize(static_cast<size_t>(read));
                    state.Offset += static_cast<unsigned long long>(read);
                    state.Pending.append(chunk);
                }
            }
        }
        ::CloseHandle(file);

        // BOM 只在文件开头出现一次。续读时 Pending 可能为空，所以用"检查过没有"标记，
        // 不能拿 Pending 是否为空当条件。
        if (!state.ByteOrderChecked && state.Pending.size() >= 2u)
        {
            state.ByteOrderChecked = true;
            const unsigned char first = static_cast<unsigned char>(state.Pending[0]);
            const unsigned char second = static_cast<unsigned char>(state.Pending[1]);
            if (first == 0xFFu && second == 0xFEu)
            {
                state.Pending.erase(0, 2u);
            }
        }

        // 行尾按 UTF-16LE 的「0A 00」找，且只从偶数偏移开始比对：
        // 逐字节找 '\n' 会把 U+0A00 这类字符误判成换行。
        size_t consumed = 0u;
        for (size_t index = 0u; index + 1u < state.Pending.size(); index += 2u)
        {
            if (state.Pending[index] != '\n' || state.Pending[index + 1u] != '\0')
            {
                continue;
            }

            std::string raw = state.Pending.substr(consumed, index - consumed);
            if (raw.size() >= 2u && raw[raw.size() - 2u] == '\r' && raw[raw.size() - 1u] == '\0')
            {
                raw.resize(raw.size() - 2u);
            }
            lines.push_back(DecodeUtf16Le(raw));
            consumed = index + 2u;
        }
        if (consumed > 0u)
        {
            state.Pending.erase(0, consumed);
        }
    }

    //**********窗口状态**********//

    struct CollectContext
    {
        std::wstring GameExecutablePath;
        std::wstring GameDirectory;
        std::wstring TargetDirectory;   // 纯Hash目录：进度分母来源
        std::wstring MappingDirectory;  // 映射产物目录：DirectoryHash.log / FileNameHash.log
        ULONGLONG StartTick = 0;

        // 分母（目标目录扫描结果）。扫描线程写、UI 线程读，用锁护住。
        CRITICAL_SECTION HashSetLock{};
        std::unordered_set<std::wstring> TargetDirectoryHashes;
        std::unordered_set<std::wstring> TargetFileHashes;
        volatile LONG ScanState = 0;  // 0=未开始 1=扫描中 2=完成
        bool ScanInline = false;      // 扫描线程开不出来时，退化成 UI 线程首帧里同步扫
        volatile LONG RefCount = 0;   // UI 线程 / 扫描线程 / Start 各占一份

        // 下面这些只由 UI 线程访问
        std::unordered_set<std::wstring> KnownDirectoryHashes;
        std::unordered_set<std::wstring> KnownFileHashes;
        bool DenominatorReady = false;
        LogTailState DirectoryTail;
        LogTailState FileNameTail;
        std::unordered_set<std::wstring> MatchedDirectoryHashes;  // 命中目标集合的目录哈希
        std::unordered_set<std::wstring> MatchedFileHashes;       // 命中目标集合的文件哈希

        HWND Window = nullptr;
        HFONT UiFont = nullptr;
        HWND StateText = nullptr;
        HWND UptimeText = nullptr;
        HWND DirectoryBar = nullptr;
        HWND DirectoryBarText = nullptr;
        HWND FileNameBar = nullptr;
        HWND FileNameBarText = nullptr;
        HWND RestoreRateText = nullptr;
        HWND LogTab = nullptr;
        HWND FileNameLogEdit = nullptr;
        HWND DirectoryLogEdit = nullptr;
        HWND StatusLine = nullptr;
    };

    void ApplyUiFont(CollectContext* context)
    {
        if (context == nullptr || context->Window == nullptr || context->UiFont == nullptr)
        {
            return;
        }
        ::SendMessageW(context->Window, WM_SETFONT, reinterpret_cast<WPARAM>(context->UiFont), TRUE);
        ::EnumChildWindows(context->Window, ApplyFontToChild, reinterpret_cast<LPARAM>(context->UiFont));
    }

    void ShowStatus(CollectContext* context, const std::wstring& text)
    {
        if (context != nullptr && context->StatusLine != nullptr)
        {
            ::SetWindowTextW(context->StatusLine, text.c_str());
        }
    }

    //**********分母：扫纯Hash目录**********//

    // 扫一遍纯Hash目录，得到去重的 16 位目录哈希数 D 与 64 位文件哈希数 F。
    // 目录名/文件名本身就是哈希（<包名>\<目录Hash16>\<文件Hash64>），不用读文件内容。
    void ScanTargetDirectory(CollectContext* context)
    {
        std::unordered_set<std::wstring> directoryHashes;
        std::unordered_set<std::wstring> fileHashes;

        std::vector<std::wstring> pending;
        pending.push_back(context->TargetDirectory);
        while (!pending.empty())
        {
            const std::wstring current = pending.back();
            pending.pop_back();

            WIN32_FIND_DATAW data{};
            HANDLE find = ::FindFirstFileW(CombinePath(current, L"*").c_str(), &data);
            if (find == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            do
            {
                const std::wstring name = data.cFileName;
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    if (name == L"." || name == L"..")
                    {
                        continue;
                    }
                    // 不跟交接点/符号链接：跟着走可能绕回自己，把扫描变成死循环。
                    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                    {
                        continue;
                    }
                    if (IsHexOfLength(name, 16u))
                    {
                        directoryHashes.insert(ToUpperAscii(name));
                    }
                    pending.push_back(CombinePath(current, name));
                }
                else if (IsHexOfLength(name, 64u))
                {
                    fileHashes.insert(ToUpperAscii(name));
                }
            } while (::FindNextFileW(find, &data) != FALSE);
            ::FindClose(find);
        }

        ::EnterCriticalSection(&context->HashSetLock);
        context->TargetDirectoryHashes.swap(directoryHashes);
        context->TargetFileHashes.swap(fileHashes);
        ::InterlockedExchange(&context->ScanState, 2);
        ::LeaveCriticalSection(&context->HashSetLock);
    }

    void ReleaseContext(CollectContext* context)
    {
        if (context == nullptr)
        {
            return;
        }
        // 窗口线程和扫描线程各持一份引用：窗口先关、扫描还在跑时不能把 context 删掉。
        if (::InterlockedDecrement(&context->RefCount) == 0)
        {
            ::DeleteCriticalSection(&context->HashSetLock);
            delete context;
        }
    }

    DWORD WINAPI TargetScanThreadProc(LPVOID parameter)
    {
        CollectContext* context = static_cast<CollectContext*>(parameter);
        ScanTargetDirectory(context);
        ReleaseContext(context);
        return 0u;
    }

    //**********分子：日志尾部读取 + 取交集**********//

    void TrimLogText(HWND edit)
    {
        const LRESULT lineCount = ::SendMessageW(edit, EM_GETLINECOUNT, 0, 0);
        const LRESULT textLength = ::SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0);
        const bool tooManyLines = lineCount > static_cast<LRESULT>(MaxLogLinesShown + TrimSlackLines);
        const bool tooManyChars = textLength > static_cast<LRESULT>(MaxLogCharsShown + TrimSlackChars);
        if (!tooManyLines && !tooManyChars)
        {
            return;
        }

        LRESULT keepFrom = 0;
        if (tooManyLines)
        {
            keepFrom = ::SendMessageW(edit,
                                      EM_LINEINDEX,
                                      static_cast<WPARAM>(lineCount - MaxLogLinesShown),
                                      0);
        }
        // 超长行（一条映射行上千字符）会让"留 1000 行"依然很大，再按字符数砍一刀。
        if (tooManyChars)
        {
            const LRESULT keepLines = static_cast<LRESULT>(MaxLogLinesShown / 2);
            const LRESULT byChars = ::SendMessageW(edit,
                                                   EM_LINEINDEX,
                                                   static_cast<WPARAM>(lineCount > keepLines ? lineCount - keepLines : 1),
                                                   0);
            if (byChars > keepFrom)
            {
                keepFrom = byChars;
            }
        }
        if (keepFrom <= 0)
        {
            return;
        }
        ::SendMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(keepFrom));
        ::SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }

    void AppendLogText(HWND edit, const std::wstring& text)
    {
        if (edit == nullptr || text.empty())
        {
            return;
        }

        const LRESULT length = ::SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0);
        const WPARAM end = length > 0 ? static_cast<WPARAM>(length) : static_cast<WPARAM>(0);
        ::SendMessageW(edit, EM_SETSEL, end, static_cast<LPARAM>(end));
        // 只读编辑框照样接受 EM_REPLACESEL —— ES_READONLY 挡的是用户输入，不是程序写入。
        ::SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
        // 追加后光标停在末尾，ES_AUTOVSCROLL 会把视图带着往下滚，也就是"实时滚动"。
        TrimLogText(edit);
    }

    void PumpOneLog(LogTailState& tail,
                    const std::unordered_set<std::wstring>& knownHashes,
                    std::unordered_set<std::wstring>& matchedHashes,
                    HWND edit)
    {
        std::vector<std::wstring> lines;
        ReadNewLines(tail, lines);
        if (lines.empty())
        {
            return;
        }

        // 分子：只算哈希落在纯Hash目录集合里的配对（取交集），
        // "游戏加载过但产物里没有"的条目不该算进还原率。
        // 用集合去重：同一条配对可能跨会话重复出现，重复计数会把进度顶到 100% 以上。
        for (const std::wstring& line : lines)
        {
            const std::wstring hash = ExtractHashFromPair(line);
            if (!hash.empty() && knownHashes.find(hash) != knownHashes.end())
            {
                matchedHashes.insert(hash);
            }
        }

        // 首次读取只把末尾若干行垫进页签（映射库可能已经有几万行，全灌进去又慢又没用）；
        // 之后每次刷新进来的就是"新增行"，逐行滚动。
        const bool firstScan = !tail.FirstScanDone;
        tail.FirstScanDone = true;
        size_t begin = lines.size() > static_cast<size_t>(MaxLogLinesShown)
                           ? lines.size() - static_cast<size_t>(MaxLogLinesShown)
                           : 0u;
        if (firstScan && lines.size() > FirstScanLinesShown)
        {
            begin = lines.size() - FirstScanLinesShown;
        }

        // 从末尾往前装到字符上限为止：页签要的是"最新的那几行"。一批读进来几万行
        // （首帧能读到 4 MiB）时，整批塞给 EDIT 会撞上限被整条拒绝，反而什么都不显示。
        size_t firstShown = lines.size();
        size_t blockChars = 0u;
        while (firstShown > begin)
        {
            const size_t cost = lines[firstShown - 1u].size() + 2u;  // + "\r\n"
            if (blockChars + cost > static_cast<size_t>(MaxLogCharsShown))
            {
                break;
            }
            blockChars += cost;
            --firstShown;
        }

        std::wstring block;
        block.reserve(blockChars);
        for (size_t index = firstShown; index < lines.size(); ++index)
        {
            block += lines[index];
            block += L"\r\n";
        }
        AppendLogText(edit, block);
    }

    //**********进度显示**********//

    void SetProgressValue(HWND bar, HWND text, size_t done, size_t total)
    {
        const unsigned int percent = total == 0u
                                         ? 0u
                                         : static_cast<unsigned int>((static_cast<unsigned long long>(done) * 100ull) /
                                                                     static_cast<unsigned long long>(total));
        ::SendMessageW(bar, PBM_SETPOS, static_cast<WPARAM>(percent), 0);
        ::SetWindowTextW(text,
                         FormatString(L"%llu / %llu  %u%%",
                                      static_cast<unsigned long long>(done),
                                      static_cast<unsigned long long>(total),
                                      percent)
                             .c_str());
    }

    void UpdateProgressTexts(CollectContext* context)
    {
        const size_t directoryTotal = context->KnownDirectoryHashes.size();
        const size_t fileTotal = context->KnownFileHashes.size();
        const size_t fileDone = context->MatchedFileHashes.size();

        SetProgressValue(context->DirectoryBar,
                         context->DirectoryBarText,
                         context->MatchedDirectoryHashes.size(),
                         directoryTotal);
        SetProgressValue(context->FileNameBar, context->FileNameBarText, fileDone, fileTotal);

        // 资源还原率 = 已收集文件配对数 / 文件目标总数（分母 F 是去重的 64 位文件哈希数）
        const double rate = fileTotal == 0u
                                ? 0.0
                                : static_cast<double>(fileDone) * 100.0 / static_cast<double>(fileTotal);
        ::SetWindowTextW(context->RestoreRateText,
                         FormatString(L"资源还原率：%.1f%% (%llu / %llu)",
                                      rate,
                                      static_cast<unsigned long long>(fileDone),
                                      static_cast<unsigned long long>(fileTotal))
                             .c_str());
    }

    void RefreshConsole(CollectContext* context)
    {
        if (context == nullptr || context->Window == nullptr)
        {
            return;
        }

        ::SetWindowTextW(context->UptimeText,
                         FormatDuration(::GetTickCount64() - context->StartTick).c_str());

        // 分母必须先就绪：目标集合还没扫完就开始数分子，先读到的那几万行会被整片判成
        // "不在目标里"，进度会永久偏低。所以扫描没完成前干脆不读日志（偏移量原地不动，
        // 一行都不会漏）。
        if (!context->DenominatorReady)
        {
            if (context->ScanInline && ::InterlockedCompareExchange(&context->ScanState, 1, 0) == 0)
            {
                ScanTargetDirectory(context);  // 扫描线程没开出来时的退路
            }

            if (::InterlockedCompareExchange(&context->ScanState, 2, 2) == 2)
            {
                ::EnterCriticalSection(&context->HashSetLock);
                context->KnownDirectoryHashes = context->TargetDirectoryHashes;
                context->KnownFileHashes = context->TargetFileHashes;
                ::LeaveCriticalSection(&context->HashSetLock);
                context->DenominatorReady = true;
                ShowStatus(context,
                           FormatString(L"目标目录扫描完成：目录哈希 %llu 个 / 文件哈希 %llu 个（进度分母）。",
                                        static_cast<unsigned long long>(context->KnownDirectoryHashes.size()),
                                        static_cast<unsigned long long>(context->KnownFileHashes.size())));
            }
            else
            {
                ShowStatus(context, L"正在扫描纯Hash目标目录（进度分母）…");
            }
        }

        if (context->DenominatorReady)
        {
            PumpOneLog(context->FileNameTail, context->KnownFileHashes, context->MatchedFileHashes, context->FileNameLogEdit);
            PumpOneLog(context->DirectoryTail, context->KnownDirectoryHashes, context->MatchedDirectoryHashes, context->DirectoryLogEdit);
        }

        UpdateProgressTexts(context);
    }

    void ShowActiveLogTab(CollectContext* context)
    {
        const LRESULT selected = ::SendMessageW(context->LogTab, TCM_GETCURSEL, 0, 0);
        const bool showFileName = selected != 1;  // 0 = FileNameHash（默认），1 = DirectoryHash
        ::ShowWindow(context->FileNameLogEdit, showFileName ? SW_SHOW : SW_HIDE);
        ::ShowWindow(context->DirectoryLogEdit, showFileName ? SW_HIDE : SW_SHOW);
    }

    //**********底部按钮**********//

    bool ConfirmDestructive(HWND owner, const std::wstring& message, const wchar_t* title)
    {
        // 默认按钮落在「否」：销毁性操作的默认答案必须是"什么都不做"。
        return ::MessageBoxW(owner, message.c_str(), title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    }

    void ClearMappingDirectory(CollectContext* context)
    {
        const std::wstring question = L"确定要清空工作区（映射产物目录）里的文件吗？\n\n" +
                                      context->MappingDirectory +
                                      L"\n\n正在收集写入的映射日志会被跳过（关掉游戏再清空即可）。";
        if (!ConfirmDestructive(context->Window, question, L"清空目录文件"))
        {
            ShowStatus(context, L"已取消：没有删除任何文件。");
            return;
        }

        unsigned int deleted = 0u;
        unsigned int skipped = 0u;
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(CombinePath(context->MappingDirectory, L"*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                // 只删这一层的文件，不递归、不动子目录：工作区里放的是映射产物，
                // 递归删除会把用户顺手放进去的东西一起带走。
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    continue;
                }
                if (::DeleteFileW(CombinePath(context->MappingDirectory, data.cFileName).c_str()) != FALSE)
                {
                    ++deleted;
                }
                else
                {
                    ++skipped;
                }
            } while (::FindNextFileW(find, &data) != FALSE);
            ::FindClose(find);
        }

        ShowStatus(context,
                   FormatString(L"清空完成：删除 %u 个文件，%u 个正在使用中跳过。", deleted, skipped));
    }

    BOOL CALLBACK CloseGameWindowEnumerator(HWND window, LPARAM parameter)
    {
        UNREFERENCED_PARAMETER(parameter);

        DWORD processId = 0u;
        ::GetWindowThreadProcessId(window, &processId);
        if (processId != ::GetCurrentProcessId() || ::IsWindowVisible(window) == FALSE)
        {
            return TRUE;
        }

        // 本控制台也在同一个进程里，别把它自己关了 —— 那样用户就看不到"正在退出游戏"。
        wchar_t className[128]{};
        ::GetClassNameW(window, className, _countof(className));
        if (::lstrcmpW(className, ConsoleWindowClassName) == 0)
        {
            return TRUE;
        }

        ::PostMessageW(window, WM_CLOSE, 0u, 0u);
        return TRUE;
    }

    DWORD WINAPI CloseGameWatchdogProc(LPVOID parameter)
    {
        UNREFERENCED_PARAMETER(parameter);
        // 游戏不理会 WM_CLOSE（卡在加载、或自己吞了消息）时，用户按了按钮总得有个结果：
        // 5 秒还没退就强杀。映射行是每条 fflush 的，强杀不会丢已收集的配对。
        ::Sleep(5000u);
        ::TerminateProcess(::GetCurrentProcess(), 0u);
        return 0u;
    }

    void CloseGameProcess(CollectContext* context)
    {
        if (!ConfirmDestructive(context->Window,
                                L"确定要关闭游戏进程吗？\n\n已收集的配对都写在映射日志里，关闭不会丢失。",
                                L"关闭游戏"))
        {
            ShowStatus(context, L"已取消：没有关闭游戏。");
            return;
        }

        if (context->StateText != nullptr)
        {
            ::SetWindowTextW(context->StateText, L"已结束（正在退出游戏）");
        }
        ::EnumWindows(CloseGameWindowEnumerator, 0);

        HANDLE thread = ::CreateThread(nullptr, 0u, CloseGameWatchdogProc, nullptr, 0u, nullptr);
        if (thread)
        {
            ::CloseHandle(thread);
        }
        ShowStatus(context, L"已请求关闭游戏；5 秒内没退出会强制结束进程。");
    }

    void OpenOutputDirectory(CollectContext* context)
    {
        Directory::Create(context->MappingDirectory);
        const HINSTANCE result = ::ShellExecuteW(context->Window,
                                                 L"open",
                                                 context->MappingDirectory.c_str(),
                                                 nullptr,
                                                 nullptr,
                                                 SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            ShowStatus(context,
                       FormatString(L"打开输出目录失败（ShellExecute 返回 %lld）。",
                                    static_cast<long long>(reinterpret_cast<INT_PTR>(result)))
                           .c_str());
            return;
        }
        ShowStatus(context, L"已在资源管理器中打开输出目录。");
    }

    void ReturnToMainWindow(CollectContext* context)
    {
        // 本窗口住在游戏进程里，"主界面"是另一个进程（YuaExtract.exe）的窗口。
        // 找不到就老实说一声并保持显示 —— 藏起来会让用户再也找不回进度。
        HWND mainWindow = ::FindWindowW(L"YuaExtractMainWindow", nullptr);
        if (mainWindow == nullptr)
        {
            ShowStatus(context, L"没找到主界面窗口（YuaExtract.exe 可能已关闭），控制台保持显示。");
            return;
        }

        ::ShowWindow(mainWindow, SW_RESTORE);
        ::SetForegroundWindow(mainWindow);
        // 用最小化而不是隐藏：任务栏上还留着一个入口，用户能再点回来。
        ::ShowWindow(context->Window, SW_MINIMIZE);
        ::SetWindowTextW(context->StateText, L"运行中（控制台已最小化）");
        ShowStatus(context, L"已返回主界面；收集在后台继续，控制台最小化在任务栏。");
    }

    //**********窗口**********//

    LRESULT CALLBACK ConsoleWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        CollectContext* context = reinterpret_cast<CollectContext*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message)
        {
            case WM_CREATE:
            {
                CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                context = static_cast<CollectContext*>(create->lpCreateParams);
                context->Window = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
                context->UiFont = CreateUiFont(hwnd);

                // 状态信息
                CreateChildControl(L"BUTTON", L"状态信息", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 6, 828, 104, hwnd, 0);
                CreateChildControl(L"STATIC", L"游戏：", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 26, 50, 18, hwnd, 0);
                CreateChildControl(L"STATIC", context->GameExecutablePath.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 76, 26, 754, 18, hwnd, 0);
                CreateChildControl(L"STATIC", L"工作区：", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 46, 60, 18, hwnd, 0);
                CreateChildControl(L"STATIC", context->MappingDirectory.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 86, 46, 744, 18, hwnd, 0);
                CreateChildControl(L"STATIC", L"状态：", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 66, 44, 18, hwnd, 0);
                context->StateText = CreateChildControl(L"STATIC", L"运行中", WS_CHILD | WS_VISIBLE | SS_LEFT, 70, 66, 130, 18, hwnd, 0);
                CreateChildControl(L"STATIC", L"PID：", WS_CHILD | WS_VISIBLE | SS_LEFT, 216, 66, 38, 18, hwnd, 0);
                CreateChildControl(L"STATIC", FormatString(L"%lu", ::GetCurrentProcessId()).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 256, 66, 90, 18, hwnd, 0);
                CreateChildControl(L"STATIC", L"运行时间：", WS_CHILD | WS_VISIBLE | SS_LEFT, 362, 66, 72, 18, hwnd, 0);
                context->UptimeText = CreateChildControl(L"STATIC", L"00:00:00", WS_CHILD | WS_VISIBLE | SS_LEFT, 436, 66, 130, 18, hwnd, 0);
                CreateChildControl(L"STATIC", L"分母目录（纯Hash）：", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 86, 150, 18, hwnd, 0);
                CreateChildControl(L"STATIC", context->TargetDirectory.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 178, 86, 652, 18, hwnd, 0);

                // 收集进度
                CreateChildControl(L"BUTTON", L"收集进度", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 116, 828, 96, hwnd, 0);
                CreateChildControl(L"STATIC", L"DirectoryHash", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 138, 88, 18, hwnd, 0);
                context->DirectoryBar = CreateChildControl(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 116, 136, 470, 20, hwnd, IdcDirectoryProgressBar);
                context->DirectoryBarText = CreateChildControl(L"STATIC", L"0 / 0  0%", WS_CHILD | WS_VISIBLE | SS_LEFT, 596, 138, 230, 18, hwnd, IdcDirectoryProgressText);
                CreateChildControl(L"STATIC", L"FileNameHash", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 166, 88, 18, hwnd, 0);
                context->FileNameBar = CreateChildControl(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 116, 164, 470, 20, hwnd, IdcFileNameProgressBar);
                context->FileNameBarText = CreateChildControl(L"STATIC", L"0 / 0  0%", WS_CHILD | WS_VISIBLE | SS_LEFT, 596, 166, 230, 18, hwnd, IdcFileNameProgressText);
                context->RestoreRateText = CreateChildControl(L"STATIC", L"资源还原率：0.0% (0 / 0)", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 190, 500, 18, hwnd, IdcRestoreRateText);
                ::SendMessageW(context->DirectoryBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
                ::SendMessageW(context->FileNameBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

                // 实时日志：两个页签，各自一个只读编辑框铺在页签的显示区上
                CreateChildControl(L"BUTTON", L"实时日志", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, LogGroupY, 828, LogGroupHeight, hwnd, 0);
                context->LogTab = CreateChildControl(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | TCS_TABS, LogTabX, LogTabY, LogTabWidth, LogTabHeight, hwnd, IdcLogTab);
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = const_cast<wchar_t*>(L"FileNameHash 实时日志");
                ::SendMessageW(context->LogTab, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
                item.pszText = const_cast<wchar_t*>(L"DirectoryHash 实时日志");
                ::SendMessageW(context->LogTab, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&item));
                ::SendMessageW(context->LogTab, TCM_SETCURSEL, 0, 0);

                // 页签显示区 = 页签按钮行下沿再往里缩一点；编辑框就铺在这块区域上
                RECT tabItem{};
                ::SendMessageW(context->LogTab, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&tabItem));
                const int logEditX = LogTabX + 6;
                const int logEditY = LogTabY + tabItem.bottom + 3;
                const int logEditWidth = LogTabWidth - 12;
                const int logEditHeight = (LogTabY + LogTabHeight) - logEditY - 4;

                // 不换行 + 横竖滚动条：一行就是一条配对，号对号，比自动换行好认。
                const DWORD logEditStyle = WS_CHILD | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                           ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_LEFT;
                context->FileNameLogEdit = CreateChildControl(L"EDIT", L"", logEditStyle | WS_VISIBLE, logEditX, logEditY, logEditWidth, logEditHeight, hwnd, IdcFileNameLogEdit, WS_EX_CLIENTEDGE);
                context->DirectoryLogEdit = CreateChildControl(L"EDIT", L"", logEditStyle, logEditX, logEditY, logEditWidth, logEditHeight, hwnd, IdcDirectoryLogEdit, WS_EX_CLIENTEDGE);
                // EDIT 默认只让放 30000 个字符，撞上限后追加会被静默拒绝（页签冻住）；
                // 这里抬高上限，真正的内容边界交给 TrimLogText 的行数/字符数截断。
                ::SendMessageW(context->FileNameLogEdit, EM_SETLIMITTEXT, static_cast<WPARAM>(LogEditCharLimit), 0);
                ::SendMessageW(context->DirectoryLogEdit, EM_SETLIMITTEXT, static_cast<WPARAM>(LogEditCharLimit), 0);
                ShowActiveLogTab(context);

                // 底部按钮
                CreateChildControl(L"BUTTON", L"启动游戏", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON, 16, 540, 152, 30, hwnd, IdcStartGameButton);
                CreateChildControl(L"BUTTON", L"关闭游戏", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 176, 540, 152, 30, hwnd, IdcCloseGameButton);
                CreateChildControl(L"BUTTON", L"清空目录文件", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 336, 540, 152, 30, hwnd, IdcClearDirectoryButton);
                CreateChildControl(L"BUTTON", L"打开输出目录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 496, 540, 152, 30, hwnd, IdcOpenOutputButton);
                CreateChildControl(L"BUTTON", L"返回主界面", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 656, 540, 152, 30, hwnd, IdcBackToMainButton);

                context->StatusLine = CreateChildControl(L"STATIC", L"就绪：等待游戏触发 Hash 计算…", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 576, 816, 20, hwnd, IdcStatusLine);
                CreateChildControl(L"STATIC", L"提示：游戏由主界面启动，控制台不重复启动；本窗口只收集配对，不改动任何文件名。", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 598, 816, 20, hwnd, 0);

                ApplyUiFont(context);
                ::SetTimer(hwnd, RefreshTimerId, RefreshIntervalMs, nullptr);
                RefreshConsole(context);
                return 0;
            }
            case WM_COMMAND:
            {
                if (context == nullptr)
                {
                    break;
                }
                switch (LOWORD(wParam))
                {
                    case IdcStartGameButton:
                        // 按钮是禁用的，正常点不到；留这条分支是为了将来真要支持"从控制台启动"时
                        // 不会变成一次静默的无效点击。
                        ShowStatus(context, L"游戏由主界面启动，控制台里不重复启动。");
                        return 0;
                    case IdcCloseGameButton:
                        CloseGameProcess(context);
                        return 0;
                    case IdcClearDirectoryButton:
                        ClearMappingDirectory(context);
                        return 0;
                    case IdcOpenOutputButton:
                        OpenOutputDirectory(context);
                        return 0;
                    case IdcBackToMainButton:
                        ReturnToMainWindow(context);
                        return 0;
                    default:
                        break;
                }
                break;
            }
            case WM_NOTIFY:
            {
                NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
                if (context != nullptr && header != nullptr && header->hwndFrom == context->LogTab &&
                    header->code == TCN_SELCHANGE)
                {
                    ShowActiveLogTab(context);
                    return 0;
                }
                break;
            }
            case WM_TIMER:
                if (context != nullptr && wParam == RefreshTimerId)
                {
                    RefreshConsole(context);
                    return 0;
                }
                break;
            case WM_CLOSE:
                // 只关窗口，不停收集：HashCore 的挂钩和映射日志写入跟这个窗口没关系，
                // 关掉它照样继续追加配对。用户想停下来就关游戏。
                ::DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                ::KillTimer(hwnd, RefreshTimerId);
                if (context != nullptr)
                {
                    if (context->UiFont != nullptr)
                    {
                        ::DeleteObject(context->UiFont);
                        context->UiFont = nullptr;
                    }
                    // 先把 USERDATA 清掉再放引用：销毁后万一还有排队的消息进来，
                    // 窗口过程取到的是空 context，不会碰到已经释放的内存。
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    ReleaseContext(context);
                }
                ::PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    DWORD WINAPI ConsoleUiThreadProc(LPVOID parameter)
    {
        CollectContext* context = static_cast<CollectContext*>(parameter);

        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_PROGRESS_CLASS | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
        ::InitCommonControlsEx(&controls);

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = ConsoleWindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        windowClass.lpszClassName = ConsoleWindowClassName;
        ::RegisterClassExW(&windowClass);

        RECT desired{0, 0, ClientWidth, ClientHeight};
        const DWORD windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        ::AdjustWindowRect(&desired, windowStyle, FALSE);

        HWND hwnd = ::CreateWindowExW(0,
                                      ConsoleWindowClassName,
                                      ConsoleWindowTitle,
                                      windowStyle,
                                      CW_USEDEFAULT,
                                      CW_USEDEFAULT,
                                      desired.right - desired.left,
                                      desired.bottom - desired.top,
                                      nullptr,
                                      nullptr,
                                      windowClass.hInstance,
                                      context);
        if (hwnd == nullptr)
        {
            ReleaseContext(context);
            return 0u;
        }

        ::ShowWindow(hwnd, SW_SHOW);
        ::UpdateWindow(hwnd);

        MSG message{};
        while (::GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        return 0u;
    }
}

namespace Engine
{
    bool CollectionConsoleUI::IsCollectOnlyMode()
    {
        const std::wstring value = TrimWhitespace(GetEnvironmentString(CollectOnlyEnvName));
        // 契约值是 "1"；顺带认 true/yes，只是不想让用户因为写了 "true" 就以为开关坏了。
        return value == L"1" ||
               _wcsicmp(value.c_str(), L"true") == 0 ||
               _wcsicmp(value.c_str(), L"yes") == 0;
    }

    void CollectionConsoleUI::Start(HMODULE moduleHandle, const std::wstring& gameDirectory)
    {
        UNREFERENCED_PARAMETER(moduleHandle);

        CollectContext* context = new CollectContext{};
        context->GameDirectory = TrimPathSeparators(gameDirectory);
        context->GameExecutablePath = Util::GetModulePathW(::GetModuleHandleW(nullptr));
        context->MappingDirectory = CombinePath(context->GameDirectory, MappingFolderName);
        context->DirectoryTail.Path = CombinePath(context->MappingDirectory, DirectoryHashLogName);
        context->FileNameTail.Path = CombinePath(context->MappingDirectory, FileNameHashLogName);

        std::wstring target = TrimPathSeparators(GetEnvironmentString(TargetDirectoryEnvName));
        if (target.empty())
        {
            // Loader 没给 CXDEC_RUNTIME_HASH_TARGET_DIR 时的契约回退：游戏目录下的 Extractor_Output。
            target = CombinePath(context->GameDirectory, L"Extractor_Output");
        }
        context->TargetDirectory = target;
        context->StartTick = ::GetTickCount64();
        ::InitializeCriticalSection(&context->HashSetLock);

        // Start 自己先持一份引用，两个线程都起来后再交回去 —— 否则线程短命时
        // Start 还在用的 context 可能已经被最后退出的那个线程删掉了。
        context->RefCount = 1;

        ::InterlockedIncrement(&context->RefCount);
        HANDLE uiThread = ::CreateThread(nullptr, 0u, ConsoleUiThreadProc, context, 0u, nullptr);
        if (uiThread == nullptr)
        {
            ::InterlockedDecrement(&context->RefCount);
            ReleaseContext(context);
            return;
        }
        ::CloseHandle(uiThread);

        // 分母扫描（纯Hash目录可能上万个子目录）放工作线程，别让窗口出现前先卡几秒。
        ::InterlockedIncrement(&context->RefCount);
        HANDLE scanThread = ::CreateThread(nullptr, 0u, TargetScanThreadProc, context, 0u, nullptr);
        if (scanThread != nullptr)
        {
            ::CloseHandle(scanThread);
        }
        else
        {
            ::InterlockedDecrement(&context->RefCount);
            context->ScanInline = true;  // 退路：UI 线程首帧里同步扫
        }

        ReleaseContext(context);
    }
}
