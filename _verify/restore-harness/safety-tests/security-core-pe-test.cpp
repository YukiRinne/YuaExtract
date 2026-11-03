// security-core-pe-test
// 回归：PeParser / PeResource / PackerDetect / UnpackCore 的 PE 边界与 VA/RVA 口径。
// 合成 PE 只在命令行临时根下生成，不加载执行、不碰注册表。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../../CxdecPeUnpacker/PackerDetect.h"
#include "../../../CxdecPeUnpacker/PeParser.h"
#include "../../../CxdecPeUnpacker/UnpackCore.h"
#include "../../../CxdecKeyStatic/PeResource.h"

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


void Put16(std::vector<uint8_t>& v, size_t off, uint16_t x) {
    v[off] = static_cast<uint8_t>(x);
    v[off + 1] = static_cast<uint8_t>(x >> 8);
}
void Put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + i] = static_cast<uint8_t>(x >> (8 * i));
}
void Put64(std::vector<uint8_t>& v, size_t off, uint64_t x) {
    for (int i = 0; i < 8; ++i) v[off + i] = static_cast<uint8_t>(x >> (8 * i));
}

// 把解码后的 PackShellHeader 反编码成 XorDecodeChained(..., 0) 的输入。
void EncodeXorWords(const std::vector<uint32_t>& decoded, std::vector<uint8_t>& out) {
    uint32_t previous = decoded.empty() ? 0u : decoded[0];
    for (size_t i = 0; i < decoded.size(); ++i) {
        const uint32_t encoded = (i == 0) ? decoded[0] : (decoded[i] ^ previous);
        for (int b = 0; b < 4; ++b) out.push_back(static_cast<uint8_t>(encoded >> (8 * b)));
        previous = encoded;
    }
}

struct HeaderOptions {
    uint32_t payloadSize = 0;
    uint32_t drvOffset = 0;
    uint32_t drvSize = 0;
    bool tlsCallback = true;
    uint32_t bindVirtualSize = 0x20;
};

std::vector<uint8_t> BuildPe(const HeaderOptions& options, bool includeResource = true,
                             size_t* tlsAddressOfCallbacks = nullptr) {
    const size_t size = 0xF00;
    std::vector<uint8_t> pe(size, 0);
    pe[0] = 'M';
    pe[1] = 'Z';
    Put32(pe, 0x3C, 0x80);

    const size_t nt = 0x80;
    pe[nt] = 'P';
    pe[nt + 1] = 'E';
    Put16(pe, nt + 4, 0x014C);            // Machine i386
    Put16(pe, nt + 6, 4);                 // NumberOfSections
    Put16(pe, nt + 20, 224);              // SizeOfOptionalHeader
    Put16(pe, nt + 22, 0x0102);           // Characteristics

    const size_t oh = nt + 24;
    Put16(pe, oh + 0, 0x010B);            // PE32
    Put32(pe, oh + 16, 0x1000);           // AddressOfEntryPoint
    Put32(pe, oh + 20, 0x1000);           // BaseOfCode
    Put32(pe, oh + 28, 0x400000);         // ImageBase
    Put32(pe, oh + 32, 0x1000);           // SectionAlignment
    Put32(pe, oh + 36, 0x200);            // FileAlignment
    Put32(pe, oh + 56, 0x5000);           // SizeOfImage
    Put32(pe, oh + 60, 0x400);            // SizeOfHeaders
    Put32(pe, oh + 92, 16);               // NumberOfRvaAndSizes
    if (includeResource) {
        Put32(pe, oh + 96 + 2 * 8, 0x2000);
        Put32(pe, oh + 96 + 2 * 8 + 4, 0x400);
    }
    Put32(pe, oh + 96 + 9 * 8, 0x3000);
    Put32(pe, oh + 96 + 9 * 8 + 4, 24);

    const size_t sec = oh + 224;
    auto section = [&](int index, const char* name, uint32_t vsize, uint32_t va, uint32_t rawSize, uint32_t rawPtr) {
        const size_t at = sec + index * 40;
        std::memcpy(pe.data() + at, name, std::strlen(name));
        Put32(pe, at + 8, vsize);
        Put32(pe, at + 12, va);
        Put32(pe, at + 16, rawSize);
        Put32(pe, at + 20, rawPtr);
        Put32(pe, at + 36, 0x60000020u);
    };
    section(0, ".text", 0x400, 0x1000, 0x400, 0x400);
    section(1, ".bind", 0x100, 0x1400, 0x100, 0x800);
    section(2, ".rsrc", 0x400, 0x2000, 0x400, 0x900);
    section(3, ".data", 0x200, 0x3000, 0x200, 0xD00);

    // ---- 资源树：全部 OffsetToData/字符串偏移都相对资源根 ----
    if (includeResource) {
        const size_t r = 0x900;
        Put16(pe, r + 12, 0);
        Put16(pe, r + 14, 2);                  // root: 2 id entries
        Put32(pe, r + 0x10, 10);               // RT_RCDATA
        Put32(pe, r + 0x14, 0x80000020u);      // -> type1 lv2 at 0x20
        Put32(pe, r + 0x18, 0x800000C0u);      // named type "TEST" at 0xC0
        Put32(pe, r + 0x1C, 0x80000070u);      // -> type2 lv2 at 0x70

        Put16(pe, r + 0x20 + 12, 1);           // type1 lv2: 1 named name
        Put16(pe, r + 0x20 + 14, 0);
        Put32(pe, r + 0x30, 0x80000038u);      // name string at 0x38
        Put32(pe, r + 0x34, 0x80000048u);      // -> lv3 at 0x48
        Put16(pe, r + 0x38, 4);
        const wchar_t* name = L"NAME";
        for (int i = 0; i < 4; ++i) Put16(pe, r + 0x3A + i * 2, static_cast<uint16_t>(name[i]));

        Put16(pe, r + 0x48 + 12, 0);           // lv3 type1
        Put16(pe, r + 0x48 + 14, 1);
        Put32(pe, r + 0x58, 0x0411u);          // 非中性语言
        Put32(pe, r + 0x5C, 0x60u);            // data entry at 0x60
        Put32(pe, r + 0x60, 0x2100u);          // blob RVA (rsrc+0x100)
        Put32(pe, r + 0x64, 9u);

        Put16(pe, r + 0x70 + 12, 0);           // type2 lv2 (id 7)
        Put16(pe, r + 0x70 + 14, 1);
        Put32(pe, r + 0x80, 7u);
        Put32(pe, r + 0x84, 0x80000088u);
        Put16(pe, r + 0x88 + 12, 0);
        Put16(pe, r + 0x88 + 14, 1);
        Put32(pe, r + 0x98, 0x0409u);
        Put32(pe, r + 0x9C, 0xA0u);
        Put32(pe, r + 0xA0, 0x2110u);
        Put32(pe, r + 0xA4, 11u);

        Put16(pe, r + 0xC0, 4);
        const wchar_t* typeName = L"TEST";
        for (int i = 0; i < 4; ++i) Put16(pe, r + 0xC2 + i * 2, static_cast<uint16_t>(typeName[i]));
        std::memcpy(pe.data() + r + 0x100, "NAME-DATA", 9);
        std::memcpy(pe.data() + r + 0x110, "CUSTOM-DATA", 11);
    }

    // ---- TLS 回调（VA）与壳头 ----
    const size_t headerSize = sizeof(PackShellHeader);
    const size_t headerFile = 0x500;
    const size_t callbackArrayFile = 0x480;
    const size_t tlsFile = 0xD00;
    const uint32_t imageBase = 0x400000;
    if (options.tlsCallback) {
        const uint32_t headerVa = imageBase + 0x1000 + (headerFile - 0x400);  // .text RVA 0x1000
        const uint32_t arrayVa = imageBase + 0x1000 + (callbackArrayFile - 0x400);
        Put32(pe, tlsFile + 12, arrayVa);        // AddressOfCallBacks (VA of array)
        Put32(pe, callbackArrayFile, headerVa + static_cast<uint32_t>(headerSize)); // callback (VA)
        if (tlsAddressOfCallbacks) *tlsAddressOfCallbacks = arrayVa;
    }

    // 按字段偏移直接填 32 位 word（PackShellHeader 全是 4 字节对齐字段）：
    //   0 XorKey / 4 Signature / 8 ImageBase(2) / 16 AEP(2) / 24 BindOffset / 28 Unknown
    //   / 32 OriginalEntryPoint(2) / 40 Unknown0001 / 44 PayloadSize / 48 DrvOffset
    //   / 52 DrvSize / 56 AppId / 60 Flags / 64 BindSectionVirtualSize
    std::vector<uint32_t> decoded(headerSize / 4u, 0u);
    decoded[0] = 0xA5A5A5A5u;                 // XorKey
    decoded[1] = 0xC0DEC0DFu;                 // Signature
    decoded[8] = 0x1000u;                     // OriginalEntryPoint（低 32 位）
    decoded[11] = options.payloadSize;        // +0x2C PayloadSize
    decoded[12] = options.drvOffset;          // +0x30 DrvDllOffset
    decoded[13] = options.drvSize;            // +0x34 DrvDllSize
    decoded[15] = 4u;                         // +0x3C Flags = NoEncryption
    decoded[16] = options.bindVirtualSize;    // +0x40 BindSectionVirtualSize
    std::vector<uint8_t> encoded;
    EncodeXorWords(decoded, encoded);
    encoded.resize(headerSize, 0);
    std::memcpy(pe.data() + headerFile, encoded.data(), encoded.size());

    return pe;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    ::CloseHandle(file);
    return ok && written == bytes.size();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: security-core-pe-test <scratch-root>\n");
        return 2;
    }
    const std::wstring root = argv[1];
    ::CreateDirectoryW(root.c_str(), nullptr);

    HeaderOptions options;
    const std::vector<uint8_t> pe = BuildPe(options);
    const std::wstring exePath = root + L"\\sample.exe";
    Check(WriteFileBytes(exePath, pe), L"write synthetic PE");

    PeReader reader(exePath);
    Check(reader.Load(), L"valid PE loads");
    Check(reader.SectionCount() == 4u, L"section count");
    Check(reader.RvaToOffset(0x1000) == 0x400u, L"RVA to file offset");
    Check(reader.RvaToOffset(0x1100, 4) == 0x500u, L"RVA to offset with length fits");
    Check(reader.RvaToOffset(0x1000, 0x1000) == 0u, L"RVA conversion rejects read beyond raw section");
    Check(reader.GetDataDirectory(2).VirtualAddress == 0x2000u, L"resource data directory readable");
    Check(reader.GetDataDirectory(16).VirtualAddress == 0u && reader.GetDataDirectory(16).Size == 0u,
          L"out-of-range data directory returns empty");

    // ---- 合法三层资源（root-relative）+ 非中性语言 ----
    const std::vector<uint8_t> rcdata = PeResource::read_rcdata(pe.data(), pe.size(), "NAME");
    const std::vector<uint8_t> custom = PeResource::read_custom_resource(pe.data(), pe.size(), "TEST", 7);
    Check(std::string(rcdata.begin(), rcdata.end()) == "NAME-DATA",
          L"legal three-level root-relative RCData read");
    Check(std::string(custom.begin(), custom.end()) == "CUSTOM-DATA",
          L"legal named-type custom resource read with non-neutral language");

    // ---- 畸形资源 ----
    {
        std::vector<uint8_t> bad = pe;
        Put32(bad, 0x900 + 0x14, 0x80FFFF00u);  // root entry -> 越界目录
        Check(PeResource::read_rcdata(bad.data(), bad.size(), "NAME").empty(),
              L"resource dir offset beyond root rejected");
    }
    {
        std::vector<uint8_t> bad = pe;
        Put32(bad, 0x900 + 0x60, 0x7FFFFF00u);  // data RVA 越界
        Check(PeResource::read_rcdata(bad.data(), bad.size(), "NAME").empty(),
              L"resource data RVA beyond file rejected");
    }
    {
        std::vector<uint8_t> bad = pe;
        Put32(bad, 0x900 + 0x64, 0xFFFFFFF0u);  // data size 越界
        Check(PeResource::read_rcdata(bad.data(), bad.size(), "NAME").empty(),
              L"resource data size beyond file rejected");
    }
    {
        std::vector<uint8_t> bad = pe;
        Put32(bad, 0x900 + 0x30, 0x80FFFF00u);  // name string 越界
        Check(PeResource::read_rcdata(bad.data(), bad.size(), "NAME").empty(),
              L"resource name string offset rejected");
    }

    // ---- PeReader 畸形头部 ----
    {
        std::vector<uint8_t> bad = pe;
        Put16(bad, 0x80 + 20, 0x10);            // SizeOfOptionalHeader 过小
        WriteFileBytes(root + L"\\bad-opt.exe", bad);
        PeReader r(root + L"\\bad-opt.exe");
        Check(!r.Load(), L"tiny SizeOfOptionalHeader rejected");
    }
    {
        std::vector<uint8_t> bad = pe;
        Put16(bad, 0x80 + 6, 0xFFFF);           // NumberOfSections 巨大
        WriteFileBytes(root + L"\\bad-sec.exe", bad);
        PeReader r(root + L"\\bad-sec.exe");
        Check(!r.Load(), L"huge NumberOfSections rejected");
    }
    {
        std::vector<uint8_t> bad = pe;
        Put32(bad, 0x178 + 20, 0xFFFFFF00u);    // .text raw pointer 越界
        WriteFileBytes(root + L"\\bad-raw.exe", bad);
        PeReader r(root + L"\\bad-raw.exe");
        Check(!r.Load(), L"section raw range beyond file rejected");
    }

    // ---- TLS AddressOfCallBacks 是 VA：检测/脱壳都要先减 ImageBase ----
    PackerDetector detector(reader);
    Check(detector.Detect() == PackVariant::V31x86, L"packer detected through TLS VA callback");

    UnpackConfig config;
    config.verboseOutput = false;
    config.keepBindSection = true;
    UnpackEngine engine(reader, config);
    const std::wstring unpacked = root + L"\\unpacked.exe";
    Check(engine.Process(unpacked), L"unpack succeeds through TLS VA callback");
    Check(::GetFileAttributesW(unpacked.c_str()) != INVALID_FILE_ATTRIBUTES, L"unpacked PE written");

    // TLS 回调 VA 低于 ImageBase：畸形，不得崩溃
    {
        std::vector<uint8_t> bad = pe;
        Put32(bad, 0xD00 + 12, 0x100u);
        WriteFileBytes(root + L"\\bad-tls.exe", bad);
        PeReader r(root + L"\\bad-tls.exe");
        Check(r.Load(), L"bad TLS PE still loads structurally");
        PackerDetector d(r);
        Check(d.Detect() == PackVariant::None, L"VA below ImageBase rejected without crash");
        UnpackEngine e(r, config);
        Check(!e.Process(root + L"\\bad-tls-out.exe"), L"unpack rejects malformed TLS VA");
    }

    // 载荷大小畸形：必须失败而不是溢出
    {
        HeaderOptions badOptions = options;
        badOptions.payloadSize = 0xFFFFFFF0u;
        const std::vector<uint8_t> bad = BuildPe(badOptions);
        WriteFileBytes(root + L"\\bad-payload.exe", bad);
        PeReader r(root + L"\\bad-payload.exe");
        Check(r.Load(), L"payload PE loads structurally");
        UnpackEngine e(r, config);
        const bool badSucceeded = e.Process(root + L"\\bad-payload-out.exe");
        if (badSucceeded) {
            std::printf("NOTE bad payload unexpectedly succeeded: %ls\n", e.GetLastError().c_str());
        }
        Check(!badSucceeded, L"huge payload size rejected");
    }

    std::printf("RESULT checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
