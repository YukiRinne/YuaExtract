// RemoveOwnedTreeNoFollow 的"不跟随重解析点"回归（headless）。
//
// 直接 include 生产 keystore.cpp，在临时目录里造：
//   - 普通目录树；
//   - 指向外部 sentinel 目录的 directory symlink（子项与自身两种形态）。
// 调用生产清理函数后：自己目录被删，sentinel 目录及其文件必须原样存在。
//
// 用法：native-cleanup-test.exe <临时根目录>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "../../CxdecRepacker/pack_static/keystore.cpp"

namespace fs = std::filesystem;

// 本测试只调用清理 helper；为保持单文件 headless 构建，提供 GameProfile::valid
// 的最小定义（profile.cpp 会牵入内置参数/CafeStella 数据，与本次测试无关）。
namespace hxv4 {
DripProgram::DripProgram(std::vector<uint32_t> holder_words, std::vector<uint32_t> context,
                         std::vector<std::vector<DripRecord>> lanes)
    : holder_words_(std::move(holder_words)),
      context_(std::move(context)),
      lanes_(std::move(lanes)) {}
namespace pack_static {
bool GameProfile::valid() const { return drip.valid() && !id.empty(); }
}
}

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

bool PathExists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WriteText(const fs::path& path, const char* text) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return false;
    const size_t n = std::strlen(text);
    const bool ok = fwrite(text, 1, n, f) == n;
    fclose(f);
    return ok;
}

std::string ReadText(const fs::path& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (f == nullptr) return {};
    char buf[64] = {};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return std::string(buf, n);
}

bool MakeDirectoryLink(const fs::path& link, const fs::path& target) {
    DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY;
    // 0x2 = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE（旧 SDK 没这个名字）
    const BOOL ok = ::CreateSymbolicLinkW(link.wstring().c_str(), target.wstring().c_str(),
                                          flags | 0x2u);
    return ok != FALSE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: native-cleanup-test <tempRoot>\n");
        return 2;
    }

    const fs::path root = fs::path(argv[1]) / L"cleanup";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const fs::path outside = root / L"outside";
    fs::create_directories(outside, ec);
    Check(WriteText(outside / L"keep.txt", "KEEP"), "outside sentinel created");

    // ---- 1. 普通树：全部删掉 ----
    {
        const fs::path owned = root / L"plain";
        fs::create_directories(owned / L"sub", ec);
        WriteText(owned / L"a.txt", "A");
        WriteText(owned / L"sub" / L"b.txt", "B");
        hxv4::pack_static::RemoveOwnedTreeNoFollow(owned);
        Check(!PathExists(owned.wstring()), "plain owned tree removed");
        Check(ReadText(outside / L"keep.txt") == "KEEP", "outside sentinel untouched by plain removal");
    }

    // ---- 2. 子树里含 directory symlink：只删链接，不动目标 ----
    {
        const fs::path owned = root / L"with_link";
        fs::create_directories(owned, ec);
        WriteText(owned / L"a.txt", "A");
        const fs::path link = owned / L"link_to_outside";
        if (!MakeDirectoryLink(link, outside)) {
            std::printf("SKIP cannot create directory symlink (privilege?)\n");
        } else {
            Check(PathExists(link.wstring()), "child symlink created");
            hxv4::pack_static::RemoveOwnedTreeNoFollow(owned);
            Check(!PathExists(owned.wstring()), "owned tree with child symlink removed");
            Check(PathExists(outside.wstring()), "child symlink target directory still exists");
            Check(ReadText(outside / L"keep.txt") == "KEEP",
                  "child symlink target file untouched");
        }
    }

    // ---- 3. 被清理路径本身是 symlink：删链接，不删目标 ----
    {
        const fs::path owned_link = root / L"owned_is_link";
        if (!MakeDirectoryLink(owned_link, outside)) {
            std::printf("SKIP cannot create self symlink (privilege?)\n");
        } else {
            Check(PathExists(owned_link.wstring()), "self symlink created");
            hxv4::pack_static::RemoveOwnedTreeNoFollow(owned_link);
            Check(!PathExists(owned_link.wstring()), "self symlink removed");
            Check(PathExists(outside.wstring()), "self symlink target directory still exists");
            Check(ReadText(outside / L"keep.txt") == "KEEP",
                  "self symlink target file untouched");
        }
    }

    std::error_code cleanup_ec;
    fs::remove_all(root, cleanup_ec);

    std::printf("RESULT failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
