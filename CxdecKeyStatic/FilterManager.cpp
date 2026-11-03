#include "FilterManager.h"
#include "PathSafety.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <cstdio>

namespace FilterManager {

namespace {

// 只在属主自己 CREATE_NEW 成功后才拥有删除权；candidate 在 CREATE_NEW 之前
// 就已经写进 path（用于拼路径），所以必须用 owned 明确区分"我的文件"与
// "撞名的别人文件"。析构 noexcept 且不分配。
struct TempFileGuard
{
    std::wstring path;
    bool owned = false;
    ~TempFileGuard() noexcept
    {
        if (owned && !path.empty())
        {
            ::DeleteFileW(path.c_str());
        }
    }
};

struct ModuleGuard
{
    HMODULE module = nullptr;
    ~ModuleGuard() noexcept
    {
        if (module != nullptr)
        {
            ::FreeLibrary(module);
        }
    }
};

struct MemoryGuard
{
    void* pointer = nullptr;
    ~MemoryGuard() noexcept
    {
        if (pointer != nullptr)
        {
            ::VirtualFree(pointer, 0, MEM_RELEASE);
        }
    }
};

// 用已映射映像的 SizeOfImage 与节表做**无回绕**的范围校验。
// 不能拿 RVA 和资源原始字节数比：RVA 是映射后虚拟地址空间里的偏移。
bool ImageRangeContains(const uint8_t* base, uint32_t rva, uint32_t length, bool require_exec)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) || dos->e_lfanew > 0x100000)
        return false;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const uint64_t image_size = nt->OptionalHeader.SizeOfImage;
    if (image_size == 0 || static_cast<uint64_t>(rva) >= image_size) return false;
    if (static_cast<uint64_t>(length) > image_size - static_cast<uint64_t>(rva)) return false;

    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const uint64_t start = sections[i].VirtualAddress;
        const uint64_t span = (sections[i].Misc.VirtualSize > sections[i].SizeOfRawData)
                                  ? sections[i].Misc.VirtualSize
                                  : sections[i].SizeOfRawData;
        const uint64_t end = start + span;
        if (static_cast<uint64_t>(rva) >= start &&
            static_cast<uint64_t>(rva) + static_cast<uint64_t>(length) <= end)
        {
            if (!require_exec) return true;
            return (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        }
    }
    return false;
}

// 把派生用的 DLL 写进临时目录的**本次独占**文件。
// 先查临时目录整条路径上的重解析点，再用 CREATE_NEW 建唯一文件；
// 写入、Flush、Close 全部检查。
//
// guard 由调用方在调用**之前**构造并传入。guard.path 会在 CREATE_NEW 之前
// 就写入候选路径（拼路径用），但只有 CREATE_NEW 成功后才把 owned 置 true：
//   碰撞(CREATE_NEW -> ERROR_FILE_EXISTS) 或其它创建失败时 owned 保持 false，
//   guard 析构绝不会 DeleteFile 别人的同名文件。
// forced_path 仅用于 headless 测试注入固定候选名；生产调用传 nullptr。
bool WritePrivateTempDll(const uint8_t* data, size_t size, TempFileGuard& guard,
                         const wchar_t* forced_path = nullptr)
{
    guard.path.clear();
    guard.owned = false;
    if (data == nullptr || size == 0) return false;

    wchar_t temp_dir[MAX_PATH] = {};
    const DWORD temp_len = ::GetTempPathW(MAX_PATH, temp_dir);
    // 0 = 失败；>= MAX_PATH = 缓冲区不够、结果可能未终止/被截断，必须失败。
    if (temp_len == 0 || temp_len >= MAX_PATH) return false;
    std::wstring offending;
    if (PathSafety::HasReparsePoint(temp_dir, &offending)) return false;

    std::wstring dir(temp_dir);
    if (dir.empty()) return false;
    if (dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');

    static std::atomic<unsigned long> counter{0};
    for (int attempt = 0; attempt < 32; ++attempt)
    {
        wchar_t name[160] = {};
        _snwprintf_s(name, _countof(name), _TRUNCATE, L"yua-native-%lu-%llu-%lu-%d.dll",
                     static_cast<unsigned long>(::GetCurrentProcessId()),
                     static_cast<unsigned long long>(::GetTickCount64()),
                     counter.fetch_add(1), attempt);
        // 先落到 guard.path 再建文件：赋值若抛，owned 还是 false，不会误删。
        if (forced_path != nullptr)
        {
            guard.path.assign(forced_path);
        }
        else
        {
            guard.path.assign(dir);
            guard.path.append(name);
        }

        HANDLE file = ::CreateFileW(guard.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            const DWORD error = ::GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            return false;  // owned=false：不删任何既有路径
        }

        // CREATE_NEW 成功了，从这一刻起这个文件才是我们的，guard 可以删。
        guard.owned = true;

        bool ok = true;
        size_t written_total = 0;
        while (written_total < size)
        {
            const size_t remaining = size - written_total;
            const DWORD chunk = static_cast<DWORD>(
                remaining > (1u << 20) ? (1u << 20) : remaining);
            DWORD written = 0;
            if (!::WriteFile(file, data + written_total, chunk, &written, nullptr) ||
                written != chunk)
            {
                ok = false;
                break;
            }
            written_total += written;
        }
        if (ok && !::FlushFileBuffers(file)) ok = false;
        if (!::CloseHandle(file)) ok = false;
        if (!ok) return false;  // guard 析构时删文件
        return true;
    }
    return false;
}

}  // namespace

static constexpr size_t MANAGER_SIZE = 0x30B0;

using ManagerCtor     = void (__thiscall*)(void* manager);
using BootstrapDerive = int  (__thiscall*)(void* manager_core,
                                            const wchar_t* bootstrap, size_t bootstrap_bytes,
                                            const uint8_t* params, size_t params_bytes);
using ArchiveDerive   = int  (__thiscall*)(void* manager_core,
                                            const wchar_t* unique, size_t unique_bytes,
                                            void* seed_bytes);
using HashKeyDerive   = void (__cdecl*)(void* out_bytes, size_t out_size,
                                         const void* data_bytes, size_t data_size,
                                         int seed);

static bool call_native_functions(
    void* manager,
    void* manager_core,
    const wchar_t* bootstrap, size_t bootstrap_len,
    const uint8_t* params_data, size_t params_len,
    const wchar_t* unique, size_t unique_len,
    uint64_t archive_seed,
    uint8_t* hash_key_out,
    ManagerCtor pMgrCtor,
    BootstrapDerive pBootstrap,
    ArchiveDerive pArchive,
    HashKeyDerive pHashKey)
{
    __try {
        pMgrCtor(manager);

        int ok = pBootstrap(manager_core,
                            bootstrap, bootstrap_len * sizeof(wchar_t),
                            params_data, params_len);
        if (ok == 0) return false;

        uint8_t local_hk[32] = {};
        uint32_t flag = *(uint32_t*)((uint8_t*)manager + 0x30A0);
        if ((flag & 3) == 3) {
            pHashKey(local_hk, sizeof(local_hk),
                     (uint8_t*)manager + 0x3040, 0x40, -1);
        }
        memcpy(hash_key_out, local_hk, 32);

        uint8_t seed_bytes[8];
        seed_bytes[0] = (uint8_t)(archive_seed);
        seed_bytes[1] = (uint8_t)(archive_seed >> 8);
        seed_bytes[2] = (uint8_t)(archive_seed >> 16);
        seed_bytes[3] = (uint8_t)(archive_seed >> 24);
        seed_bytes[4] = (uint8_t)(archive_seed >> 32);
        seed_bytes[5] = (uint8_t)(archive_seed >> 40);
        seed_bytes[6] = (uint8_t)(archive_seed >> 48);
        seed_bytes[7] = (uint8_t)(archive_seed >> 56);
        pArchive(manager_core, unique, unique_len * sizeof(wchar_t), seed_bytes);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

DripProgram derive_drip_program(
    const uint8_t* dll_data, size_t dll_size,
    const std::wstring& final_bootstrap,
    const std::wstring& unique,
    const uint8_t* params_data, size_t params_len,
    uint64_t archive_seed,
    uint32_t rva_manager_ctor,
    uint32_t rva_bootstrap_derive,
    uint32_t rva_archive_derive,
    uint32_t rva_hashkey_derive,
    bool allow_native_execution) {

    DripProgram prog{};
    prog.ok = false;

    // 安全默认（第二层）：即使有人绕过 recover_drip_program 直接调到这里，
    // 未授权时也在创建临时文件 / LoadLibrary 之前退出。不能依赖调用方守规矩。
    if (!allow_native_execution) return prog;

    if (!dll_data || dll_size < 64) return prog;
    if (dll_data[0] != 'M' || dll_data[1] != 'Z') return prog;

    // 只做"非零"的早筛；RVA 的合法范围必须等映像映射后用 SizeOfImage/节表判断，
    // 不能拿 RVA 和资源原始字节数比（那是两类地址）。
    if (rva_manager_ctor == 0 || rva_bootstrap_derive == 0 ||
        rva_archive_derive == 0 || rva_hashkey_derive == 0)
        return prog;

    // guard 必须在创建临时文件之前就位：任何后续 wstring 分配抛异常都不会漏文件。
    TempFileGuard temp_guard;
    if (!WritePrivateTempDll(dll_data, dll_size, temp_guard)) return prog;

    ModuleGuard module;
    module.module = ::LoadLibraryW(temp_guard.path.c_str());
    if (module.module == nullptr) return prog;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(module.module);

    // 每个要调用的函数入口都必须落在**已映射映像**的可执行节里，
    // 且 rva + 16 不越过 SizeOfImage（无整数回绕）。
    if (!ImageRangeContains(base, rva_manager_ctor, 16, true) ||
        !ImageRangeContains(base, rva_bootstrap_derive, 16, true) ||
        !ImageRangeContains(base, rva_archive_derive, 16, true) ||
        !ImageRangeContains(base, rva_hashkey_derive, 16, true))
        return prog;

    {
        uint8_t* seed_ptr = (uint8_t*)module.module + 0x81758;
        uint64_t loaded_seed = 0;
        if (!IsBadReadPtr(seed_ptr, 8))
            loaded_seed = *(uint64_t*)seed_ptr;
        if (loaded_seed != 0) {
            archive_seed = loaded_seed;
        } else {
            // 回退：ArchiveUpdate 默认 seed（0x2CAFEACE,0xDEADBEEF 按小端组合）
            uint8_t default_seed[8] = { 0xCE, 0xEA, 0xAF, 0x2C, 0xEF, 0xBE, 0xAD, 0xDE };
            archive_seed = 0;
            for (int i = 0; i < 8; ++i)
                archive_seed |= (uint64_t)default_seed[i] << (i * 8);
        }
    }

    prog.source_module = temp_guard.path;
    prog.source_module_base = (uintptr_t)module.module;

    auto pMgrCtor   = (ManagerCtor)(base + rva_manager_ctor);
    auto pBootstrap = (BootstrapDerive)(base + rva_bootstrap_derive);
    auto pArchive   = (ArchiveDerive)(base + rva_archive_derive);
    auto pHashKey   = (HashKeyDerive)(base + rva_hashkey_derive);

    MemoryGuard manager_guard;
    manager_guard.pointer = VirtualAlloc(nullptr, MANAGER_SIZE, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
    if (!manager_guard.pointer) return prog;
    void* manager = manager_guard.pointer;
    std::memset(manager, 0, MANAGER_SIZE);

    uint8_t* mgr = (uint8_t*)manager;
    uint8_t* core = mgr + 8;
    prog.manager_va = (uintptr_t)manager;

    uint8_t hash_key_buf[32] = {};
    bool native_ok = call_native_functions(
        manager, core,
        final_bootstrap.c_str(), final_bootstrap.size(),
        params_data, params_len,
        unique.c_str(), unique.size(),
        archive_seed,
        hash_key_buf,
        pMgrCtor, pBootstrap, pArchive, pHashKey);

    if (!native_ok) return prog;

    // holder_words 从 core 起读；按 core+4 读会错位一个 dword，filter state 全错
    for (int i = 0; i < 6; ++i)
        prog.holder_words[i] = *(uint32_t*)(core + i * 4);
    std::memcpy(prog.hxv4_key.data(), core + 0x3038, 32);
    std::memcpy(prog.hxv4_nonce1.data(), core + 0x3058, 24);
    std::memcpy(prog.hxv4_nonce0.data(), core + 0x3078, 24);

    uint32_t drip_impl_rva = *(uint32_t*)(core);
    uint8_t* drip_impl = (uint8_t*)(uintptr_t)drip_impl_rva;
    prog.drip_impl_va = (uintptr_t)drip_impl;

    if (drip_impl && !IsBadReadPtr(drip_impl, 0x810)) {
        uint32_t context_ptr = *(uint32_t*)(drip_impl + 0x04 + 0x0C);
        prog.context_va = (uintptr_t)context_ptr;
        if (context_ptr >= (uint32_t)(uintptr_t)manager &&
            context_ptr < (uint32_t)(uintptr_t)manager + MANAGER_SIZE) {
            uint8_t* ctx_start = (uint8_t*)(uintptr_t)context_ptr;
            uint8_t* ctx_end   = mgr + MANAGER_SIZE;
            size_t ctx_bytes   = ctx_end - ctx_start;
            size_t ctx_count   = ctx_bytes / 4;
            prog.context_u32.resize(ctx_count);
            for (size_t i = 0; i < ctx_count; ++i)
                prog.context_u32[i] = *(uint32_t*)(ctx_start + i * 4);

            // 提取lanes（128条lane，每条0x10字节，起始于drip_impl + 0x04）
            //
            // 别拿 manager 缓冲区当边界：drip_impl 是**另一块分配**，不在 manager 里。
            // 曾经拿 mgr + MANAGER_SIZE 做上界，只有 drip_impl 恰好落在 manager 下面
            // 时才碰巧成立——落在上面就第一次循环直接 break，一条 lane 都提不出来
            // （约三次里出一次），而且不报错。
            // 上面那句 IsBadReadPtr(drip_impl, 0x810) 已经盖住了 0x800 的整张表，
            // 这里不需要再判一遍。
            static constexpr int LANE_COUNT = 128;
            static constexpr int LANE_SIZE = 0x10;
            uint8_t* lane_base = drip_impl + 0x04;
            for (int li = 0; li < LANE_COUNT; ++li) {
                uint8_t* lane = lane_base + li * LANE_SIZE;
                uint32_t begin   = *(uint32_t*)(lane);
                uint32_t end     = *(uint32_t*)(lane + 4);
                // 跳过current和ctx
                if (end < begin || ((end - begin) % 8) != 0) continue;
                std::vector<std::pair<uint32_t, uint32_t>> records;
                for (uint32_t rec = begin; rec < end; rec += 8) {
                    uint8_t* rp = (uint8_t*)(uintptr_t)rec;
                    if (IsBadReadPtr(rp, 8)) break;
                    uint32_t param = *(uint32_t*)(rp);
                    uint32_t callback = *(uint32_t*)(rp + 4);
                    records.push_back({param, callback});
                }
                prog.lanes.push_back(std::move(records));
            }
        }
    }

    std::memcpy(prog.hash_key.data(), hash_key_buf, 32);
    prog.ok = true;

    // 临时 DLL / HMODULE / manager 都由 guard 在返回时释放；异常路径同样如此。
    return prog;
}

} // namespace FilterManager
