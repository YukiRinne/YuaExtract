#include "BootstrapExtract.h"
#include "BresDecrypt.h"
#include "Crypto/ZlibInflate.h"
#include "PeResource.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace Bootstrap {

static void parse_config_table(const uint8_t* dll, size_t dll_size, uint32_t table_rva,
                                std::wstring& unique, std::wstring& warning,
                                std::vector<uint8_t>& params) {
    if (table_rva == 0 || table_rva > dll_size || dll_size - table_rva < 2) return;

    size_t cursor = table_rva;
    while (cursor < dll_size && dll_size - cursor >= 3) {
        if (dll[cursor] == 0) break;

        size_t label_start = cursor;
        while (cursor < dll_size && dll[cursor] != 0) cursor++;
        if (cursor >= dll_size) break;
        size_t label_len = cursor - label_start;
        cursor++;

        if (dll_size - cursor < 2) break;
        uint32_t length = (uint32_t)dll[cursor] | ((uint32_t)dll[cursor+1] << 8);
        cursor += 2;

        if (length == 0 || length > dll_size - cursor) {
            // 长度字段坏掉时不能 cursor += length（会越过末尾，甚至回绕后再也前进不了）。
            cursor += std::min<uint32_t>(length, (uint32_t)(dll_size - cursor));
            continue;
        }

        auto label_match = [&](const char* s) {
            return (size_t)strlen(s) == label_len &&
                   memcmp(dll + label_start, s, label_len) == 0;
        };

        if (label_match("UNIQUE")) {
            unique.clear();
            for (uint32_t i = 0; i + 1 < length; i += 2) {
                wchar_t ch = (wchar_t)dll[cursor + i]
                          | ((wchar_t)dll[cursor + i + 1] << 8);
                if (ch == 0) break;
                unique.push_back(ch);
            }
        } else if (label_match("WARNING")) {
            warning.clear();
            for (uint32_t i = 0; i < length; ++i) {
                if (dll[cursor + i] == 0) break;
                warning.push_back((wchar_t)dll[cursor + i]);
            }
        } else if (label_match("PARAMS")) {
            params.assign(dll + cursor, dll + cursor + length);
        }

        cursor += length;
    }
}

BootstrapConfig extract_bootstrap(
    const uint8_t* bootstrap_ct, size_t ct_len,
    const std::wstring& bootstrap_path,
    const uint8_t* salt, size_t salt_len,
    uint32_t archive_seed_rva) {

    BootstrapConfig cfg;
    cfg.ok = false;
    cfg.archive_seed = 0;

    std::vector<uint8_t> zlib_data;
    if (!Crypto::bres_decrypt(bootstrap_path, salt, salt_len,
                               bootstrap_ct, ct_len, zlib_data))
        return cfg;

    if (zlib_data.size() <= 8) return cfg;
    cfg.dll_data = Crypto::zlib_decompress(zlib_data.data() + 8,
                                            zlib_data.size() - 8);
    if (cfg.dll_data.empty()) return cfg;

    if (cfg.dll_data.size() < 2 || memcmp(cfg.dll_data.data(), "MZ", 2) != 0)
        return cfg;

    const uint32_t TABLE_RVA = 0x80E38;
    uint32_t table_file_off = PeResource::rva_to_file_offset(
        cfg.dll_data.data(), cfg.dll_data.size(), TABLE_RVA);
    if (table_file_off == UINT32_MAX) {
        cfg.ok = false;
        return cfg;
    }
    parse_config_table(cfg.dll_data.data(), cfg.dll_data.size(),
                        table_file_off, cfg.unique, cfg.warning, cfg.params);

    if (cfg.unique.empty()) {
        cfg.ok = false;
        return cfg;
    }

    uint32_t seed_file_off = archive_seed_rva > 0
        ? PeResource::rva_to_file_offset(cfg.dll_data.data(), cfg.dll_data.size(), archive_seed_rva)
        : 0;
    if (seed_file_off != UINT32_MAX && seed_file_off > 0 && seed_file_off + 8 <= cfg.dll_data.size()) {
        cfg.archive_seed =
            (uint64_t)cfg.dll_data[seed_file_off]
          | ((uint64_t)cfg.dll_data[seed_file_off + 1] << 8)
          | ((uint64_t)cfg.dll_data[seed_file_off + 2] << 16)
          | ((uint64_t)cfg.dll_data[seed_file_off + 3] << 24)
          | ((uint64_t)cfg.dll_data[seed_file_off + 4] << 32)
          | ((uint64_t)cfg.dll_data[seed_file_off + 5] << 40)
          | ((uint64_t)cfg.dll_data[seed_file_off + 6] << 48)
          | ((uint64_t)cfg.dll_data[seed_file_off + 7] << 56);
    }

    cfg.ok = true;
    return cfg;
}

} // namespace Bootstrap
