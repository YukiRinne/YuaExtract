#include "PeResource.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace PeResource {

// PE结构（资源解析的最小子集）
#pragma pack(push, 1)
struct DosHeader {
    uint8_t  magic[2];    // "MZ"
    uint8_t  _pad[58];
    uint32_t e_lfanew;
};
struct FileHeader {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};
struct DataDirectory {
    uint32_t VirtualAddress;
    uint32_t Size;
};
struct OptionalHeader {
    uint16_t Magic;
    uint8_t  _pad1[94];
    DataDirectory DataDir[16];
};
struct SectionHeader {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
};
struct ResDir {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint16_t NumberOfNamedEntries;
    uint16_t NumberOfIdEntries;
};
struct ResDirEntry {
    uint32_t Name;
    uint32_t OffsetToData;
};
struct ResDataEntry {
    uint32_t OffsetToData;
    uint32_t Size;
    uint32_t CodePage;
    uint32_t Reserved;
};
#pragma pack(pop)

static const uint32_t RES_DIR_FLAG = 0x80000000;
static const uint32_t RES_NAME_FLAG = 0x80000000;

// PE 头解析结果。
struct PeHeaders {
    const FileHeader* file_header;
    const OptionalHeader* optional_header;
    const SectionHeader* sections;
    size_t section_count;
    uint32_t rva_count;
};

static constexpr uint32_t kPe32OptionalHeaderMinimum = 96u; // DataDirectory 之前的固定部分
static constexpr uint32_t kDataDirectoryOffset = 96u;

// 解析 PE 头与节表，任何越界都返回 false。
// 不能只看 "e_lfanew + sizeof(NT headers)"：SizeOfOptionalHeader / NumberOfSections
// 都是文件里的字段，必须用声明值去卡节表与数据目录的实际范围。
static bool parse_pe_headers(const uint8_t* pe, size_t pe_size, PeHeaders& out) {
    if (!pe || pe_size < sizeof(DosHeader)) return false;
    auto* dos = (const DosHeader*)pe;
    if (dos->magic[0] != 'M' || dos->magic[1] != 'Z') return false;

    const size_t e_lfanew = (size_t)dos->e_lfanew;
    if (e_lfanew < sizeof(DosHeader) || e_lfanew > pe_size) return false;
    if (pe_size - e_lfanew < 4u + sizeof(FileHeader)) return false;

    size_t fh_off = e_lfanew + 4u;
    auto* fh = (const FileHeader*)(pe + fh_off);

    const size_t oh_off = fh_off + sizeof(FileHeader);
    const size_t oh_size = fh->SizeOfOptionalHeader;
    if (oh_size < kPe32OptionalHeaderMinimum || oh_size > pe_size - oh_off) return false;
    auto* oh = (const OptionalHeader*)(pe + oh_off);
    if (oh->Magic != 0x10Bu) return false;  // PE32

    // NumberOfRvaAndSizes 在 optional header 偏移 92 处（PE32）。
    uint32_t rva_count = 0;
    std::memcpy(&rva_count, reinterpret_cast<const uint8_t*>(oh) + 92u, sizeof(rva_count));
    if (rva_count > 16u) rva_count = 16u;
    const uint64_t directories_bytes =
        static_cast<uint64_t>(kDataDirectoryOffset) + static_cast<uint64_t>(rva_count) * 8u;
    if (directories_bytes > oh_size) return false;

    const size_t sec_off = oh_off + oh_size;
    const size_t sec_count = fh->NumberOfSections;
    if (sec_count > (pe_size - sec_off) / sizeof(SectionHeader)) return false;

    out.file_header = fh;
    out.optional_header = oh;
    out.sections = (const SectionHeader*)(pe + sec_off);
    out.section_count = sec_count;
    out.rva_count = rva_count;
    return true;
}

uint32_t rva_to_file_offset(const uint8_t* pe, size_t pe_size, uint32_t rva) {
    PeHeaders h{};
    if (!parse_pe_headers(pe, pe_size, h)) return UINT32_MAX;

    for (size_t i = 0; i < h.section_count; ++i) {
        const SectionHeader& section = h.sections[i];
        uint32_t vsize = section.VirtualSize;
        if (vsize == 0) vsize = section.SizeOfRawData;
        if (rva < section.VirtualAddress) continue;
        const uint32_t delta = rva - section.VirtualAddress;
        if (delta >= vsize) continue;

        // 只返回真正有文件字节的位置：虚拟填充区 / 节外 RVA 一律无效。
        if (delta >= section.SizeOfRawData) return UINT32_MAX;
        const uint64_t offset = static_cast<uint64_t>(section.PointerToRawData) + delta;
        if (offset >= pe_size) return UINT32_MAX;
        return static_cast<uint32_t>(offset);
    }
    return UINT32_MAX;
}

uint32_t read_image_base(const uint8_t* pe, size_t pe_size) {
    PeHeaders h{};
    if (!parse_pe_headers(pe, pe_size, h)) return 0;
    // PE32 OptionalHeader 的 ImageBase 位于其偏移 28 处。
    if (h.file_header->SizeOfOptionalHeader < 32) return 0;
    uint32_t image_base = 0;
    std::memcpy(&image_base, reinterpret_cast<const uint8_t*>(h.optional_header) + 28u, sizeof(image_base));
    return image_base;
}

// 资源目录范围：所有 OffsetToData / 名字偏移都相对资源根，而不是当前这一层目录。
struct ResourceTree {
    const uint8_t* root = nullptr;
    size_t size = 0;
};

static const ResDir* resource_dir_at(const ResourceTree& tree, uint32_t offset) {
    if (tree.root == nullptr || offset > tree.size) return nullptr;
    if (tree.size - offset < sizeof(ResDir)) return nullptr;
    return reinterpret_cast<const ResDir*>(tree.root + offset);
}

static bool resource_entries(const ResourceTree& tree, const ResDir* dir, uint32_t dir_offset,
                             const ResDirEntry** entries, uint32_t* count) {
    if (dir == nullptr || entries == nullptr || count == nullptr) return false;
    const uint32_t total = static_cast<uint32_t>(dir->NumberOfNamedEntries) +
                           static_cast<uint32_t>(dir->NumberOfIdEntries);
    if (dir_offset > tree.size) return false;
    const size_t after_header = tree.size - dir_offset - sizeof(ResDir);
    if (static_cast<uint64_t>(total) * sizeof(ResDirEntry) > after_header) return false;
    *entries = reinterpret_cast<const ResDirEntry*>(dir + 1);
    *count = total;
    return true;
}

static bool resource_name_matches(const ResourceTree& tree, uint32_t name_offset,
                                  const wchar_t* expected) {
    if (expected == nullptr) return false;
    if (name_offset > tree.size || tree.size - name_offset < 2u) return false;
    uint16_t length = 0;
    std::memcpy(&length, tree.root + name_offset, sizeof(length));
    const uint64_t bytes = 2u + static_cast<uint64_t>(length) * 2u;
    if (bytes > tree.size - name_offset) return false;
    if (static_cast<size_t>(length) != wcslen(expected)) return false;
    const uint8_t* text = tree.root + name_offset + 2u;
    for (uint16_t i = 0; i < length; ++i) {
        uint16_t ch = 0;
        std::memcpy(&ch, text + i * 2u, sizeof(ch));
        if (static_cast<wchar_t>(ch) != expected[i]) return false;
    }
    return true;
}

// Find a resource by walking the 3-level tree: Type → Name → Language.
// 返回(data_ptr, data_size)对，未找到则返回(nullptr, 0)。
static std::pair<const uint8_t*, uint32_t> find_resource(
    const uint8_t* pe, size_t pe_size, const ResourceTree& tree,
    bool type_is_name, const wchar_t* type_name, uint16_t type_id,
    bool name_is_name, const wchar_t* res_name, uint16_t res_id) {
    const ResDir* root = resource_dir_at(tree, 0);
    if (root == nullptr) return {nullptr, 0};

    const ResDirEntry* entries = nullptr;
    uint32_t count = 0;
    if (!resource_entries(tree, root, 0u, &entries, &count)) return {nullptr, 0};

    // 第1层：Type
    uint32_t lv2_offset = 0;
    bool found_lv2 = false;
    for (uint32_t i = 0; i < count; ++i) {
        bool match = false;
        if (type_is_name) {
            if ((entries[i].Name & RES_NAME_FLAG) != 0) {
                match = resource_name_matches(tree, entries[i].Name & 0x7FFFFFFFu, type_name);
            }
        } else {
            if ((entries[i].Name & RES_NAME_FLAG) == 0 && entries[i].Name == type_id)
                match = true;
        }
        if (match && (entries[i].OffsetToData & RES_DIR_FLAG)) {
            lv2_offset = entries[i].OffsetToData & 0x7FFFFFFFu;
            found_lv2 = true;
            break;
        }
    }
    const ResDir* lv2 = found_lv2 ? resource_dir_at(tree, lv2_offset) : nullptr;
    if (!lv2) return {nullptr, 0};

    // 第2层：Name
    const ResDirEntry* lv2_entries = nullptr;
    uint32_t lv2_count = 0;
    if (!resource_entries(tree, lv2, lv2_offset, &lv2_entries, &lv2_count)) return {nullptr, 0};

    uint32_t lv3_offset = 0;
    bool found_lv3 = false;
    for (uint32_t i = 0; i < lv2_count; ++i) {
        bool match = false;
        if (name_is_name) {
            if ((lv2_entries[i].Name & RES_NAME_FLAG) != 0) {
                match = resource_name_matches(tree, lv2_entries[i].Name & 0x7FFFFFFFu, res_name);
            }
        } else {
            if ((lv2_entries[i].Name & RES_NAME_FLAG) == 0 && lv2_entries[i].Name == res_id)
                match = true;
        }
        if (match && (lv2_entries[i].OffsetToData & RES_DIR_FLAG)) {
            lv3_offset = lv2_entries[i].OffsetToData & 0x7FFFFFFFu;
            found_lv3 = true;
            break;
        }
    }
    const ResDir* lv3 = found_lv3 ? resource_dir_at(tree, lv3_offset) : nullptr;
    if (!lv3) return {nullptr, 0};

    // 第3层：Language。不假设第一个就是中性语言：逐个找第一个真正指向数据的条目。
    const ResDirEntry* lv3_entries = nullptr;
    uint32_t lv3_count = 0;
    if (!resource_entries(tree, lv3, lv3_offset, &lv3_entries, &lv3_count)) return {nullptr, 0};

    for (uint32_t i = 0; i < lv3_count; ++i) {
        if ((lv3_entries[i].OffsetToData & RES_DIR_FLAG) != 0) continue;
        const uint32_t data_offset = lv3_entries[i].OffsetToData;
        if (data_offset > tree.size || tree.size - data_offset < sizeof(ResDataEntry)) continue;
        const auto* data_entry = reinterpret_cast<const ResDataEntry*>(tree.root + data_offset);

        const uint32_t file_off = rva_to_file_offset(pe, pe_size, data_entry->OffsetToData);
        if (file_off == UINT32_MAX) continue;
        if (static_cast<uint64_t>(file_off) + data_entry->Size > pe_size) continue;
        return {pe + file_off, data_entry->Size};
    }
    return {nullptr, 0};
}

static bool get_resource_tree(const uint8_t* pe, size_t pe_size, ResourceTree& tree) {
    tree = ResourceTree{};
    PeHeaders headers{};
    if (!parse_pe_headers(pe, pe_size, headers)) return false;
    if (headers.rva_count <= 2u) return false;
    if (headers.file_header->SizeOfOptionalHeader <
        kDataDirectoryOffset + 3u * sizeof(DataDirectory)) return false;

    const DataDirectory* directories =
        reinterpret_cast<const DataDirectory*>(reinterpret_cast<const uint8_t*>(headers.optional_header) +
                                               kDataDirectoryOffset);
    const uint32_t res_rva = directories[2].VirtualAddress;
    const uint32_t res_sz = directories[2].Size;
    if (res_rva == 0 || res_sz == 0) return false;

    const uint32_t res_off = rva_to_file_offset(pe, pe_size, res_rva);
    if (res_off == UINT32_MAX) return false;
    if (res_off >= pe_size) return false;

    const size_t available = pe_size - res_off;
    tree.root = pe + res_off;
    // 声明的资源段大小不能超出文件；超出就只信文件里真实存在的部分。
    tree.size = std::min<size_t>(res_sz, available);
    return tree.size >= sizeof(ResDir);
}

std::vector<uint8_t> read_rcdata(const uint8_t* pe_data, size_t pe_size,
                                  const char* name) {
    ResourceTree tree;
    if (!get_resource_tree(pe_data, pe_size, tree)) return {};

    // 将ASCII名称转换为wchar_t
    size_t nlen = std::strlen(name);
    std::vector<wchar_t> wname(nlen + 1);
    for (size_t i = 0; i < nlen; ++i) wname[i] = (wchar_t)(unsigned char)name[i];
    wname[nlen] = 0;

    auto [ptr, sz] = find_resource(pe_data, pe_size, tree,
                                    false, nullptr, 10,
                                    true, wname.data(), 0);

    if (!ptr) return {};

    std::vector<uint8_t> out(sz);
    std::memcpy(out.data(), ptr, sz);
    return out;
}

std::vector<uint8_t> read_custom_resource(const uint8_t* pe_data, size_t pe_size,
                                           const char* type, uint16_t id) {
    ResourceTree tree;
    if (!get_resource_tree(pe_data, pe_size, tree)) return {};

    // 将type名称转换为wchar_t
    size_t tlen = std::strlen(type);
    std::vector<wchar_t> wtype(tlen + 1);
    for (size_t i = 0; i < tlen; ++i) wtype[i] = (wchar_t)(unsigned char)type[i];
    wtype[tlen] = 0;

    auto [ptr, sz] = find_resource(pe_data, pe_size, tree,
                                    true, wtype.data(), 0,
                                    false, nullptr, id);

    if (!ptr) return {};

    std::vector<uint8_t> out(sz);
    std::memcpy(out.data(), ptr, sz);
    return out;
}

} // namespace PeResource
#include <windows.h>
#include <fstream>

namespace PeResource {

std::vector<uint8_t> read_rcdata(const wchar_t* exe_path, const wchar_t* name) {
    std::vector<uint8_t> out;

    HMODULE h = LoadLibraryExW(exe_path, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!h) return out;

    HRSRC hrsrc = FindResourceW(h, name, RT_RCDATA);
    if (!hrsrc) { FreeLibrary(h); return out; }

    HGLOBAL hglob = LoadResource(h, hrsrc);
    if (!hglob) { FreeLibrary(h); return out; }

    DWORD sz = SizeofResource(h, hrsrc);
    const void* ptr = LockResource(hglob);
    if (ptr && sz > 0) {
        out.resize(sz);
        memcpy(out.data(), ptr, sz);
    }

    FreeLibrary(h);
    return out;
}

std::vector<uint8_t> read_custom_resource(const wchar_t* exe_path,
                                           const wchar_t* type_name, uint16_t id) {
    std::vector<uint8_t> out;

    HMODULE h = LoadLibraryExW(exe_path, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!h) return out;

    HRSRC hrsrc = FindResourceExW(h, type_name, MAKEINTRESOURCEW(id),
                                   MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
    if (!hrsrc) { FreeLibrary(h); return out; }

    HGLOBAL hglob = LoadResource(h, hrsrc);
    if (!hglob) { FreeLibrary(h); return out; }

    DWORD sz = SizeofResource(h, hrsrc);
    const void* ptr = LockResource(hglob);
    if (ptr && sz > 0) {
        out.resize(sz);
        memcpy(out.data(), ptr, sz);
    }

    FreeLibrary(h);
    return out;
}

std::vector<uint8_t> read_raw_offset(const wchar_t* exe_path, uint32_t offset, size_t size) {
    std::vector<uint8_t> out;
    constexpr size_t kMaxRead = 64ull * 1024ull * 1024ull;
    if (size == 0 || size > kMaxRead) return out;

    std::ifstream f(exe_path, std::ios::binary);
    if (!f) return out;
    f.seekg(0, std::ios::end);
    const std::streamoff end = f.tellg();
    if (end < 0 || static_cast<uint64_t>(offset) > static_cast<uint64_t>(end) ||
        static_cast<uint64_t>(size) > static_cast<uint64_t>(end) - static_cast<uint64_t>(offset)) {
        return out;
    }
    f.seekg(offset);
    if (!f) return out;
    out.resize(size);
    f.read((char*)out.data(), static_cast<std::streamsize>(size));
    if (f.gcount() != (std::streamsize)size) out.clear();
    return out;
}

} // namespace PeResource
