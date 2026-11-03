#include "ExportApi.h"
#include "TryExport.h"
#include "NativeExecution.h"

#include <cstring>
#include <string>

namespace {

// ExtractKey / ExtractKeyEx 的公共实现。导出边界所有 C++ 异常都由 ExportGuard
// 转成 errorOut，不让异常穿过 DLL 边界。
BOOL RunExtract(const wchar_t* exePath, const wchar_t* outputDir, DWORD flags,
                char* errorOut, int errorOutSize) {
    if (errorOut != nullptr && errorOutSize > 0) errorOut[0] = '\0';

    const bool ok = ExportGuard::Run(
        [&](const std::string& m) { ExportGuard::WriteAnsi(m, errorOut, errorOutSize); },
        [&]() -> bool {
            if (exePath == nullptr || outputDir == nullptr) {
                ExportGuard::WriteAnsi("游戏 EXE 或输出目录为空", errorOut, errorOutSize);
                return false;
            }

            // 未知位一律拒绝，不能忽略后再继续（那等于把授权面放大）。
            if (NativeExecution::HasUnknownFlags(flags)) {
                ExportGuard::WriteAnsi(NativeExecution::UnknownFlagsMessage(flags),
                                       errorOut, errorOutSize);
                return false;
            }

            Engine::GameParams params;
            params.allow_native_execution = NativeExecution::IsAllowed(flags);

            std::string error;
            if (!Engine::recover_drip_program(exePath, outputDir, params, &error)) {
                if (!error.empty()) ExportGuard::WriteAnsi(error, errorOut, errorOutSize);
                return false;
            }
            return true;
        });
    return ok ? TRUE : FALSE;
}

}  // namespace

BOOL __stdcall ExtractKey(const wchar_t* exePath, const wchar_t* outputDir,
                          char* errorOut, int errorOutSize) {
    // 旧接口固定 flags = 0：兼容导出名，但绝不隐式执行内嵌 DLL。
    return RunExtract(exePath, outputDir, 0u, errorOut, errorOutSize);
}

BOOL __stdcall ExtractKeyEx(const wchar_t* exePath, const wchar_t* outputDir, DWORD flags,
                            char* errorOut, int errorOutSize) {
    return RunExtract(exePath, outputDir, flags, errorOut, errorOutSize);
}
