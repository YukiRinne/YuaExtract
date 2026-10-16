#pragma once

// YuaExtract 的工作区模型。
//
// 工作区 = 游戏目录。三个产物目录都由它派生，和注入模块自己的约定一致：
//   <游戏目录>\Extractor_Output         解包结果（模块写）
//   <游戏目录>\StringHashDumper_Output  Hash 映射（模块写）
//   <游戏目录>\Restored_Extractor_Output 还原结果（本工具步骤3 写）
//
// 之所以把路径推导单独放一层：界面上"当前任务/流程进度/关键目录/还原结果"四处
// 都要用同一份路径，散在界面代码里早晚会出现两处拼法不一致。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <string>

namespace Yua
{
    struct WorkspacePaths
    {
        std::wstring GameExe;
        std::wstring GameDir;       // 工作区
        std::wstring ExtractDir;    // 解包结果
        std::wstring HashDir;       // Hash 映射
        std::wstring RestoreDir;    // 还原结果
        std::wstring RestoreReport; // 还原报告
        std::wstring KeyDir;        // 静态 Key 提取结果
    };

    WorkspacePaths BuildWorkspace(const std::wstring& gameExePath);

    // 目录探测。Deep = false 时只看存在性与顶层目录数（界面定时刷新用，不能每次都递归）；
    // Deep = true 时递归统计文件数与字节数（"检查状态"按钮用）。
    struct DirProbe
    {
        bool exists = false;
        unsigned int topLevelEntries = 0;
        unsigned int files = 0;
        unsigned long long bytes = 0;
    };

    DirProbe ProbeDirectory(const std::wstring& directory, bool deep);

    // 文本文件行数（映射表条数就靠它显示），读不动就回 0。
    unsigned int CountTextFileLines(const std::wstring& path);

    // 取日志尾部若干行。只读文件末尾一块，不把整个日志读进内存。
    std::wstring ReadTextFileTail(const std::wstring& path, unsigned int maxLines);

    // 递归删除目录（先清只读属性）。目录不存在算成功。
    bool RemoveDirectoryTree(const std::wstring& directory, std::wstring& error);

    // 把文件系统时间戳转成人看的字符串；失败返回空串。
    std::wstring FileTimestampText(const std::wstring& path);

    // ---------- 共享的小格式化（界面与还原报告都要用同一套口径） ----------

    inline std::wstring FormatCount(unsigned long long value)
    {
        const std::wstring text = std::to_wstring(value);
        std::wstring grouped;
        grouped.reserve(text.size() + text.size() / 3 + 1);
        int counter = 0;
        for (size_t index = text.size(); index > 0; --index)
        {
            grouped.push_back(text[index - 1]);
            if (++counter == 3 && index > 1)
            {
                grouped.push_back(L',');
                counter = 0;
            }
        }
        std::reverse(grouped.begin(), grouped.end());
        return grouped;
    }

    inline std::wstring FormatSize(unsigned long long bytes)
    {
        const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
        double value = static_cast<double>(bytes);
        size_t unit = 0;
        while (value >= 1024.0 && unit + 1 < (sizeof(units) / sizeof(units[0])))
        {
            value /= 1024.0;
            ++unit;
        }
        wchar_t buffer[64]{};
        if (unit == 0)
        {
            ::swprintf_s(buffer, L"%llu %s", bytes, units[0]);
        }
        else
        {
            ::swprintf_s(buffer, L"%.2f %s", value, units[unit]);
        }
        return buffer;
    }

    inline std::wstring TimestampNow()
    {
        SYSTEMTIME time{};
        ::GetLocalTime(&time);
        wchar_t buffer[64]{};
        ::swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth, time.wDay,
                     time.wHour, time.wMinute, time.wSecond);
        return buffer;
    }

    inline std::wstring JoinPath(const std::wstring& directory, const std::wstring& name)
    {
        if (directory.empty())
        {
            return name;
        }
        if (name.empty())
        {
            return directory;
        }
        if (directory.back() == L'\\' || directory.back() == L'/')
        {
            return directory + name;
        }
        return directory + L'\\' + name;
    }
}
