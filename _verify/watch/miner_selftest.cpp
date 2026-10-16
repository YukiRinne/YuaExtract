// HashNameMiner 自测：在真实解包产物上跑一遍挖掘，对照 Python 参考实现的命中数。
// 临时验证工具，不属于产品，编译产物放 %TEMP%。
//
// 用法: miner_selftest.exe <Extractor_Output> <工具目录\Log>
//
// 注意必须用 wmain：游戏目录里有日文，窄字符 argv 经过 ACP 转换会毁掉路径
// （第一次跑就是这么得到 0 个目标的）。

#include <cstdio>
#include <string>
#include <vector>

#include "HashNameMiner.h"

namespace
{
    bool IsHex64(const std::wstring& name)
    {
        if (name.size() != 64u)
        {
            return false;
        }
        for (wchar_t c : name)
        {
            const bool hex = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f');
            if (!hex)
            {
                return false;
            }
        }
        return true;
    }

    std::wstring UpperCase(const std::wstring& text)
    {
        std::wstring out = text;
        for (wchar_t& c : out)
        {
            if (c >= L'a' && c <= L'f')
            {
                c = static_cast<wchar_t>(c - L'a' + L'A');
            }
        }
        return out;
    }

    // 递归收集 64 位哈希名；顺带统计非哈希名文件
    void CollectTargets(const std::wstring& directory, std::set<std::wstring>& targets, unsigned int& plainNames)
    {
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW((directory + L"\\*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            std::wprintf(L"  [警告] 打不开目录: %s\n", directory.c_str());
            return;
        }
        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const std::wstring full = directory + L"\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                CollectTargets(full, targets, plainNames);
            }
            else if (IsHex64(name))
            {
                targets.insert(UpperCase(name));
            }
            else
            {
                ++plainNames;
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }

    // 顶层子目录 = 封包
    std::vector<std::wstring> CollectPackages(const std::wstring& root)
    {
        std::vector<std::wstring> packages;
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW((root + L"\\*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return packages;
        }
        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                && name != L"Extractor_Log" && name != L"ExtractLog")
            {
                packages.push_back(root + L"\\" + name);
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
        return packages;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::printf("用法: miner_selftest.exe <Extractor_Output> <Log目录>\n");
        return 2;
    }

    const std::wstring outDir = argv[1];
    const std::wstring logDir = argv[2];

    std::printf("=== ReadSalt ===\n");
    const Yua::Miner::SaltInfo salt = Yua::Miner::ReadSalt(logDir);
    std::printf("  available = %s\n", salt.available ? "true" : "false");
    if (!salt.available)
    {
        std::wprintf(L"  message   = %s\n", salt.message.c_str());
        return 1;
    }
    std::printf("  seed      = %ls\n", salt.seed.c_str());
    std::printf("  fileSalt  = [%ls]\n", salt.fileNameHasherSalt.c_str());

    std::set<std::wstring> targets;
    unsigned int plainNames = 0;
    CollectTargets(outDir, targets, plainNames);
    const std::vector<std::wstring> packages = CollectPackages(outDir);

    std::printf("\n=== 目标集合 ===\n");
    std::printf("  64 位哈希名文件 = %u\n", static_cast<unsigned int>(targets.size()));
    std::printf("  非哈希名文件    = %u\n", plainNames);
    std::printf("  顶层封包        = %u\n", static_cast<unsigned int>(packages.size()));

    Yua::Miner::Options options;
    options.scanRoots = packages;
    options.excludeNames = { L"Extractor_Log", L"ExtractLog" };

    // 可选第 3 个参数：逗号分隔的包名白名单（诊断用）
    if (argc >= 4 && argv[3][0] != L'\0')
    {
        std::vector<std::wstring> filtered;
        std::wstring wanted = argv[3];
        size_t start = 0;
        while (start <= wanted.size())
        {
            size_t comma = wanted.find(L',', start);
            if (comma == std::wstring::npos) comma = wanted.size();
            const std::wstring name = wanted.substr(start, comma - start);
            for (const std::wstring& pkg : packages)
            {
                const std::wstring leaf = pkg.substr(pkg.find_last_of(L'\\') + 1);
                if (leaf == name)
                {
                    filtered.push_back(pkg);
                }
            }
            if (comma == wanted.size()) break;
            start = comma + 1;
        }
        options.scanRoots = filtered;
        std::printf("  白名单包: %u 个\n", static_cast<unsigned int>(filtered.size()));
    }

    int progressLines = 0;
    options.progress = [&progressLines](unsigned int files, unsigned int candidates, unsigned int hits)
    {
        if (++progressLines % 20 == 0)
        {
            std::printf("  ...扫描 %u 文件 / 候选 %u / 命中 %u\n", files, candidates, hits);
        }
    };

    const ULONGLONG started = ::GetTickCount64();
    const Yua::Miner::Result result = Yua::Miner::MineNames(salt, targets, options);
    const ULONGLONG elapsed = ::GetTickCount64() - started;

    std::printf("\n=== 结果 ===\n");
    std::printf("  扫描文件   = %u\n", result.scannedFiles);
    std::printf("  候选       = %u\n", result.candidates);
    std::printf("  试算哈希   = %u\n", result.triedHashes);
    std::printf("  命中       = %u\n", static_cast<unsigned int>(result.files.size()));
    std::printf("  触顶       = %s\n", result.limitHit ? "yes" : "no");
    std::printf("  耗时       = %llu ms\n", static_cast<unsigned long long>(elapsed));
    if (!result.message.empty())
    {
        std::printf("  message    = %ls\n", result.message.c_str());
    }

    std::printf("\n=== 抽查前 5 条 ===\n");
    int shown = 0;
    for (const auto& item : result.files)
    {
        std::printf("  %ls : %ls\n", item.first.c_str(), item.second.c_str());
        if (++shown >= 5)
        {
            break;
        }
    }

    std::printf("\n参照: Python 参考实现同一份数据命中 52906（它只扫 scn/data/image/uipsd/bgimage/\n");
    std::printf("      evimage/evimage2/fgimage 八个包；本测试扫全部封包，命中数应 >= 它）。\n");
    return 0;
}
