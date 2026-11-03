// FilterManager::WritePrivateTempDll / TempFileGuard 的所有权回归（headless）。
//
// 直接 include 生产 FilterManager.cpp，用 forced_path 测试 seam 固定候选名：
//   - CREATE_NEW 撞名（已存在 sentinel 文件）：函数返回 false、guard.owned=false，
//     guard 析构不得删除别人的 sentinel；
//   - CREATE_NEW 其它失败（候选是目录）：同样 owned=false，目录完好；
//   - 成功 CREATE_NEW：owned=true，guard 析构删除自己的临时文件。
//
// 用法：native-tempfile-test.exe <临时根目录>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../CxdecKeyStatic/FilterManager.cpp"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

bool WriteFileBytes(const fs::path& path, const char* text) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return false;
    const size_t n = std::strlen(text);
    const bool ok = fwrite(text, 1, n, f) == n;
    fclose(f);
    return ok;
}

std::string ReadFileText(const fs::path& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (f == nullptr) return {};
    char buf[64] = {};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return std::string(buf, n);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: native-tempfile-test <tempRoot>\n");
        return 2;
    }

    const fs::path root = fs::path(argv[1]) / L"tempfile";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    uint8_t payload[7] = {1, 2, 3, 4, 5, 6, 7};

    // ---- 1. 撞名：sentinel 已存在，CREATE_NEW 每次都失败 ----
    {
        const fs::path sentinel = root / L"collide.dll";
        Check(WriteFileBytes(sentinel, "SENTINEL"), "sentinel created");
        {
            FilterManager::TempFileGuard guard;
            const bool ok = FilterManager::WritePrivateTempDll(
                payload, sizeof(payload), guard, sentinel.c_str());
            Check(!ok, "collision returns false");
            Check(!guard.owned, "collision guard is not owner");
            Check(guard.path == sentinel.wstring(), "collision path is the injected candidate");
        }
        Check(fs::exists(sentinel, ec), "collision sentinel still exists");
        Check(ReadFileText(sentinel) == "SENTINEL", "collision sentinel content untouched");
    }

    // ---- 2. CREATE_NEW 其它失败：候选路径是目录 ----
    {
        const fs::path as_dir = root / L"asdir.dll";
        fs::create_directories(as_dir, ec);
        {
            FilterManager::TempFileGuard guard;
            const bool ok = FilterManager::WritePrivateTempDll(
                payload, sizeof(payload), guard, as_dir.c_str());
            Check(!ok, "CREATE_NEW on directory fails");
            Check(!guard.owned, "failed CREATE_NEW guard is not owner");
        }
        Check(fs::is_directory(as_dir, ec), "existing directory untouched");
    }

    // ---- 3. 成功 CREATE_NEW：guard 拥有并在析构时删除自己的临时文件 ----
    {
        const fs::path ok_path = root / L"owned.dll";
        std::wstring created;
        {
            FilterManager::TempFileGuard guard;
            const bool ok = FilterManager::WritePrivateTempDll(
                payload, sizeof(payload), guard, ok_path.c_str());
            Check(ok, "CREATE_NEW succeeds for fresh candidate");
            Check(guard.owned, "successful candidate is owned");
            created = guard.path;
            Check(fs::exists(ok_path, ec), "owned temp file exists while guard alive");
        }
        Check(!created.empty() && !fs::exists(created, ec), "guard deleted its own temp file");
    }

    // ---- 4. 生产默认命名器：成功创建，guard 析构清理 ----
    {
        std::wstring created;
        {
            FilterManager::TempFileGuard guard;
            const bool ok = FilterManager::WritePrivateTempDll(payload, sizeof(payload), guard);
            Check(ok, "default naming succeeds");
            Check(guard.owned, "default-named candidate is owned");
            created = guard.path;
            Check(fs::exists(created, ec), "default-named temp file exists while guard alive");
        }
        Check(!created.empty() && !fs::exists(created, ec), "default-named temp file cleaned up");
    }

    std::error_code cleanup_ec;
    fs::remove_all(root, cleanup_ec);

    std::printf("RESULT failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
