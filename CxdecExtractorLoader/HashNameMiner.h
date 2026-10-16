#pragma once

// 「补充还原」的候选挖掘内核 —— 从游戏自己的资源里挖出候选名，用哈希验证后
// 产出"哈希 -> 真名"的对照。
//
// 为什么它写不错名字：
//   候选名（来源可以很随便） → 算哈希 → 与磁盘上待还原文件的哈希集合求交 → 命中才写入。
// 每一条都要过一次哈希相等，所以最坏情况只是**补不上**，不会补错。这条是硬底线，
// 调用方也不允许绕过它自己造名字。
//
// 它为什么不是"猜"：
//   游戏把资源名以明文存在自己的脚本/配置资源里（这个游戏是 PSB + UTF-8），
//   所以名字本来就在那儿，我们只是把它挖出来并用哈希确认它的归属。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Yua
{
    namespace Miner
    {
        // 哈希用的盐，来自 <工具目录>\Log\Universal.log（运行时收集模块写的）。
        // 必须至少跑过一次步骤2 才有这个文件 —— 这也是本功能的前提。
        struct SaltInfo
        {
            bool available = false;
            std::wstring seed;                // "Hash Seed"，**原样字符串**（不是 hex 解码）
            std::wstring fileNameHasherSalt;  // 可为空；非空时 BLAKE2s 要带 key
            std::wstring pathNameHasherSalt;  // 本内核不用（目录哈希由运行时模块收集）
            std::wstring source;              // 实际读到的 Universal.log 路径（写报告用）
            std::wstring message;             // 读不到时的原因，要能直接给用户看
        };

        // 读 <logDirectory>\Universal.log 里的 Hash Seed / FileNameHasherSalt / PathNameHasherSalt。
        // 读不到不是异常情况：available=false + message 说明原因，调用方据此提示用户。
        SaltInfo ReadSalt(const std::wstring& logDirectory);

        struct Options
        {
            std::vector<std::wstring> scanRoots;      // 要挖的包目录（绝对路径）
            std::vector<std::wstring> excludeNames;   // 跳过的包名（纯二进制，如 voice/voice2/video）
            unsigned int maxCandidates = 4000000u;    // 候选上限，防爆炸（曾出现过 3356 万候选）
            std::function<bool()> cancelRequested;    // 返回 true 表示用户中止
            std::function<void(unsigned int files, unsigned int candidates, unsigned int hits)> progress;
        };

        struct Result
        {
            std::map<std::wstring, std::wstring> files;   // 大写 64 位 hash -> 明文名（含后缀）
            unsigned int scannedFiles = 0;
            unsigned int candidates = 0;
            unsigned int triedHashes = 0;
            bool limitHit = false;                        // 触到候选上限，结果不完整
            std::wstring message;                         // 异常/中止时的说明
        };

        // targets 必须是**大写**的 64 位哈希集合（磁盘上仍是 Hash 名的那些文件）。
        // 返回的 files 里每一条都已通过哈希验证。
        Result MineNames(const SaltInfo& salt, const std::set<std::wstring>& targets, const Options& options);
    }
}
