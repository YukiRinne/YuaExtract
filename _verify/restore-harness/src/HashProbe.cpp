// HashProbe.exe —— 夹具哈希的"产品侧真值"（验证脚手架专用，不属于产品）。
//
// 为什么需要它：扩展集夹具里的 file/domain 哈希必须与产品**逐位一致**，
// 否则生成期自校验会大面积报例外，好实现看起来像没实现（README 踩坑 #14 的同类）。
// 这里不自己实现哈希，而是**直接编译并调用产品自己的 CxdecRepacker/core/resource_hash.cpp**，
// 于是"夹具哈希 == 产品哈希"这件事由编译器保证，而不是靠我抄公式。
//
// 用法（build-hashprobe.ps1 会编它）：
//   HashProbe.exe <spec.tsv> <out.tsv> <salt>
//   spec 每行（UTF-8，制表符分隔，# 注释）：KIND<TAB>输入
//     FILE  裸文件名           -> file_hash(名字, 盐)
//     DIR   已归一化目录路径    -> domain_hash(路径, 盐)
//     ROOT  （输入忽略）        -> domain_hash(u"", 盐)
//   out 每行：KIND<TAB>输入<TAB>大写 HEX

#include "core/resource_hash.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::u16string Utf8ToUtf16(const std::string& utf8)
{
    if (utf8.empty()) { return std::u16string(); }
    const int count = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) { return std::u16string(); }
    std::wstring wide(static_cast<size_t>(count), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], count);

    std::u16string out;
    out.reserve(wide.size());
    for (const wchar_t ch : wide) { out.push_back(static_cast<char16_t>(ch)); }
    return out;
}

std::string ToHex(const uint8_t* data, size_t length)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string text;
    text.reserve(length * 2);
    for (size_t index = 0; index < length; ++index)
    {
        text.push_back(kHex[(data[index] >> 4) & 0x0F]);
        text.push_back(kHex[data[index] & 0x0F]);
    }
    return text;
}

std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : line)
    {
        if (ch == '\t') { parts.push_back(current); current.clear(); }
        else { current.push_back(ch); }
    }
    parts.push_back(current);
    return parts;
}

std::string Trim(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\r' || text[begin] == '\n')) { ++begin; }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\r' || text[end - 1] == '\n')) { --end; }
    return text.substr(begin, end - begin);
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    // 诊断一律走窄字符 fprintf：宽字符 fwprintf 在非交互/重定向的 stderr 上实测什么都不输出，
    // 排查时看起来像"程序没跑"，白花时间。
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: HashProbe.exe <spec.tsv> <out.tsv> <salt>  (argc=%d)\n", argc);
        return 2;
    }

    // 盐只可能是 ASCII 实际值（xp3hnp 之类），转成 UTF-8 走同一条路
    char saltPath[MAX_PATH * 4] = {0};
    ::WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, saltPath, sizeof(saltPath) - 1, nullptr, nullptr);
    const std::string salt(saltPath);

    // 注意：必须用**宽字符**路径打开文件。本仓库路径含中文（`YuaToolkit插件`），
    // 窄字符 CRT 走的是 ANSI 代码页，UTF-8 路径会直接 fopen 失败（实测 "cannot open spec"）。
    std::ifstream input(argv[1], std::ios::binary);
    if (!input)
    {
        std::fprintf(stderr, "cannot open spec (argc=%d)\n", argc);
        return 2;
    }
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    if (!output)
    {
        std::fprintf(stderr, "cannot open out (argc=%d)\n", argc);
        return 2;
    }

    int written = 0;
    std::string line;
    while (std::getline(input, line))
    {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') { continue; }

        const std::vector<std::string> parts = SplitTabs(line);
        if (parts.size() < 2) { continue; }
        const std::string kind = Trim(parts[0]);
        // 必须 Trim：spec 是 CRLF 的文本，两列的行里 parts[1] 会带上行尾的 '\r'，
        // 于是哈希算的是 "base.png\r" —— 与夹具对不上，而且差异只在"两列的行"上出现，极难查。
        const std::string value = Trim(parts[1]);

        std::string digest;
        if (kind == "FILE")
        {
            const hxv4::Hash32 hash = hxv4::file_hash(Utf8ToUtf16(value), salt);
            digest = ToHex(hash.data(), hash.size());
        }
        else if (kind == "DIR")
        {
            const uint64_t hash = hxv4::domain_hash(Utf8ToUtf16(value), salt);
            uint8_t bytes[8];
            for (int index = 0; index < 8; ++index)
            {
                bytes[index] = static_cast<uint8_t>((hash >> (8 * (7 - index))) & 0xFF);   // 大端
            }
            digest = ToHex(bytes, sizeof(bytes));
        }
        else if (kind == "ROOT")
        {
            const uint64_t hash = hxv4::domain_hash(std::u16string_view(), salt);
            uint8_t bytes[8];
            for (int index = 0; index < 8; ++index)
            {
                bytes[index] = static_cast<uint8_t>((hash >> (8 * (7 - index))) & 0xFF);
            }
            digest = ToHex(bytes, sizeof(bytes));
        }
        else
        {
            std::fprintf(stderr, "unknown KIND=%s\n", kind.c_str());
            return 2;
        }

        output << kind << '\t' << value << '\t' << digest << '\n';
        ++written;
    }

    output.flush();
    std::printf("HashProbe: %d entries -> (salt %s)\n", written, salt.c_str());
    return 0;
}
