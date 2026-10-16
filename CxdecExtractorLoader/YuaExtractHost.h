#pragma once

// YuaExtract 新主界面与上游 Loader 实现之间的桥。
//
// 上游那一大堆 helper（浏览 exe、校验、定位模块 DLL、启动并注入、静态取 Key、
// 原版功能窗口……）都在 CxdecExtractorLoader.cpp 的匿名命名空间里，外部看不见。
// 与其把那个文件拆开重排（改动面大、回归风险高），不如在这里声明一层窄接口，
// 由 CxdecExtractorLoader.cpp 末尾实现转发。
//
// 新界面（YuaExtractMainWindow.cpp）只依赖本头文件。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <string>

#include "log.h"

namespace Yua
{
namespace Host
{
    // Loader 自己的会话日志文件名（工具目录\Log\ 下）。
    // 上游叫 CxdecExtractorLoader.log；既然产品改名为 YuaExtract，日志也跟着改，
    // 界面上的「显示详细日志」读的就是这个文件。
    constexpr const wchar_t* kLogFileName = L"YuaExtract.log";

    // 界面标题与版本
    constexpr const wchar_t* kProductName = L"YuaExtract";
    constexpr const wchar_t* kProductVersion = L"1.0.0";

    std::wstring LoaderDirectory();
    std::wstring LogDirectory();

    std::wstring ResolveModulePath(const std::wstring& dllFileName);
    bool ModuleExists(const std::wstring& dllFileName);
    // 拼一句"缺哪个 DLL、该放哪儿"的提示，几个入口共用。
    std::wstring ModuleMissingHint(const std::wstring& dllFileName);

    std::wstring TargetExe();
    std::wstring TargetDirectory();
    void SetTargetExe(const std::wstring& exePath);
    bool ValidateGameExe(HWND owner, const std::wstring& path);
    bool PickGameExe(HWND owner, std::wstring& chosen);
    std::wstring BrowseForFolder(HWND owner, const wchar_t* title);

    // 启动游戏并注入模块。
    //   runtimeHashTargetDirectory 非空时写进 CXDEC_RUNTIME_HASH_TARGET_DIR，
    //   CxdecStringDumper 会用它预填"待恢复目录"。
    // 只负责"起进程 + 注入"，不关闭本窗口，也不接管进度回调。
    bool LaunchGameWithModule(HWND owner, const std::wstring& dllFileName,
                              const std::wstring& runtimeHashTargetDirectory, std::wstring& error);

    // 选定目标后触发上游的 SteamStub 检测/反篡改补丁流程。
    // 返回 true = 本次调用已交给补丁流程处理（可能生成了 <名字>_crack.exe），
    // 调用方应当检查 CrackedExePath 并据此决定后续动作。
    bool PrepareTargetExe(HWND owner, const std::wstring& exePath);

    // 反篡改补丁流程产出的成品：<exePath 去掉扩展名>_crack.exe（存在才返回，否则空串）。
    // 上游的做法是弹框让用户"把它拖回来继续"，我们直接接上，省掉这次手动操作。
    std::wstring CrackedExePath(const std::wstring& exePath);

    // 上游原版功能窗口（解包 / 运行时恢复 / Hook 撞库 / 提取Key / 封包）。
    void ShowLegacyFunctionsWindow(HWND owner);
    // 静态 Key 提取（等价于原版窗口里的「提取Key」）。
    void RunStaticKeyExtractionForTarget(HWND owner);

    void EnableDropOnWindow(HWND hwnd);

    void LogLine(Log::Level level, const std::wstring& text);
    std::wstring Format(const wchar_t* format, ...);

    bool Confirm(HWND owner, const std::wstring& text, const std::wstring& caption);
    void ShowInfo(HWND owner, const std::wstring& text);
    void ShowError(HWND owner, const std::wstring& text);
    bool OpenInExplorer(HWND owner, const std::wstring& path);
    bool OpenWithShell(HWND owner, const std::wstring& path);
}
}
