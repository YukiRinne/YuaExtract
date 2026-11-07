// 还原报告（RestoreReport.txt）统计的读取与解析。
//
// 独立成小头文件（全部 inline，无 .cpp）：解析逻辑要被两边共享 ——
//   · YuaWebViewBridge.inl 的 WorkspaceJson / NotifyRestore 往前端报数；
//   · _verify/restore-report/main.cpp 的纯逻辑验证（不依赖主工程，cl 一条命令就能编）。
//
// 口径必须与 HashNameRestorer.cpp 写报告的代码一致：
//   · 「哈希名文件」「  已还原」两行用 FormatCount，数字带半角逗号分组；
//   · 「还原成功率」行是 std::to_wstring(percent) + "%"，后面跟全角括号注释。
// 解析规则：每个标签取**第一行**「标签后面能读出整数」的值；整数允许逗号。
// 三个标签缺任何一个就整体算 absent，不许编造数字（尤其不许编 100）。

#ifndef YUA_RESTORE_REPORT_STATS_H
#define YUA_RESTORE_REPORT_STATS_H

#include <windows.h>

#include <cwchar>
#include <string>

namespace Yua
{
    // 读还原报告开头最多 64 KiB（统计三行都在文件头部，明细动辄几 MB，不整读）。
    // 文件是 UTF-8 BOM（WriteTextFileUtf8Bom 写出）：先剥 BOM，再按 UTF-8 解码。
    // 打不开 / 读失败 / 解码失败返回空串，由调用方决定怎么报（不许拿空串当 0% 成功）。
    inline std::wstring ReadRestoreReportHead(const std::wstring& path)
    {
        std::wstring text;
        if (path.empty())
        {
            return text;
        }
        const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return text;
        }
        char buffer[64 * 1024];
        DWORD bytesRead = 0;
        const BOOL ok = ::ReadFile(handle, buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr);
        ::CloseHandle(handle);
        if (!ok || bytesRead == 0u)
        {
            return text;
        }
        const char* begin = buffer;
        int size = static_cast<int>(bytesRead);
        if (size >= 3 && static_cast<unsigned char>(begin[0]) == 0xEFu &&
            static_cast<unsigned char>(begin[1]) == 0xBBu && static_cast<unsigned char>(begin[2]) == 0xBFu)
        {
            begin += 3;
            size -= 3;
        }
        // 64 KiB 可能截断在一个多字节序列中间：不带 MB_ERR_INVALID_CHARS，
        // 截断处按 U+FFFD 替换即可，前面的完整行不受影响。
        const int wideSize = ::MultiByteToWideChar(CP_UTF8, 0, begin, size, nullptr, 0);
        if (wideSize <= 0)
        {
            return text;
        }
        text.resize(static_cast<size_t>(wideSize));
        if (::MultiByteToWideChar(CP_UTF8, 0, begin, size, &text[0], wideSize) <= 0)
        {
            text.clear();
        }
        return text;
    }

    namespace RestoreReportDetail
    {
        inline bool IsBlank(wchar_t ch) { return ch == L' ' || ch == L'\t'; }

        // 从 position 开始跳过空白后读一个「允许逗号分组」的整数（FormatCount 的口径），
        // 成功时把 position 推进到数字之后（调用方要继续检查 % 就得从数字末尾接着走）。
        // 逗号只认「夹在数字中间」的（1,234）；读到非数字就停。溢出按 UINT_MAX 封顶。
        inline bool ReadGroupedNumber(const std::wstring& line, size_t& position, unsigned int& value)
        {
            while (position < line.size() && IsBlank(line[position]))
            {
                ++position;
            }
            size_t digits = 0;
            unsigned long long accumulated = 0;
            while (position < line.size())
            {
                const wchar_t ch = line[position];
                if (ch == L',' && digits > 0 && position + 1 < line.size() && line[position + 1] >= L'0' &&
                    line[position + 1] <= L'9')
                {
                    ++position;
                    continue;
                }
                if (ch < L'0' || ch > L'9')
                {
                    break;
                }
                ++digits;
                accumulated = accumulated * 10u + static_cast<unsigned long long>(ch - L'0');
                if (accumulated > 0xFFFFFFFFull)
                {
                    accumulated = 0xFFFFFFFFull;
                }
                ++position;
            }
            if (digits == 0u)
            {
                return false;
            }
            value = static_cast<unsigned int>(accumulated);
            return true;
        }

        // 取下一行（支持 \r\n / \n / 孤 \r）。
        inline bool NextLine(const std::wstring& text, size_t& position, std::wstring& line)
        {
            if (position >= text.size())
            {
                return false;
            }
            size_t end = text.find(L'\n', position);
            if (end == std::wstring::npos)
            {
                line = text.substr(position);
                position = text.size();
            }
            else
            {
                line = text.substr(position, end - position);
                position = end + 1;
            }
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }
            return true;
        }

        // 行首（允许前导空白）是否以 label 开头；是则返回 label 之后的位置。
        inline bool LineStartsWith(const std::wstring& line, const wchar_t* label, size_t& after)
        {
            const size_t labelLength = ::wcslen(label);
            size_t start = 0;
            while (start < line.size() && IsBlank(line[start]))
            {
                ++start;
            }
            if (line.compare(start, labelLength, label) != 0)
            {
                return false;
            }
            after = start + labelLength;
            return true;
        }

        // 逐行找第一个「label 开头且后面读得出整数」的行。表头行（已还原\t缺目录…）
        // 后面没有数字，天然落空，不会误命中。
        inline bool FindCount(const std::wstring& text, const wchar_t* label, unsigned int& value)
        {
            size_t position = 0;
            std::wstring line;
            while (NextLine(text, position, line))
            {
                size_t after = 0;
                if (LineStartsWith(line, label, after) && ReadGroupedNumber(line, after, value))
                {
                    return true;
                }
            }
            return false;
        }
    }

    // 解析统计三行。三个标签都读到才 present=true；否则一律 false + 三个 0。
    // 还原成功率取「% 前面的整数」，0..100 对外口径与报告正文一致。
    inline void ParseRestoreReportStats(const std::wstring& text, bool& present, unsigned int& restored,
                                        unsigned int& hashed, unsigned int& ratePercent)
    {
        using namespace RestoreReportDetail;
        present = false;
        restored = 0u;
        hashed = 0u;
        ratePercent = 0u;

        unsigned int hashedValue = 0u;
        unsigned int restoredValue = 0u;
        unsigned int rateValue = 0u;
        if (!FindCount(text, L"哈希名文件", hashedValue))
        {
            return;
        }
        if (!FindCount(text, L"已还原", restoredValue))
        {
            return;
        }
        // 成功率行：数字后面（跳过空白）必须是 '%' 才算命中，避免把括号注释里的
        // 「1000 / 1234」当成百分数。
        bool rateFound = false;
        {
            size_t position = 0;
            std::wstring line;
            while (NextLine(text, position, line))
            {
                size_t after = 0;
                unsigned int value = 0u;
                if (!LineStartsWith(line, L"还原成功率", after) || !ReadGroupedNumber(line, after, value))
                {
                    continue;
                }
                size_t cursor = after;
                while (cursor < line.size() && IsBlank(line[cursor]))
                {
                    ++cursor;
                }
                if (cursor < line.size() && line[cursor] == L'%')
                {
                    rateValue = value;
                    rateFound = true;
                    break;
                }
            }
        }
        if (!rateFound)
        {
            return;
        }
        present = true;
        hashed = hashedValue;
        restored = restoredValue;
        ratePercent = rateValue;
    }
}

#endif // YUA_RESTORE_REPORT_STATS_H
