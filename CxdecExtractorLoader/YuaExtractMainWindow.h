#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <string>

namespace Yua
{
    // 创建 YuaExtract 主窗口、跑消息循环，返回进程退出码。
    // initialExe 非空时预填"目标游戏"（命令行/拖到 exe 上的场景）。
    int RunMainWindow(HINSTANCE instance, const std::wstring& initialExe);
}
