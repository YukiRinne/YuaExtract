// security-extension-tests.cpp
// 扩展集内核 / 在线更新 安全与事务回归（headless Win32 x86，无 UI、无网络）。
// 直接编入生产 ExtensionSet.cpp / OnlineUpdate.cpp，调用真实公开入口；不复制算法。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include "ExtensionSet.h"
#include "OnlineUpdate.h"
#include "AtomicFile.h"
#include "zlib.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

int AtomicFileHeaderSelfCheck();

namespace Yua
{
namespace Host
{
    std::wstring g_toolDirectory;
    std::wstring LoaderDirectory()
    {
        return g_toolDirectory;
    }
}
}

namespace
{
    int g_checks = 0;
    int g_failures = 0;
    std::wstring g_root;

    constexpr size_t kCap = 64ull * 1024ull * 1024ull;

    std::wstring FormatUInt(unsigned long long value)
    {
        wchar_t buffer[32]{};
        ::swprintf_s(buffer, L"%llu", value);
        return buffer;
    }

    std::wstring Combine(const std::wstring& a, const std::wstring& b)
    {
        if (a.empty()) return b;
        if (b.empty()) return a;
        if (a.back() == L'\\' || a.back() == L'/') return a + b;
        return a + L"\\" + b;
    }

    bool DirectoryExists(const std::wstring& path)
    {
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    bool FileExists(const std::wstring& path)
    {
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    bool MakeDirectoryTree(const std::wstring& path)
    {
        if (DirectoryExists(path)) return true;
        const std::wstring parent = path.substr(0u, path.find_last_of(L"\\/"));
        if (!parent.empty() && parent != path && !MakeDirectoryTree(parent)) return false;
        return ::CreateDirectoryW(path.c_str(), nullptr) != FALSE
            || ::GetLastError() == ERROR_ALREADY_EXISTS;
    }

    bool ReadBytes(const std::wstring& path, std::vector<unsigned char>& out)
    {
        out.clear();
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0)
        {
            ::CloseHandle(file);
            return false;
        }
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t offset = 0u;
        while (offset < out.size())
        {
            DWORD read = 0u;
            if (!::ReadFile(file, out.data() + offset,
                            static_cast<DWORD>(std::min<size_t>(out.size() - offset, 1u << 20)),
                            &read, nullptr) || read == 0u)
            {
                ::CloseHandle(file);
                return false;
            }
            offset += read;
        }
        ::CloseHandle(file);
        return true;
    }

    bool WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes)
    {
        MakeDirectoryTree(path.substr(0u, path.find_last_of(L"\\/")));
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0u;
        const bool ok = bytes.empty()
            || (::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
                && written == bytes.size());
        ::CloseHandle(file);
        return ok;
    }

    void Snapshot(const std::wstring& root, const std::wstring& relative,
                  std::map<std::wstring, std::wstring>& out)
    {
        const std::wstring current = relative.empty() ? root : Combine(root, relative);
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(Combine(current, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return;
        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring childRel = relative.empty() ? name : relative + L"\\" + name;
            const std::wstring full = Combine(root, childRel);
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                out[childRel] = L"<reparse>";
            }
            else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                out[childRel] = L"<dir>";
                Snapshot(root, childRel, out);
            }
            else
            {
                LARGE_INTEGER size{};
                size.LowPart = data.nFileSizeLow;
                size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
                out[childRel] = L"file:" + FormatUInt(static_cast<unsigned long long>(size.QuadPart))
                              + L":" + Yua::ExtensionSet::Sha256HexFile(full);
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }

    std::map<std::wstring, std::wstring> Snapshot(const std::wstring& root)
    {
        std::map<std::wstring, std::wstring> out;
        if (DirectoryExists(root)) Snapshot(root, L"", out);
        return out;
    }

    std::vector<std::wstring> FindArtifacts(const std::wstring& root)
    {
        std::vector<std::wstring> found;
        const std::map<std::wstring, std::wstring> snapshot = Snapshot(root);
        for (const auto& item : snapshot)
        {
            if (item.first.find(L".yua-tmp.") != std::wstring::npos
                || item.first.find(L".yua-bak.") != std::wstring::npos)
            {
                found.push_back(item.first);
            }
        }
        return found;
    }

    std::string ToUtf8(const std::wstring& text)
    {
        if (text.empty()) return std::string();
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                               static_cast<int>(text.size()), nullptr, 0,
                                               nullptr, nullptr);
        if (need <= 0) return std::string();
        std::string out(static_cast<size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              &out[0], need, nullptr, nullptr);
        return out;
    }

    std::vector<unsigned char> TextBytes(const std::wstring& text)
    {
        const std::string utf8 = ToUtf8(text);
        return std::vector<unsigned char>(utf8.begin(), utf8.end());
    }

    bool LastWriteTime(const std::wstring& path, unsigned long long& value)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        {
            return false;
        }
        value = (static_cast<unsigned long long>(static_cast<unsigned long>(data.ftLastWriteTime.dwHighDateTime)) << 32)
              | data.ftLastWriteTime.dwLowDateTime;
        return true;
    }

    std::wstring ParentOf(const std::wstring& path)
    {
        const size_t separator = path.find_last_of(L"\\/");
        return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
    }

    int CountSessionDirectories(const std::wstring& base)
    {
        int count = 0;
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(Combine(base, L"session_*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return 0;
        do
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) ++count;
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
        return count;
    }

    void Check(bool condition, const std::string& name, const std::wstring& detail = L"")
    {
        ++g_checks;
        std::printf("SEC-EXT %s %s", condition ? "PASS" : "FAIL", name.c_str());
        if (!detail.empty())
        {
            std::printf(" | %ls", detail.c_str());
        }
        std::printf("\n");
        if (!condition) ++g_failures;
    }

    Yua::ExtensionSet::Archive MakeArchive(const std::wstring& publisher,
                                           const std::wstring& title,
                                           const std::wstring& exeDigest,
                                           const std::wstring& salt,
                                           const std::wstring& marker)
    {
        Yua::ExtensionSet::Archive archive;
        archive.meta.publisher = publisher;
        archive.meta.title = title;
        archive.meta.contributor = L"测试";
        archive.meta.version = L"v1";
        archive.meta.date = L"2026-11-03";
        archive.meta.comment = marker;
        archive.identity.targetExe.name = L"game.exe";
        archive.identity.targetExe.size = 12345;
        archive.identity.targetExe.sha256 = exeDigest;
        archive.identity.salt = salt;
        archive.identity.saltKnown = true;
        archive.identity.sourceDirectory = L"Restored_Extractor_Output";
        archive.directories = {L"system/", L"/"};
        archive.files = {L"a.txt", L"中文名字.png"};
        archive.stats.directories = 2u;
        archive.stats.files = 2u;
        archive.stats.exceptions = 0u;
        return archive;
    }

    std::vector<unsigned char> EncodeOk(const Yua::ExtensionSet::Archive& archive)
    {
        std::wstring error;
        std::vector<unsigned char> blob = Yua::ExtensionSet::EncodeArchive(archive, error);
        if (blob.empty())
        {
            Check(false, "encode", error);
        }
        return blob;
    }

    Yua::ExtensionSet::LibraryEntry MakeEntry(const Yua::ExtensionSet::Archive& archive,
                                              const std::vector<unsigned char>& blob)
    {
        Yua::ExtensionSet::LibraryEntry entry;
        entry.publisher = archive.meta.publisher;
        entry.title = archive.meta.title;
        entry.exeDigest = archive.identity.targetExe.sha256;
        entry.saltDigest = Yua::ExtensionSet::Sha256HexText(archive.identity.salt);
        entry.archiveDigest = Yua::ExtensionSet::Sha256HexBytes(blob.data(), blob.size());
        entry.entryCount = archive.stats.directories + archive.stats.files;
        return entry;
    }

    std::wstring ExeDigest(wchar_t ch)
    {
        // 必须是合法的 64 位十六进制，否则 manifest 解析会把整份当坏文本。
        static const wchar_t* digits = L"0123456789ABCDEF";
        const wchar_t digit = digits[static_cast<unsigned>(ch) & 0x0Fu];
        return std::wstring(64u, digit);
    }

    // ---------------- 原始容器构造（用于上限 / 截断 / CRC 负例） ----------------
    struct RawChunk
    {
        uint16_t id;
        std::vector<unsigned char> body;
    };

    std::vector<unsigned char> BuildRaw(const std::vector<RawChunk>& chunks, bool compress,
                                       uint32_t crcOverride = 0u, bool overrideCrc = false)
    {
        std::vector<unsigned char> payload;
        struct Entry { uint16_t id; uint32_t offset; uint32_t size; };
        std::vector<Entry> table;
        for (const RawChunk& chunk : chunks)
        {
            Entry entry{chunk.id, static_cast<uint32_t>(payload.size()),
                        static_cast<uint32_t>(chunk.body.size())};
            table.push_back(entry);
            payload.insert(payload.end(), chunk.body.begin(), chunk.body.end());
        }

        std::vector<unsigned char> stored = payload;
        uint32_t flags = 0u;
        if (compress)
        {
            uLongf bound = ::compressBound(static_cast<uLong>(payload.size()));
            stored.resize(static_cast<size_t>(bound));
            const int status = ::compress2(stored.data(), &bound, payload.data(),
                                           static_cast<uLong>(payload.size()), 1);
            if (status != Z_OK) return {};
            stored.resize(static_cast<size_t>(bound));
            flags = 1u;
        }

        const uint32_t crc = overrideCrc
            ? crcOverride
            : static_cast<uint32_t>(::crc32(0L, payload.data(), static_cast<uInt>(payload.size())));

        std::vector<unsigned char> out;
        auto u16 = [&](uint16_t v) {
            out.push_back(static_cast<unsigned char>(v & 0xFFu));
            out.push_back(static_cast<unsigned char>((v >> 8) & 0xFFu));
        };
        auto u32 = [&](uint32_t v) {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFFu));
        };
        u32(0x31535859u); u16(1u); u16(20u); u32(flags); u32(crc);
        u32(static_cast<uint32_t>(table.size()));
        for (const Entry& entry : table)
        {
            u16(entry.id); u16(0u); u32(entry.offset); u32(entry.size);
        }
        out.insert(out.end(), stored.begin(), stored.end());
        return out;
    }

    std::vector<unsigned char> BuildCappedRaw(size_t payloadTarget, bool compress)
    {
        std::vector<unsigned char> meta;
        const std::string metaText = "publisher=CapBoundary\ntitle=Limit\n";
        meta.assign(metaText.begin(), metaText.end());
        std::vector<unsigned char> identity;
        const std::string identityText = "saltKnown=1\nsalt=\n";
        identity.assign(identityText.begin(), identityText.end());
        if (payloadTarget < meta.size() + identity.size()) return {};
        std::vector<unsigned char> filler(payloadTarget - meta.size() - identity.size(), 0u);
        std::vector<RawChunk> chunks{
            {0x0001u, meta},
            {0x0002u, identity},
            {0x0006u, filler},
        };
        // RULES 内容无所谓，DecodeArchive 会原样保留。
        return BuildRaw(chunks, compress);
    }

    void TestRoundTrip()
    {
        auto archive = MakeArchive(L"测试会社", L"游戏:①", ExeDigest(L'A'), L"盐值", L"roundtrip");
        const auto blob = EncodeOk(archive);
        Yua::ExtensionSet::Archive decoded;
        std::wstring error;
        const bool ok = Yua::ExtensionSet::DecodeArchive(blob, decoded, error);
        Check(ok, "roundtrip-decode", error);
        if (!ok) return;
        Check(decoded.meta.publisher == L"测试会社" && decoded.meta.title == L"游戏:①",
              "roundtrip-meta");
        Check(decoded.identity.salt == L"盐值" && decoded.identity.saltKnown,
              "roundtrip-salt");
        Check(decoded.directories.size() == 2u && decoded.files.size() == 2u,
              "roundtrip-counts");
        Check(decoded.lookupReady, "roundtrip-lookup");
    }

    void TestInvalidStored2MiB()
    {
        std::vector<unsigned char> blob(20u + 2u * 1024u * 1024u, 0xFFu);
        blob[0] = 'Y'; blob[1] = 'X'; blob[2] = 'S'; blob[3] = '1';
        blob[4] = 1; blob[5] = 0; blob[6] = 20; blob[7] = 0;
        std::memset(blob.data() + 8u, 0, 12u);
        Yua::ExtensionSet::Archive archive;
        std::wstring error;
        const DWORD start = ::GetTickCount();
        const bool ok = Yua::ExtensionSet::DecodeArchive(blob, archive, error);
        const DWORD elapsed = ::GetTickCount() - start;
        Check(!ok && !error.empty(), "stored-2mib-reject", error);
        Check(elapsed < 5000u, "stored-2mib-bounded-time",
              L"elapsed=" + FormatUInt(elapsed) + L"ms");

        std::vector<unsigned char> compressed(20u + 2u * 1024u * 1024u, 0xFFu);
        compressed[0] = 'Y'; compressed[1] = 'X'; compressed[2] = 'S'; compressed[3] = '1';
        compressed[4] = 1; compressed[5] = 0; compressed[6] = 20; compressed[7] = 0;
        std::memset(compressed.data() + 8u, 0, 12u);
        compressed[8] = 1;   // flags = zlib
        const bool ok2 = Yua::ExtensionSet::DecodeArchive(compressed, archive, error);
        Check(!ok2 && !error.empty(), "zlib-garbage-reject", error);
    }

    void TestZlibCapCorruption()
    {
        auto archive = MakeArchive(L"P", L"T", ExeDigest(L'B'), L"s", L"zlib");
        const auto good = EncodeOk(archive);

        std::vector<unsigned char> truncated = good;
        truncated.resize(truncated.size() - 5u);
        Yua::ExtensionSet::Archive decoded;
        std::wstring error;
        Check(!Yua::ExtensionSet::DecodeArchive(truncated, decoded, error),
              "zlib-truncated-reject", error);

        std::vector<unsigned char> badCrc = good;
        badCrc[12] ^= 0x5Au;
        Check(!Yua::ExtensionSet::DecodeArchive(badCrc, decoded, error),
              "crc-reject", error);

        // 64 MiB + 1 解压炸弹：压缩后很小，必须按 payload 上限拒绝。
        const auto bomb = BuildCappedRaw(kCap + 1u, true);
        Check(!bomb.empty(), "cap-bomb-built");
        Check(!Yua::ExtensionSet::DecodeArchive(bomb, decoded, error),
              "cap-bomb-reject", error);
        Check(error.find(L"上限") != std::wstring::npos
                  || error.find(L"超过") != std::wstring::npos,
              "cap-bomb-message", error);
    }

    void TestPayloadCapBoundary()
    {
        const auto atCap = BuildCappedRaw(kCap, true);
        Check(!atCap.empty(), "cap-boundary-built");
        Yua::ExtensionSet::Archive decoded;
        std::wstring error;
        const bool ok = Yua::ExtensionSet::DecodeArchive(atCap, decoded, error);
        Check(ok, "cap-boundary-accept-64mib", error);
    }

    void TestInstallLifecycle()
    {
        const std::wstring library = Combine(g_root, L"lifecycle-lib");
        auto archive1 = MakeArchive(L"会社A", L"作品A", ExeDigest(L'C'), L"salt1", L"v1");
        const auto blob1 = EncodeOk(archive1);
        std::wstring error;
        Check(Yua::ExtensionSet::InstallBytesToLibrary(library, blob1, error, L"test://v1"),
              "install-first", error);

        const auto catalog1 = Yua::ExtensionSet::EnumerateLibrary(library);
        Check(catalog1.ok && catalog1.entries.size() == 1u, "install-first-one-row",
              L"entries=" + FormatUInt(catalog1.entries.size()));

        const auto rules1Path = Combine(Combine(Combine(library, L"会社A"), L"作品A"), L"rules.int");
        std::vector<unsigned char> installed1;
        Check(ReadBytes(rules1Path, installed1) && installed1 == blob1, "install-first-bytes");

        error.clear();
        Check(Yua::ExtensionSet::InstallBytesToLibrary(library, blob1, error, L"test://v1"),
              "install-idempotent", error);
        const auto catalog2 = Yua::ExtensionSet::EnumerateLibrary(library);
        Check(catalog2.entries.size() == 1u, "install-idempotent-one-row",
              L"entries=" + FormatUInt(catalog2.entries.size()));

        auto archive2 = MakeArchive(L"会社A", L"作品A", ExeDigest(L'C'), L"salt1", L"v2-updated");
        const auto blob2 = EncodeOk(archive2);
        error.clear();
        Check(Yua::ExtensionSet::InstallBytesToLibrary(library, blob2, error, L"test://v2"),
              "install-update", error);
        const auto catalog3 = Yua::ExtensionSet::EnumerateLibrary(library);
        Check(catalog3.entries.size() == 1u, "install-update-one-row",
              L"entries=" + FormatUInt(catalog3.entries.size()));
        std::vector<unsigned char> installed2;
        Check(ReadBytes(rules1Path, installed2) && installed2 == blob2, "install-update-bytes");
    }

    void TestWriteFileAtomicHelper()
    {
#ifdef YUA_ATOMICFILE_TEST_SEAM
        const std::wstring dir = Combine(g_root, L"atomic-helper");
        MakeDirectoryTree(dir);
        const std::wstring target = Combine(dir, L"file.bin");
        const std::vector<unsigned char> oldBytes{'o', 'l', 'd'};
        const std::vector<unsigned char> newBytes{'n', 'e', 'w'};
        Check(WriteBytes(target, oldBytes), "helper-baseline");

        // 直接消费 helper：Commit 替换成功后抛 bad_alloc，helper 必须回滚并返回 false。
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::ThrowAfterReplace;
        std::wstring error;
        const bool throwOk = Common::AtomicFile::WriteFileAtomic(target, newBytes, error);
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::None;
        Check(!throwOk && !error.empty(), "helper-throw-fails", error);
        std::vector<unsigned char> afterThrow;
        Check(ReadBytes(target, afterThrow) && afterThrow == oldBytes, "helper-throw-restored");

        // ReplaceFile 半途（旧目标已进 backup、目标缺失）：helper 必须用 backup 恢复。
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::ReplacePartial;
        error.clear();
        const bool partialOk = Common::AtomicFile::WriteFileAtomic(target, newBytes, error);
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::None;
        Check(!partialOk && !error.empty(), "helper-partial-fails", error);
        std::vector<unsigned char> afterPartial;
        Check(ReadBytes(target, afterPartial) && afterPartial == oldBytes,
              "helper-partial-restored");
        const auto helperArtifacts = FindArtifacts(dir);
        Check(helperArtifacts.empty(), "helper-no-artifacts",
              helperArtifacts.empty() ? L"" : helperArtifacts.front());

        // 已完整提交后 Finalize 抛异常：helper 必须返回 true（数据已写入），并带清理提示。
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::ThrowDuringFinalize;
        error.clear();
        const bool finalizeThrowOk = Common::AtomicFile::WriteFileAtomic(target, newBytes, error);
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::None;
        std::vector<unsigned char> afterFinalizeThrow;
        Check(finalizeThrowOk && ReadBytes(target, afterFinalizeThrow) && afterFinalizeThrow == newBytes,
              "helper-finalize-throw-written");
        Check(!error.empty(), "helper-finalize-throw-hint", error);
        // 把目标恢复回 old，供后面的 rollback 失败用例使用。
        Check(WriteBytes(target, oldBytes), "helper-finalize-throw-reset");

        // 回滚本身失败：必须保留 backup，并在 error 里给出恢复路径。
        Common::AtomicFile::StagedFile staged;
        std::wstring stageError;
        Check(Common::AtomicFile::StageBytes(target, newBytes, staged, stageError),
              "helper-rollbackfail-stage", stageError);
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::ThrowAfterReplace;
        try
        {
            std::wstring commitError;
            Common::AtomicFile::Commit(staged, commitError);
        }
        catch (const std::bad_alloc&)
        {
        }
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::None;
        Check(staged.committed, "helper-rollbackfail-committed");
        HANDLE lock = ::CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(lock != INVALID_HANDLE_VALUE, "helper-rollbackfail-lock");
        std::wstring rollbackError;
        const bool rollbackOk = Common::AtomicFile::Rollback(staged, rollbackError);
        Check(!rollbackOk && staged.rollbackFailed && !staged.backup.empty()
                  && FileExists(staged.backup)
                  && rollbackError.find(staged.backup) != std::wstring::npos,
              "helper-rollbackfail-retains-backup", rollbackError);
        std::vector<unsigned char> backupBytes;
        Check(ReadBytes(staged.backup, backupBytes) && backupBytes == oldBytes,
              "helper-rollbackfail-backup-is-old");
        if (lock != INVALID_HANDLE_VALUE) ::CloseHandle(lock);

        // Discard 遇祖先 junction：只保留路径，不能顺着链接删外部同名 temp。
        const std::wstring discardDir = Combine(g_root, L"discard-ancestor");
        MakeDirectoryTree(discardDir);
        Common::AtomicFile::StagedFile discardStaged;
        std::wstring discardError;
        Check(Common::AtomicFile::StageBytes(Combine(discardDir, L"out.bin"), newBytes,
                                            discardStaged, discardError),
              "discard-stage", discardError);
        const std::wstring tempLeaf =
            discardStaged.temp.substr(discardStaged.temp.find_last_of(L"\\/") + 1u);
        const std::wstring external = Combine(g_root, L"discard-external");
        MakeDirectoryTree(external);
        const std::wstring externalTemp = Combine(external, tempLeaf);
        Check(WriteBytes(externalTemp, std::vector<unsigned char>{'x', 'y', 'z'}),
              "discard-external-sentinel");
        const std::wstring movedDir = discardDir + L"_moved";
        Check(::MoveFileExW(discardDir.c_str(), movedDir.c_str(), 0) != FALSE,
              "discard-dir-moved");
        const std::wstring discardLinkCommand =
            L"cmd /c mklink /J \"" + discardDir + L"\" \"" + external + L"\" >nul 2>&1";
        Check(::_wsystem(discardLinkCommand.c_str()) == 0, "discard-junction-created");
        Common::AtomicFile::Discard(discardStaged);
        Check(FileExists(externalTemp), "discard-external-temp-preserved");
        Check(!discardStaged.temp.empty() && discardStaged.stateUnknown,
              "discard-keeps-unknown-temp-path");
        ::RemoveDirectoryW(discardDir.c_str());
        ::MoveFileExW(movedDir.c_str(), discardDir.c_str(), 0);
#endif
    }

    void TestManifestCorruptionNoChange()
    {
        const std::wstring library = Combine(g_root, L"corrupt-lib");
        MakeDirectoryTree(library);
        const std::wstring manifest = Combine(library, L"manifest.txt");
        const std::wstring text =
            L"# 会社\t作品\t目标exe摘要\t兄弟exe摘要\t盐摘要\t档案摘要\t条数\t生成时间\t安装时间\t来源\r\n"
            L"坏行\t少了列\r\n"
            L"Good\tRow\t" + ExeDigest(L'D') + L"\t\t" + ExeDigest(L'E') + L"\t" + ExeDigest(L'F')
            + L"\t1\tsrc\tinst\tsource\r\n";
        const std::vector<unsigned char> bytes = TextBytes(text);
        Check(WriteBytes(manifest, bytes), "corrupt-manifest-written");
        const auto before = Snapshot(library);

        auto archive = MakeArchive(L"新会社", L"新作品", ExeDigest(L'1'), L"s", L"x");
        const auto blob = EncodeOk(archive);
        std::wstring error;
        Check(!Yua::ExtensionSet::InstallBytesToLibrary(library, blob, error, L"test://x"),
              "corrupt-manifest-refused", error);
        const auto after = Snapshot(library);
        Check(before == after, "corrupt-manifest-no-disk-change",
              L"before=" + FormatUInt(before.size()) + L" after=" + FormatUInt(after.size()));

        // manifest.txt 是同名目录：不能当成"清单不存在"然后重建空清单。
        const std::wstring dirLibrary = Combine(g_root, L"manifest-dir-lib");
        const std::wstring dirManifest = Combine(dirLibrary, L"manifest.txt");
        MakeDirectoryTree(dirManifest);
        const std::wstring dirSentinel = Combine(dirManifest, L"keep.txt");
        Check(WriteBytes(dirSentinel, std::vector<unsigned char>{'d'}), "manifest-dir-sentinel");
        const auto dirBefore = Snapshot(dirLibrary);
        auto dirArchive = MakeArchive(L"目录会社", L"目录作品", ExeDigest(L'2'), L"s", L"dir");
        const auto dirBlob = EncodeOk(dirArchive);
        std::wstring dirError;
        Check(!Yua::ExtensionSet::InstallBytesToLibrary(dirLibrary, dirBlob, dirError, L"dir"),
              "manifest-dir-refused", dirError);
        Check(FileExists(dirSentinel) && Snapshot(dirLibrary) == dirBefore,
              "manifest-dir-no-change");
    }

    void TestManifestLockRollback()
    {
        const std::wstring library = Combine(g_root, L"lock-lib");
        auto archive1 = MakeArchive(L"锁会社", L"锁作品", ExeDigest(L'G'), L"salt", L"old");
        const auto blob1 = EncodeOk(archive1);
        std::wstring error;
        Check(Yua::ExtensionSet::InstallBytesToLibrary(library, blob1, error, L"test://old"),
              "lock-install-base", error);

        const std::wstring slot = Combine(Combine(library, L"锁会社"), L"锁作品");
        const std::wstring rulesPath = Combine(slot, L"rules.int");
        const std::wstring infoPath = Combine(slot, L"info.txt");
        const std::wstring manifestPath = Combine(library, L"manifest.txt");
        std::vector<unsigned char> rulesBefore, infoBefore, manifestBefore;
        Check(ReadBytes(rulesPath, rulesBefore) && ReadBytes(infoPath, infoBefore)
                  && ReadBytes(manifestPath, manifestBefore),
              "lock-read-baseline");
        const std::wstring rulesHash = Yua::ExtensionSet::Sha256HexBytes(rulesBefore.data(), rulesBefore.size());
        const std::wstring infoHash = Yua::ExtensionSet::Sha256HexBytes(infoBefore.data(), infoBefore.size());
        const std::wstring manifestHash = Yua::ExtensionSet::Sha256HexBytes(manifestBefore.data(), manifestBefore.size());
        unsigned long long rulesMtime = 0u, infoMtime = 0u, manifestMtime = 0u;
        Check(LastWriteTime(rulesPath, rulesMtime) && LastWriteTime(infoPath, infoMtime)
                  && LastWriteTime(manifestPath, manifestMtime),
              "lock-time-baseline");

        // 故意只给 READ 共享：后续 ReplaceFileW 需要的 DELETE/WRITE 会被拒。
        HANDLE lock = ::CreateFileW(manifestPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(lock != INVALID_HANDLE_VALUE, "manifest-lock-open");

        auto archive2 = MakeArchive(L"锁会社", L"锁作品", ExeDigest(L'G'), L"salt", L"new-bytes");
        const auto blob2 = EncodeOk(archive2);
        error.clear();
        const bool installed = Yua::ExtensionSet::InstallBytesToLibrary(library, blob2, error, L"test://new");
        Check(!installed && !error.empty(), "manifest-lock-install-fails", error);
        if (lock != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(lock);
        }

        std::vector<unsigned char> rulesAfter, infoAfter, manifestAfter;
        Check(ReadBytes(rulesPath, rulesAfter) && ReadBytes(infoPath, infoAfter)
                  && ReadBytes(manifestPath, manifestAfter),
              "lock-read-after");
        Check(Yua::ExtensionSet::Sha256HexBytes(rulesAfter.data(), rulesAfter.size()) == rulesHash,
              "lock-rules-preserved");
        Check(Yua::ExtensionSet::Sha256HexBytes(infoAfter.data(), infoAfter.size()) == infoHash,
              "lock-info-preserved");
        Check(Yua::ExtensionSet::Sha256HexBytes(manifestAfter.data(), manifestAfter.size()) == manifestHash,
              "lock-manifest-preserved");
        unsigned long long rulesMtimeAfter = 0u, infoMtimeAfter = 0u, manifestMtimeAfter = 0u;
        Check(LastWriteTime(rulesPath, rulesMtimeAfter) && rulesMtimeAfter == rulesMtime,
              "lock-rules-mtime-preserved",
              L"before=" + FormatUInt(rulesMtime) + L" after=" + FormatUInt(rulesMtimeAfter));
        Check(LastWriteTime(infoPath, infoMtimeAfter) && infoMtimeAfter == infoMtime,
              "lock-info-mtime-preserved",
              L"before=" + FormatUInt(infoMtime) + L" after=" + FormatUInt(infoMtimeAfter));
        Check(LastWriteTime(manifestPath, manifestMtimeAfter) && manifestMtimeAfter == manifestMtime,
              "lock-manifest-mtime-preserved");
        const auto artifacts = FindArtifacts(library);
        Check(artifacts.empty(), "lock-no-temp-or-backup",
              artifacts.empty() ? L"" : artifacts.front());

        // 另一个更容易留垃圾的路径：失败目标是**全新槽位**（目标目录也是本次新建的）。
        // 回滚后 rules/info 删除、自己新建的空目录反向收回，只留原有 manifest/根。
        const std::wstring freshLibrary = Combine(g_root, L"lock-fresh-lib");
        auto baseArchive = MakeArchive(L"基础会社", L"基础作品", ExeDigest(L'H'), L"s", L"base");
        const auto baseBlob = EncodeOk(baseArchive);
        std::wstring baseError;
        Check(Yua::ExtensionSet::InstallBytesToLibrary(freshLibrary, baseBlob, baseError, L"base"),
              "lock-fresh-base", baseError);
        const std::wstring freshManifest = Combine(freshLibrary, L"manifest.txt");
        HANDLE freshLock = ::CreateFileW(freshManifest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(freshLock != INVALID_HANDLE_VALUE, "lock-fresh-open");
        auto newArchive = MakeArchive(L"全新会社", L"全新作品", ExeDigest(L'I'), L"s", L"brand-new");
        const auto newBlob = EncodeOk(newArchive);
        std::wstring newError;
        Check(!Yua::ExtensionSet::InstallBytesToLibrary(freshLibrary, newBlob, newError, L"new"),
              "lock-fresh-install-fails", newError);
        if (freshLock != INVALID_HANDLE_VALUE) ::CloseHandle(freshLock);
        Check(!DirectoryExists(Combine(freshLibrary, L"全新会社")),
              "lock-fresh-created-dirs-rolled-back");
        const auto freshArtifacts = FindArtifacts(freshLibrary);
        Check(freshArtifacts.empty(), "lock-fresh-no-temp-or-backup",
              freshArtifacts.empty() ? L"" : freshArtifacts.front());
    }

    void TestLibraryReparseRejected()
    {
        const std::wstring external = Combine(g_root, L"lib-external");
        MakeDirectoryTree(external);
        const std::wstring sentinel = Combine(external, L"sentinel.bin");
        Check(WriteBytes(sentinel, std::vector<unsigned char>{'s', 'e', 'n', 't'}),
              "lib-reparse-sentinel");
        const auto before = Snapshot(external);

        const std::wstring linkedLibrary = Combine(g_root, L"lib-junction");
        const std::wstring command =
            L"cmd /c mklink /J \"" + linkedLibrary + L"\" \"" + external + L"\" >nul 2>&1";
        const int linkResult = ::_wsystem(command.c_str());
        Check(linkResult == 0 && DirectoryExists(linkedLibrary), "lib-reparse-junction-created");

        auto archive = MakeArchive(L"链接会社", L"链接作品", ExeDigest(L'J'), L"s", L"junction");
        const auto blob = EncodeOk(archive);
        std::wstring error;
        Check(!Yua::ExtensionSet::InstallBytesToLibrary(linkedLibrary, blob, error, L"junction"),
              "lib-reparse-install-refused", error);
        Check(Snapshot(external) == before, "lib-reparse-external-unchanged");
        Check(FileExists(sentinel), "lib-reparse-sentinel-preserved");
        ::RemoveDirectoryW(linkedLibrary.c_str());   // 只摘链接
    }

    void TestDryRunReparse()
    {
        const std::wstring salt = L"dry-salt";
        auto archive = MakeArchive(L"扫描会社", L"扫描作品", ExeDigest(L'K'), salt, L"dry");
        std::wstring lookupError;
        Check(Yua::ExtensionSet::RebuildLookup(archive, lookupError),
              "dryrun-lookup", lookupError);

        const std::wstring dirHash = Yua::ExtensionSet::DomainHashHex(L"system/", salt);
        const std::wstring fileHash = Yua::ExtensionSet::FileHashHex(L"a.txt", salt);
        const std::wstring source = Combine(g_root, L"dry-source");
        const std::wstring normalDir = Combine(source, dirHash);
        MakeDirectoryTree(normalDir);
        Check(WriteBytes(Combine(normalDir, fileHash + L".png"),
                         std::vector<unsigned char>{1, 2, 3}),
              "dryrun-normal-file");

        const std::wstring external = Combine(g_root, L"dry-external");
        const std::wstring externalDir = Combine(external, dirHash);
        MakeDirectoryTree(externalDir);
        Check(WriteBytes(Combine(externalDir, fileHash + L".png"),
                         std::vector<unsigned char>{4, 5, 6}),
              "dryrun-external-file");
        const std::wstring junction = Combine(source, L"linked");
        const std::wstring command =
            L"cmd /c mklink /J \"" + junction + L"\" \"" + external + L"\" >nul 2>&1";
        Check(::_wsystem(command.c_str()) == 0 && DirectoryExists(junction),
              "dryrun-junction-created");

        Yua::ExtensionSet::DryRunOptions options;
        options.sourceDirectory = source;
        const auto dry = Yua::ExtensionSet::DryRun(archive, options);
        Check(dry.ok, "dryrun-ok", dry.error);
        Check(dry.skippedReparsePoints == 1u, "dryrun-reparse-skipped",
              L"skipped=" + FormatUInt(dry.skippedReparsePoints));
        Check(dry.totalFiles == 1u, "dryrun-no-follow-count",
              L"total=" + FormatUInt(dry.totalFiles));

        // 来源根本身是 junction：拒绝跟随，不能报"全部成功"。
        const std::wstring sourceLink = Combine(g_root, L"dry-source-link");
        const std::wstring linkCommand =
            L"cmd /c mklink /J \"" + sourceLink + L"\" \"" + external + L"\" >nul 2>&1";
        Check(::_wsystem(linkCommand.c_str()) == 0, "dryrun-root-junction-created");
        Yua::ExtensionSet::DryRunOptions badOptions;
        badOptions.sourceDirectory = sourceLink;
        const auto badDry = Yua::ExtensionSet::DryRun(archive, badOptions);
        Check(!badDry.ok && !badDry.error.empty(), "dryrun-root-reparse-rejected", badDry.error);
        ::RemoveDirectoryW(sourceLink.c_str());
    }

    void TestAtomicFileSeams()
    {
#ifdef YUA_ATOMICFILE_TEST_SEAM
        const std::wstring library = Combine(g_root, L"atomic-seam-lib");
        auto baseArchive = MakeArchive(L"原子会社", L"原子作品", ExeDigest(L'R'), L"s", L"base");
        const auto baseBlob = EncodeOk(baseArchive);
        std::wstring error;
        Check(Yua::ExtensionSet::InstallBytesToLibrary(library, baseBlob, error, L"base"),
              "atomic-seam-base", error);
        const std::wstring slot = Combine(Combine(library, L"原子会社"), L"原子作品");
        const std::wstring rulesPath = Combine(slot, L"rules.int");
        const std::wstring infoPath = Combine(slot, L"info.txt");
        const std::wstring manifestPath = Combine(library, L"manifest.txt");
        std::vector<unsigned char> rules0, info0, manifest0;
        Check(ReadBytes(rulesPath, rules0) && ReadBytes(infoPath, info0)
                  && ReadBytes(manifestPath, manifest0),
              "atomic-seam-baseline");
        const std::wstring rulesHash = Yua::ExtensionSet::Sha256HexBytes(rules0.data(), rules0.size());
        const std::wstring infoHash = Yua::ExtensionSet::Sha256HexBytes(info0.data(), info0.size());
        const std::wstring manifestHash =
            Yua::ExtensionSet::Sha256HexBytes(manifest0.data(), manifest0.size());

        // ReplaceFile 半途：旧目标被挪到 backup、目标缺失；Rollback 必须用 backup 恢复。
        auto partialArchive = MakeArchive(L"原子会社", L"原子作品", ExeDigest(L'R'), L"s", L"partial");
        const auto partialBlob = EncodeOk(partialArchive);
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::ReplacePartial;
        std::wstring partialError;
        const bool partialInstalled =
            Yua::ExtensionSet::InstallBytesToLibrary(library, partialBlob, partialError, L"partial");
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::None;
        Check(!partialInstalled && !partialError.empty(), "atomic-seam-partial-fails", partialError);
        std::vector<unsigned char> rules1, info1, manifest1;
        Check(ReadBytes(rulesPath, rules1) && ReadBytes(infoPath, info1)
                  && ReadBytes(manifestPath, manifest1),
              "atomic-seam-partial-readable");
        Check(Yua::ExtensionSet::Sha256HexBytes(rules1.data(), rules1.size()) == rulesHash,
              "atomic-seam-partial-rules-restored");
        Check(Yua::ExtensionSet::Sha256HexBytes(info1.data(), info1.size()) == infoHash,
              "atomic-seam-partial-info-restored");
        Check(Yua::ExtensionSet::Sha256HexBytes(manifest1.data(), manifest1.size()) == manifestHash,
              "atomic-seam-partial-manifest-restored");
        const auto partialArtifacts = FindArtifacts(library);
        Check(partialArtifacts.empty(), "atomic-seam-partial-no-artifacts",
              partialArtifacts.empty() ? L"" : partialArtifacts.front());

        // Replace 成功后抛 bad_alloc：Install catch 必须回滚已提交项，不能留半安装。
        auto throwArchive = MakeArchive(L"原子会社", L"原子作品", ExeDigest(L'R'), L"s", L"throw");
        const auto throwBlob = EncodeOk(throwArchive);
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::ThrowAfterReplace;
        std::wstring throwError;
        const bool throwInstalled =
            Yua::ExtensionSet::InstallBytesToLibrary(library, throwBlob, throwError, L"throw");
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::None;
        Check(!throwInstalled && !throwError.empty(), "atomic-seam-throw-caught", throwError);
        std::vector<unsigned char> rules2, info2, manifest2;
        Check(ReadBytes(rulesPath, rules2) && ReadBytes(infoPath, info2)
                  && ReadBytes(manifestPath, manifest2),
              "atomic-seam-throw-readable");
        Check(Yua::ExtensionSet::Sha256HexBytes(rules2.data(), rules2.size()) == rulesHash,
              "atomic-seam-throw-rules-restored");
        Check(Yua::ExtensionSet::Sha256HexBytes(info2.data(), info2.size()) == infoHash,
              "atomic-seam-throw-info-restored");
        Check(Yua::ExtensionSet::Sha256HexBytes(manifest2.data(), manifest2.size()) == manifestHash,
              "atomic-seam-throw-manifest-restored");
        const auto throwArtifacts = FindArtifacts(library);
        Check(throwArtifacts.empty(), "atomic-seam-throw-no-artifacts",
              throwArtifacts.empty() ? L"" : throwArtifacts.front());

        // 完整 commit 后 Finalize 抛异常：安装必须返回 true（数据已装好），只带清理提示。
        auto finalizeThrowArchive =
            MakeArchive(L"原子会社", L"原子作品", ExeDigest(L'R'), L"s", L"finalize-throw");
        const auto finalizeThrowBlob = EncodeOk(finalizeThrowArchive);
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::ThrowDuringFinalize;
        std::wstring finalizeThrowError;
        const bool finalizeThrowInstalled = Yua::ExtensionSet::InstallBytesToLibrary(
            library, finalizeThrowBlob, finalizeThrowError, L"finalize-throw");
        Common::AtomicFile::TestActionRef() = Common::AtomicFile::TestAction::None;
        Check(finalizeThrowInstalled, "atomic-seam-finalize-throw-install-true",
              finalizeThrowError);
        Check(!finalizeThrowError.empty(), "atomic-seam-finalize-throw-hint", finalizeThrowError);
#endif
    }

    void TestDownloadStagingFailure()
    {
#ifdef YUA_SECURITY_EXTENSION_TEST_SEAM
        Yua::Host::g_toolDirectory = Combine(g_root, L"tool-download");
        MakeDirectoryTree(Yua::Host::g_toolDirectory);
        const std::wstring base = Combine(Combine(Yua::Host::g_toolDirectory, L"Log"),
                                          L"OnlineStaging");
        const int before = CountSessionDirectories(base);

        Yua::ExtensionSet::LibraryEntry entry;
        entry.publisher = L"下载会社";
        entry.title = L"下载作品";
        entry.exeDigest = ExeDigest(L'X');
        entry.archiveDigest = ExeDigest(L'Y');
        entry.entryCount = 1u;

        Yua::Online::TestFailDownloadAfterStaging() = true;
        Yua::Online::DownloadResult result;
        std::wstring error;
        const bool ok = Yua::Online::DownloadArchive(L"https://example.invalid/", entry, L"",
                                                     result, error);
        Yua::Online::TestFailDownloadAfterStaging() = false;

        Check(!ok && !error.empty(), "download-staging-fail-returned", error);
        Check(CountSessionDirectories(base) == before, "download-staging-fail-cleaned",
              L"before=" + FormatUInt(static_cast<unsigned long long>(before))
                  + L" after=" + FormatUInt(static_cast<unsigned long long>(
                                    CountSessionDirectories(base))));
#endif
    }

    void TestStagingAncestorJunction()
    {
        Yua::Host::g_toolDirectory = Combine(g_root, L"tool-ancestor");
        MakeDirectoryTree(Yua::Host::g_toolDirectory);
        const std::wstring session = Yua::Online::DefaultStagingDirectory();
        Check(!session.empty() && DirectoryExists(session), "staging-ancestor-session", session);
        if (session.empty()) return;
        Check(WriteBytes(Combine(session, L"own.txt"), std::vector<unsigned char>{9}),
              "staging-ancestor-own-file");

        const std::wstring leaf = session.substr(session.find_last_of(L"\\/") + 1u);
        const std::wstring base = ParentOf(session);
        const std::wstring movedBase = base + L"_moved";
        Check(::MoveFileExW(base.c_str(), movedBase.c_str(), 0) != FALSE,
              "staging-ancestor-base-moved");

        const std::wstring external = Combine(g_root, L"staging-external-ancestor");
        const std::wstring externalLeaf = Combine(external, leaf);
        MakeDirectoryTree(externalLeaf);
        const std::wstring externalSentinel = Combine(externalLeaf, L"keep.txt");
        Check(WriteBytes(externalSentinel, std::vector<unsigned char>{'k'}), "staging-ancestor-sentinel");
        const std::wstring linkCommand =
            L"cmd /c mklink /J \"" + base + L"\" \"" + external + L"\" >nul 2>&1";
        Check(::_wsystem(linkCommand.c_str()) == 0, "staging-ancestor-junction-created");

        // 祖先被换成 junction：清理必须拒绝穿过链接，登记保留、外部 sentinel 不动。
        Yua::Online::CleanupStaging(session);
        Check(FileExists(externalSentinel), "staging-ancestor-external-sentinel-preserved");
        Check(DirectoryExists(Combine(movedBase, leaf)), "staging-ancestor-original-preserved");

        // 复原路径后，同一登记仍应可清理 —— 证明没有"假干净地 unregister"。
        ::RemoveDirectoryW(base.c_str());
        Check(::MoveFileExW(movedBase.c_str(), base.c_str(), 0) != FALSE,
              "staging-ancestor-base-restored");
        Yua::Online::CleanupStaging(session);
        Check(!DirectoryExists(session), "staging-ancestor-clean-after-restore");
    }

    void TestClassifySlotSanitize()
    {
        Yua::ExtensionSet::LibraryCatalog local;
        Yua::ExtensionSet::LibraryEntry localEntry;
        localEntry.publisher = L"Game:A";
        localEntry.title = L"Title";
        localEntry.exeDigest = ExeDigest(L'9');
        localEntry.entryCount = 10u;
        local.entries.push_back(localEntry);

        Yua::ExtensionSet::LibraryEntry remote;
        remote.publisher = L"GameA";
        remote.title = L"Title";
        remote.exeDigest = ExeDigest(L'8');
        remote.entryCount = 10u;
        const auto stateSameCount = Yua::Online::Classify(remote, local);
        Check(stateSameCount == Yua::Online::EntryState::Update, "classify-sanitized-slot-update",
              Yua::Online::DescribeState(stateSameCount));

        remote.entryCount = 5u;
        const auto stateOlder = Yua::Online::Classify(remote, local);
        Check(stateOlder == Yua::Online::EntryState::LocalNewer, "classify-sanitized-slot-localnewer",
              Yua::Online::DescribeState(stateOlder));

        // 不同 exe、不同槽位才是 New。
        remote.publisher = L"Other";
        remote.title = L"Other";
        remote.exeDigest = ExeDigest(L'7');
        const auto stateNew = Yua::Online::Classify(remote, local);
        Check(stateNew == Yua::Online::EntryState::New, "classify-different-new",
              Yua::Online::DescribeState(stateNew));
    }

    void TestConcurrentWriters()
    {
        const std::wstring library = Combine(g_root, L"concurrent-lib");
        auto a1 = MakeArchive(L"并发A", L"作品1", ExeDigest(L'1'), L"s1", L"a");
        auto a2 = MakeArchive(L"并发B", L"作品2", ExeDigest(L'2'), L"s2", L"b");
        const auto blob1 = EncodeOk(a1);
        const auto blob2 = EncodeOk(a2);
        std::atomic<int> okCount{0};
        std::wstring e1, e2;
        std::thread t1([&]() {
            std::wstring error;
            if (Yua::ExtensionSet::InstallBytesToLibrary(library, blob1, error, L"t1")) ++okCount;
            else e1 = error;
        });
        std::thread t2([&]() {
            std::wstring error;
            if (Yua::ExtensionSet::InstallBytesToLibrary(library, blob2, error, L"t2")) ++okCount;
            else e2 = error;
        });
        t1.join();
        t2.join();
        Check(okCount.load() == 2, "concurrent-both-succeed",
              e1.empty() ? e2 : e1);
        const auto catalog = Yua::ExtensionSet::EnumerateLibrary(library);
        Check(catalog.entries.size() == 2u, "concurrent-manifest-no-loss",
              L"entries=" + FormatUInt(catalog.entries.size()));
    }

    void TestByteBinding()
    {
        const std::wstring library = Combine(g_root, L"bytes-lib");
        auto archiveA = MakeArchive(L"字节会社", L"字节作品", ExeDigest(L'A'), L"s", L"bytes-A");
        const auto blobA = EncodeOk(archiveA);
        auto entry = MakeEntry(archiveA, blobA);
        std::wstring detail, error;
        Check(Yua::Online::VerifyArchiveBytes(blobA, entry, detail, error),
              "verify-bytes-A", error);

        // 模拟"校验之后 path 被替换成 B"：安装只拿 A 的 bytes，不允许重开 path。
        auto archiveB = MakeArchive(L"字节会社", L"字节作品", ExeDigest(L'B'), L"s", L"bytes-B");
        const auto blobB = EncodeOk(archiveB);
        const std::wstring replacedPath = Combine(g_root, L"replaced.rules.int");
        Check(WriteBytes(replacedPath, blobB), "bytes-path-replaced");
        error.clear();
        Check(Yua::ExtensionSet::InstallBytesToLibrary(library, blobA, error, L"test://A"),
              "verify-bytes-install-A", error);
        const std::wstring installed =
            Combine(Combine(Combine(library, L"字节会社"), L"字节作品"), L"rules.int");
        std::vector<unsigned char> actual;
        Check(ReadBytes(installed, actual)
                  && Yua::ExtensionSet::Sha256HexBytes(actual.data(), actual.size())
                         == Yua::ExtensionSet::Sha256HexBytes(blobA.data(), blobA.size()),
              "verify-bytes-installed-A-not-B");

        // 篡改后字节校验必须失败
        std::vector<unsigned char> tampered = blobA;
        tampered[0] ^= 0x01;
        detail.clear();
        error.clear();
        Check(!Yua::Online::VerifyArchiveBytes(tampered, entry, detail, error),
              "verify-bytes-tampered-reject", error);
    }

    void TestStagingCleanup()
    {
        Yua::Host::g_toolDirectory = Combine(g_root, L"tool");
        MakeDirectoryTree(Yua::Host::g_toolDirectory);

        const std::wstring external = Combine(g_root, L"external-sentinel");
        MakeDirectoryTree(external);
        const std::wstring extFile = Combine(external, L"keep.txt");
        Check(WriteBytes(extFile, std::vector<unsigned char>{'k', 'e', 'e', 'p'}),
              "staging-external-sentinel-written");

        const std::wstring staging = Yua::Online::DefaultStagingDirectory();
        Check(!staging.empty() && DirectoryExists(staging), "staging-session-created", staging);
        Check(WriteBytes(Combine(staging, L"own.tmp"), std::vector<unsigned char>{1, 2, 3}),
              "staging-own-file-written");

        const std::wstring junction = Combine(staging, L"link_to_external");
        const std::wstring command = L"cmd /c mklink /J \"" + junction + L"\" \"" + external + L"\" >nul 2>&1";
        const int linkResult = ::_wsystem(command.c_str());
        Check(linkResult == 0 && DirectoryExists(junction), "staging-junction-created",
              L"code=" + FormatUInt(static_cast<unsigned long long>(linkResult)));

        Yua::Online::CleanupStaging(staging);
        Check(!DirectoryExists(staging), "staging-owned-session-removed");
        Check(FileExists(extFile), "staging-junction-target-preserved");
        Check(DirectoryExists(external), "staging-junction-target-dir-preserved");

        // 未登记 explicit 目录：必须拒绝清理，sentinel 原样保留。
        const std::wstring unowned = Combine(g_root, L"unowned-staging");
        MakeDirectoryTree(unowned);
        const std::wstring unownedFile = Combine(unowned, L"sentinel.txt");
        Check(WriteBytes(unownedFile, std::vector<unsigned char>{'u'}), "staging-unowned-written");
        Yua::Online::CleanupStaging(unowned);
        Check(FileExists(unownedFile) && DirectoryExists(unowned),
              "staging-unowned-refused");

        // 空串 no-op（不能去删任何共享默认根）。
        const auto toolBefore = Snapshot(Yua::Host::g_toolDirectory);
        Yua::Online::CleanupStaging(L"");
        const auto toolAfter = Snapshot(Yua::Host::g_toolDirectory);
        Check(toolBefore == toolAfter && DirectoryExists(Combine(Yua::Host::g_toolDirectory, L"Log")),
              "staging-empty-noop");
    }

    void CleanupRoot()
    {
        // 测试自己的临时根：只删本测试创建的目录树（与生产清理无关）。
        const std::wstring command = L"cmd /c rmdir /s /q \"" + g_root + L"\"";
        ::_wsystem(command.c_str());
    }
}

int main()
{
    const std::wstring temp = [] {
        wchar_t buffer[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, buffer);
        return std::wstring(buffer);
    }();
    g_root = Combine(temp, L"YuaExtSecurityExt_" + FormatUInt(::GetCurrentProcessId()) + L"_"
                                 + FormatUInt(::GetTickCount64()));
    if (!MakeDirectoryTree(g_root))
    {
        std::printf("SEC-EXT FAIL setup-temp-root\n");
        return 2;
    }

    TestRoundTrip();
    Check(AtomicFileHeaderSelfCheck() == 0, "atomicfile-header-selfcheck");
    TestInvalidStored2MiB();
    TestZlibCapCorruption();
    TestPayloadCapBoundary();
    TestInstallLifecycle();
    TestManifestCorruptionNoChange();
    TestManifestLockRollback();
    TestAtomicFileSeams();
    TestWriteFileAtomicHelper();
    TestDownloadStagingFailure();
    TestStagingAncestorJunction();
    TestLibraryReparseRejected();
    TestDryRunReparse();
    TestClassifySlotSanitize();
    TestConcurrentWriters();
    TestByteBinding();
    TestStagingCleanup();

    CleanupRoot();
    std::printf("SEC-EXT SUMMARY checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
