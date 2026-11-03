#pragma once
#include <windows.h>
#include "CxdecRecover.h"

#ifdef __cplusplus
extern "C" {
#endif

// 旧入口：保持原签名/导出名（@16）。安全默认 —— 等价于 flags = 0，
// **不会**执行游戏内嵌 DLL；未显式授权时直接失败并写明原因。
__declspec(dllexport) BOOL __stdcall ExtractKey(
    const wchar_t* exePath,
    const wchar_t* outputDir,
    char* errorOut,
    int errorOutSize);

// 显式授权入口。
//   flags bit0 = 允许执行游戏内嵌原生代码（仅此位合法；其余位立即拒绝）。
//   flags = 0 与旧入口行为一致（安全默认）。
// 授权后会把游戏自带的 BOOTSTRAP DLL 映射进本进程并调用其导出函数；
// 这不是 sandbox，代码与本工具同权限、同地址空间。
__declspec(dllexport) BOOL __stdcall ExtractKeyEx(
    const wchar_t* exePath,
    const wchar_t* outputDir,
    DWORD flags,
    char* errorOut,
    int errorOutSize);

// 让导出表同时包含无装饰名 "ExtractKey"/"ExtractKeyEx"，便于 GetProcAddress
// 直接按名字查找。@N 是 __stdcall 参数字节数（x86），改签名必须同步改这里。
#pragma comment(linker, "/EXPORT:ExtractKey=_ExtractKey@16")
#pragma comment(linker, "/EXPORT:ExtractKeyEx=_ExtractKeyEx@20")

#ifdef __cplusplus
}
#endif
