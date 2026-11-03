// security-core-restorer-test
// 白盒回归：直接 include 生产实现 HashNameRestorer.cpp，调用真实匿名命名空间函数
// （WriteMinedNamesList / RestoreSinglePackage / LoadHashMapTables）。
// 只使用命令行给出的一次性临时根；不运行 GUI、不访问网络。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../../CxdecExtractorLoader/HashNameRestorer.cpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", label);
    } else {
        std::printf("ok   %s\n", label);
    }
}

void Check(bool condition, const wchar_t* label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %ls\n", label);
    } else {
        std::printf("ok   %ls\n", label);
    }
}


bool MakeDir(const std::wstring& path) {
    return ::CreateDirectoryW(path.c_str(), nullptr) != FALSE || ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
    ::CloseHandle(file);
    return ok && written == static_cast<DWORD>(size);
}

bool WriteText(const std::wstring& path, const std::string& text) {
    return WriteBytes(path, text.data(), text.size());
}

bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (1 << 20)) {
        ::CloseHandle(file);
        return false;
    }
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = out.empty() || ::ReadFile(file, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    ::CloseHandle(file);
    return ok && read == out.size();
}

std::string Utf8(const std::wstring& text) {
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out(need > 0 ? (size_t)need : 0, '\0');
    if (need > 0) ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], need, nullptr, nullptr);
    return out;
}

constexpr const wchar_t* kDirHash1 = L"0123456789ABCDEF";
constexpr const wchar_t* kFileHash1 = L"0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
constexpr const wchar_t* kDirHash2 = L"1111111111111111";
constexpr const wchar_t* kFileHash2 = L"1111111111111111111111111111111111111111111111111111111111111111";
constexpr const wchar_t* kDirHash3 = L"2222222222222222";
constexpr const wchar_t* kFileHash3 = L"2222222222222222222222222222222222222222222222222222222222222222";

std::wstring HashPath(const std::wstring& root, const wchar_t* dir, const wchar_t* file, const char* content) {
    const std::wstring directory = root + L"\\" + dir;
    MakeDir(directory);
    const std::wstring path = directory + L"\\" + file;
    WriteText(path, content);
    return path;
}

bool CreateJunction(const std::wstring& link, const std::wstring& target) {
    std::wstring command = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul 2>nul";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    if (!::CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &startup, &process)) {
        return false;
    }
    ::WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 1;
    ::GetExitCodeProcess(process.hProcess, &code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return code == 0 && ::GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES;
}

unsigned int CountFilesRecursive(const std::wstring& root) {
    unsigned int count = 0;
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW((root + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    for (;;) {
        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                count += CountFilesRecursive(root + L"\\" + name);
            } else {
                ++count;
            }
        }
        if (!::FindNextFileW(find, &data)) break;
    }
    ::FindClose(find);
    return count;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: security-core-restorer-test <scratch-root>\n");
        return 2;
    }
    const std::wstring root = argv[1];
    MakeDir(root);
    const std::wstring src = root + L"\\src";
    const std::wstring out = root + L"\\out";
    MakeDir(src);
    MakeDir(out);
    const std::wstring pack = src + L"\\pack";
    MakeDir(pack);

    HashPath(pack, kDirHash1, kFileHash1, "hello");
    HashPath(pack, kDirHash2, kFileHash2, "root");
    HashPath(pack, kDirHash3, kFileHash3, "chinese");

    Yua::HashMapTables tables;
    tables.directories[kDirHash1] = L"bgimage";
    tables.files[kFileHash1] = L"bgimage/cg01.png";
    tables.directories[kDirHash2] = L"";
    tables.files[kFileHash2] = L"root.txt";
    tables.directories[kDirHash3] = L"中文目录";
    tables.files[kFileHash3] = L"名字.png";

    Yua::RestoreOptions options;
    options.sourceDirectory = src;
    options.outputDirectory = out;

    Yua::RestorePackageTask task;
    task.packageName = L"pack";
    task.packageDirectory = pack;

    Yua::RestorePackageResult result = Yua::RestoreSinglePackage(task, tables, options);
    Check(result.succeeded && result.failedFiles == 0, L"normal restore succeeds");
    Check(::GetFileAttributesW((out + L"\\pack\\bgimage\\cg01.png").c_str()) != INVALID_FILE_ATTRIBUTES,
          L"mapped hierarchy preserved: pack\\bgimage\\cg01.png");
    Check(::GetFileAttributesW((out + L"\\pack\\bgimage\\bgimage").c_str()) == INVALID_FILE_ATTRIBUTES,
          L"no doubled directory");
    Check(::GetFileAttributesW((out + L"\\pack\\root.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
          L"empty root domain maps to package root");
    Check(::GetFileAttributesW((out + L"\\pack\\中文目录\\名字.png").c_str()) != INVALID_FILE_ATTRIBUTES,
          L"chinese directory and name");

    // ---- 恶意映射在载入时被拒绝（来源统计可见）----
    const std::wstring mapDir = root + L"\\maps";
    MakeDir(mapDir);
    {
        std::string log;
        log += "\xEF\xBB\xBF";
        const std::wstring line = std::wstring(L"..\\..\\escape##YSig##") + kFileHash1 + L"\r\n";
        log += Utf8(line);
        const std::wstring line2 = std::wstring(L"C:\\absolute##YSig##") + kDirHash1 + L"\r\n";
        log += Utf8(line2);
        WriteText(mapDir + L"\\FileNameHash.log", log);
        WriteText(mapDir + L"\\DirectoryHash.log", "");
    }
    Yua::HashMapTables loaded = Yua::LoadHashMapTables(mapDir);
    Check(loaded.rejectedEntries >= 2u, L"malicious mappings counted as rejected");
    Check(loaded.files.find(kFileHash1) == loaded.files.end(), L"traversal file mapping not accepted");
    Check(loaded.directories.find(kDirHash1) == loaded.directories.end(), L"absolute dir mapping not accepted");

    // ---- RestoreSinglePackage 的第二道防线 ----
    Yua::HashMapTables evil;
    evil.directories[kDirHash1] = L"..\\..\\..\\outside";
    evil.files[kFileHash1] = L"escape.txt";
    const std::wstring sentinel = root + L"\\sentinel.txt";
    WriteText(sentinel, "sentinel");
    Yua::RestoreOptions evilOptions;
    evilOptions.sourceDirectory = src;
    evilOptions.outputDirectory = out;
    Yua::RestorePackageResult evilResult = Yua::RestoreSinglePackage(task, evil, evilOptions);
    Check(evilResult.failedFiles >= 1u, L"traversal mapping fails per-file");
    Check(::GetFileAttributesW((root + L"\\outside\\escape.txt").c_str()) == INVALID_FILE_ATTRIBUTES,
          L"nothing escaped output root");
    std::string sentinelBytes;
    Check(ReadAll(sentinel, sentinelBytes) && sentinelBytes == "sentinel", L"outside sentinel unchanged");

    // ---- 源/输出同源或嵌套拒绝 ----
    Yua::RestoreOptions overlap = options;
    overlap.outputDirectory = src + L"\\nested";
    Yua::RestorePackageResult overlapResult = Yua::RestoreSinglePackage(task, tables, overlap);
    Check(!overlapResult.succeeded && overlapResult.completed, L"nested output root rejected");
    Check(::GetFileAttributesW((src + L"\\nested").c_str()) == INVALID_FILE_ATTRIBUTES,
          L"nested reject did not create output dir under source");

    // 尾点/尾空白与源同义：Win32 会折叠，必须建目录前拒绝且源目录文件数不变
    const unsigned int srcFilesBefore = CountFilesRecursive(src);
    {
        Yua::RestoreOptions dotAlias = options;
        dotAlias.outputDirectory = src + L".";
        Check(!Yua::RestoreSinglePackage(task, tables, dotAlias).succeeded,
              L"trailing-dot same-source alias rejected");
        Yua::RestoreOptions spaceAlias = options;
        spaceAlias.outputDirectory = src + L" ";
        Check(!Yua::RestoreSinglePackage(task, tables, spaceAlias).succeeded,
              L"trailing-space same-source alias rejected");
        Check(CountFilesRecursive(src) == srcFilesBefore,
              L"source unchanged after alias rejections");
    }

    // 源根/封包目录是 junction：不得进入目标
    {
        const std::wstring jReal = root + L"\\jsrc-real";
        MakeDir(jReal);
        HashPath(jReal, kDirHash1, kFileHash1, "junction-source");
        const std::wstring jLink = root + L"\\jsrc-link";
        if (CreateJunction(jLink, jReal)) {
            const std::wstring jOut = root + L"\\jout";
            Yua::RestorePackageTask jTask;
            jTask.packageName = L"pack";
            jTask.packageDirectory = jLink + L"\\" + kDirHash1;
            Yua::RestoreOptions jOptions;
            jOptions.sourceDirectory = jLink;
            jOptions.outputDirectory = jOut;
            Yua::RestorePackageResult jResult = Yua::RestoreSinglePackage(jTask, tables, jOptions);
            Check(!jResult.succeeded, L"source root junction rejected");
            Check(::GetFileAttributesW(jOut.c_str()) == INVALID_FILE_ATTRIBUTES,
                  L"source junction rejection did not create output root");
        } else {
            std::printf("NOTE junction creation unsupported; source-root junction check skipped\n");
        }
    }

    // 源目录里的文件 symlink：拒绝扫描，不能复制到目标
    {
        const std::wstring linkSrc = root + L"\\linksrc";
        const std::wstring linkPack = linkSrc + L"\\pack";
        MakeDir(linkPack);
        const std::wstring sentinelLinkTarget = root + L"\\link-sentinel.txt";
        WriteText(sentinelLinkTarget, "link-target");
        const std::wstring linkFile = linkPack + L"\\filelink.txt";
        if (::CreateSymbolicLinkW(linkFile.c_str(), sentinelLinkTarget.c_str(), 0) != FALSE) {
            const std::wstring linkOut = root + L"\\linkout";
            Yua::RestorePackageTask linkTask;
            linkTask.packageName = L"pack";
            linkTask.packageDirectory = linkPack;
            Yua::RestoreOptions linkOptions;
            linkOptions.sourceDirectory = linkSrc;
            linkOptions.outputDirectory = linkOut;
            Yua::RestorePackageResult linkResult = Yua::RestoreSinglePackage(linkTask, tables, linkOptions);
            Check(!linkResult.succeeded, L"file symlink in source rejected");
            Check(::GetFileAttributesW((linkOut + L"\\pack\\filelink.txt").c_str()) == INVALID_FILE_ATTRIBUTES,
                  L"file symlink not copied to output");
        } else {
            std::printf("NOTE file symlink creation unsupported; file-link check skipped\n");
        }
    }

    // ---- 冲突不覆盖 ----
    const std::wstring conflictTarget = out + L"\\pack\\bgimage\\cg01.png";
    WriteText(conflictTarget, "other-content");
    Yua::RestorePackageResult conflictResult = Yua::RestoreSinglePackage(task, tables, options);
    std::string conflictBytes;
    Check(ReadAll(conflictTarget, conflictBytes) && conflictBytes == "other-content",
          L"existing target not overwritten");
    std::string dupBytes;
    Check(ReadAll(out + L"\\pack\\bgimage\\cg01__dup1.png", dupBytes) && dupBytes == "hello",
          L"conflict went to __dup1 with source content");
    Check(conflictResult.renamedForConflict >= 1u, L"conflict rename counted");

    // ---- 同内容重跑不产生 dup，并记录 delivery ----
    const std::wstring out2 = root + L"\\out2";
    MakeDir(out2);
    Yua::RestoreOptions rerun = options;
    rerun.outputDirectory = out2;
    Yua::RestorePackageResult first = Yua::RestoreSinglePackage(task, tables, rerun);
    Yua::RestorePackageResult second = Yua::RestoreSinglePackage(task, tables, rerun);
    Check(first.deliveries.size() >= 3u, L"first pass records deliveries");
    Check(::GetFileAttributesW((out2 + L"\\pack\\bgimage\\cg01__dup1.png").c_str()) == INVALID_FILE_ATTRIBUTES,
          L"identical rerun does not create dup");
    Check(second.deliveries.size() >= 3u && second.restoredFiles >= 1u,
          L"identical existing still records delivery and restored count");

    // ---- 两遍式：按源记录旧落点，只删本源自己的旧目标 ----
    const std::wstring out3 = root + L"\\out3";
    MakeDir(out3);
    MakeDir(out3 + L"\\pack");
    const std::wstring oldTargetRel = std::wstring(L"pack\\") + kFileHash1 + L".png";
    const std::wstring oldTargetFull = out3 + L"\\" + oldTargetRel;
    WriteText(oldTargetFull, "hello");
    const std::wstring userFile = out3 + L"\\pack" + L"\\" + kFileHash1 + L".user.txt";
    WriteText(userFile, "user-data");

    Yua::RestoreOptions pass2 = options;
    pass2.outputDirectory = out3;
    pass2.previousDeliveries[std::wstring(L"pack\\") + kDirHash1 + L"\\" + kFileHash1] = oldTargetRel;
    Yua::RestorePackageResult pass2Result = Yua::RestoreSinglePackage(task, tables, pass2);
    Check(::GetFileAttributesW((out3 + L"\\pack\\bgimage\\cg01.png").c_str()) != INVALID_FILE_ATTRIBUTES,
          L"pass2 delivers true name");
    Check(::GetFileAttributesW(oldTargetFull.c_str()) == INVALID_FILE_ATTRIBUTES,
          L"pass2 removes only its own old target");
    Check(::GetFileAttributesW(userFile.c_str()) != INVALID_FILE_ATTRIBUTES,
          L"pass2 does not wildcard-delete user files");
    bool deliveryFound = false;
    for (const auto& delivery : pass2Result.deliveries) {
        if (delivery.sourceRelative == std::wstring(L"pack\\") + kDirHash1 + L"\\" + kFileHash1 &&
            delivery.targetRelative == L"pack\\bgimage\\cg01.png") {
            deliveryFound = true;
        }
    }
    Check(deliveryFound, L"delivery records SOURCE -> actual target");

    // 同源同目标同内容 → skip（不再复制/计数）
    Yua::RestoreOptions pass2Again = pass2;
    pass2Again.previousDeliveries.clear();
    for (const auto& delivery : pass2Result.deliveries) {
        pass2Again.previousDeliveries[delivery.sourceRelative] = delivery.targetRelative;
    }
    Yua::RestorePackageResult skipped = Yua::RestoreSinglePackage(task, tables, pass2Again);
    Check(skipped.skippedAlreadyDelivered >= 1u, L"same source+target+content skipped in next pass");

    // ---- 同源 dup 两遍不膨胀：前一版落 __dup1，下一遍应跳过而不是再造 __dup2 ----
    {
        const std::wstring out4 = root + L"\\out4";
        MakeDir(out4);
        MakeDir(out4 + L"\\pack");
        MakeDir(out4 + L"\\pack\\bgimage");
        Check(WriteText(out4 + L"\\pack\\bgimage\\cg01.png", "occupied-by-user"),
              L"conflict fixture written");
        Yua::RestoreOptions firstDup = options;
        firstDup.outputDirectory = out4;
        Yua::RestorePackageResult firstDupResult = Yua::RestoreSinglePackage(task, tables, firstDup);
        Check(::GetFileAttributesW((out4 + L"\\pack\\bgimage\\cg01__dup1.png").c_str()) != INVALID_FILE_ATTRIBUTES,
              L"first pass lands __dup1 on conflict");
        Yua::RestoreOptions secondDup = firstDup;
        for (const auto& delivery : firstDupResult.deliveries) {
            secondDup.previousDeliveries[delivery.sourceRelative] = delivery.targetRelative;
        }
        Yua::RestorePackageResult secondDupResult = Yua::RestoreSinglePackage(task, tables, secondDup);
        Check(secondDupResult.skippedAlreadyDelivered >= 1u,
              L"previous __dup actual target treated as same logical target");
        Check(::GetFileAttributesW((out4 + L"\\pack\\bgimage\\cg01__dup2.png").c_str()) == INVALID_FILE_ATTRIBUTES,
              L"two passes do not grow dup chain");
        std::string occupied;
        Check(ReadAll(out4 + L"\\pack\\bgimage\\cg01.png", occupied) && occupied == "occupied-by-user",
              L"conflicting user target still untouched");
    }

    // ---- MinedNames：历史优先 + 原子替换失败保留旧字节 ----
    const std::wstring histDir = root + L"\\hist";
    MakeDir(histDir);
    const std::wstring histPath = histDir + L"\\MinedNames.lst";
    const std::string oldContent =
        "\xEF\xBB\xBF# old\r\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:旧名字.txt\r\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb:保留.txt\r\n";
    WriteText(histPath, oldContent);

    std::map<std::wstring, std::wstring> mined;
    mined[L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"] = L"新名字.txt";
    mined[L"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"] = L"新增.txt";
    unsigned int written = 0;
    DWORD writeError = 0;
    Check(Yua::WriteMinedNamesList(histPath, mined, written, writeError), L"MinedNames write succeeds");
    std::string merged;
    ReadAll(histPath, merged);
    Check(merged.find("旧名字.txt") != std::string::npos, L"MinedNames old entry wins over new");
    Check(merged.find("保留.txt") != std::string::npos, L"MinedNames unrelated history kept");
    Check(merged.find("新增.txt") != std::string::npos, L"MinedNames new entry appended");
    Check(written == 3u, L"MinedNames merged count reported");

    // 目标目录是 junction → AtomicFile 拒绝，旧文件字节保持
    const std::wstring realHist = root + L"\\realhist";
    MakeDir(realHist);
    const std::wstring junctionHist = root + L"\\junctionhist";
    {
        std::wstring command = L"cmd.exe /c mklink /J \"" + junctionHist + L"\" \"" + realHist + L"\" >nul 2>nul";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(L'\0');
        if (::CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &startup, &process)) {
            ::WaitForSingleObject(process.hProcess, 30000);
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
        }
    }
    if (::GetFileAttributesW(junctionHist.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring junctionFile = junctionHist + L"\\MinedNames.lst";
        WriteText(realHist + L"\\MinedNames.lst", oldContent);
        unsigned int rejectedWritten = 0;
        DWORD rejectedError = 0;
        const bool refused = !Yua::WriteMinedNamesList(junctionFile, mined, rejectedWritten, rejectedError);
        Check(refused, L"atomic replace refuses reparse-point target");
        std::string afterRefusal;
        ReadAll(realHist + L"\\MinedNames.lst", afterRefusal);
        Check(afterRefusal == oldContent, L"failed atomic replace keeps old bytes");
    } else {
        std::printf("NOTE junction creation unsupported; atomic refusal check skipped\n");
    }

    // 旧文件读不了（这里是同名目录）必须停止写，保留原对象
    {
        const std::wstring readFailDir = root + L"\\readfail";
        MakeDir(readFailDir);
        const std::wstring asDirectory = readFailDir + L"\\MinedNames.lst";
        MakeDir(asDirectory);
        unsigned int failedWritten = 0;
        DWORD failedError = 0;
        std::wstring failedMessage;
        Check(!Yua::WriteMinedNamesList(asDirectory, mined, failedWritten, failedError, &failedMessage),
              L"unreadable existing MinedNames target refuses write");
        Check(::GetFileAttributesW(asDirectory.c_str()) != INVALID_FILE_ATTRIBUTES,
              L"unreadable MinedNames object preserved");
    }

    // 旧文件含无法解析的行：不静默丢掉，保留原字节
    {
        const std::wstring badDir = root + L"\\badmined";
        MakeDir(badDir);
        const std::wstring badPath = badDir + L"\\MinedNames.lst";
        const std::string badContent =
            "\xEF\xBB\xBF# old\r\n"
            "this-line-is-not-a-mapping\r\n"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:旧名字.txt\r\n";
        WriteText(badPath, badContent);
        unsigned int badWritten = 0;
        DWORD badError = 0;
        std::wstring badMessage;
        Check(!Yua::WriteMinedNamesList(badPath, mined, badWritten, badError, &badMessage),
              L"invalid old MinedNames line refuses rewrite");
        std::string afterBad;
        ReadAll(badPath, afterBad);
        Check(afterBad == badContent, L"invalid old MinedNames bytes preserved");
    }

    // 空 mined + 已有历史：不丢条目
    unsigned int emptyWritten = 0;
    DWORD emptyError = 0;
    Yua::WriteMinedNamesList(histPath, std::map<std::wstring, std::wstring>(), emptyWritten, emptyError);
    std::string afterEmpty;
    ReadAll(histPath, afterEmpty);
    Check(afterEmpty.find("旧名字.txt") != std::string::npos &&
              afterEmpty.find("保留.txt") != std::string::npos &&
              afterEmpty.find("新增.txt") != std::string::npos,
          L"empty partial run keeps all history");

    // ---- 多线程并发写：命名互斥体保证读-并集-替换串行，不丢别人的新条目 ----
    {
        const std::wstring concurrentPath = root + L"\\concurrent\\MinedNames.lst";
        MakeDir(root + L"\\concurrent");
        WriteText(concurrentPath, oldContent);
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        std::vector<int> ok(4, 0);
        const wchar_t* hashes[4] = {
            L"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
            L"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
            L"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
            L"9999999999999999999999999999999999999999999999999999999999999999",
        };
        const wchar_t* names[4] = {L"并发A.txt", L"并发B.txt", L"并发C.txt", L"并发D.txt"};
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&, i]() {
                while (!start.load()) ::Sleep(1);
                std::map<std::wstring, std::wstring> one;
                one[hashes[i]] = names[i];
                unsigned int count = 0;
                DWORD err = 0;
                ok[i] = Yua::WriteMinedNamesList(concurrentPath, one, count, err) ? 1 : 0;
            });
        }
        start.store(true);
        for (auto& thread : threads) thread.join();
        std::string concurrentContent;
        ReadAll(concurrentPath, concurrentContent);
        bool allWritersOk = true;
        bool allEntries = true;
        for (int i = 0; i < 4; ++i) {
            allWritersOk = allWritersOk && ok[i] == 1;
            allEntries = allEntries && concurrentContent.find(Utf8(names[i])) != std::string::npos;
        }
        Check(allWritersOk, L"concurrent MinedNames writes all succeed");
        Check(allEntries, L"concurrent writes do not overwrite each other's entries");
        Check(concurrentContent.find("旧名字.txt") != std::string::npos,
              L"concurrent writes keep history");
    }

    std::printf("RESULT checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
