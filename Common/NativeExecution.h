#pragma once

// 游戏内嵌原生代码（BOOTSTRAP DLL）执行的显式授权位。
//
// 本工具链里有两处会**真的把游戏自带的 DLL 映射进当前进程并调用其中的函数**：
//   1) CxdecKeyStatic 的 DRIP 程序派生（FilterManager::derive_drip_program）；
//   2) CxdecRepacker 在仓库里找不到参数时调用上面那份派生。
// 这两件事都不是 sandbox：被调用的代码和本工具同权限、同地址空间。
//
// 所以默认**一律不执行**：旧导出（ExtractKey / Repack / DeriveKeys）固定按
// flags = 0 调用，行为是"明确失败并说明需要授权"，不会静默继续，也不会生成
// 一个用内置参数凑出来的错误产物。只有新导出（ExtractKeyEx / RepackEx /
// DeriveKeysEx）的 bit0 能放行，且必须在调用点由用户显式确认。
//
// 这里只做位定义与文案，不引入任何运行时状态；**不要**用进程环境变量做 opt-in，
// 也不要让旧导出自己把这一位置 true。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <string>

namespace NativeExecution
{
    // 唯一合法的授权位：允许执行可信游戏内嵌的原生代码。
    constexpr DWORD kFlagAllow = 0x00000001u;

    // 目前认识的全部位。其余位一律视为调用方写错，立即拒绝。
    constexpr DWORD kKnownFlags = kFlagAllow;

    inline bool HasUnknownFlags(DWORD flags)
    {
        return (flags & ~kKnownFlags) != 0;
    }

    inline bool IsAllowed(DWORD flags)
    {
        return (flags & kFlagAllow) != 0;
    }

    // UTF-8。用于各导出给调用方的 errorOut（导出边界统一是 ANSI/UTF-8 混用，
    // ExportGuard::WriteAnsi 会做转换）。
    inline const char* DeniedMessage()
    {
        return "拒绝执行游戏内嵌的原生代码：该操作需要把游戏自带的 DLL 映射进本进程并调用，"
               "这不是 sandbox。默认已禁止；如确认目标游戏可信，请通过显式授权入口"
               "（ExtractKeyEx / RepackEx / DeriveKeysEx 的 flags bit0，或 CLI 的 "
               "--allow-native-code）重试。旧接口不会隐式授权。";
    }

    inline std::string UnknownFlagsMessage(DWORD flags)
    {
        char buffer[128] = {};
        _snprintf_s(buffer, _countof(buffer), _TRUNCATE,
                    "未知的授权标志 0x%08lX：只接受 bit0（0x1，允许执行游戏内嵌原生代码）。",
                    static_cast<unsigned long>(flags));
        return std::string(buffer);
    }
}
