#include "pch.h"
#include "PackerDetect.h"
#include "PeParser.h"
#include "CryptoUtil.h"

namespace {
// TLS 目录里的 AddressOfCallBacks / 回调数组元素都是 **VA**，不是 RVA。
// 先减 ImageBase 再转文件偏移；减不动（低于镜像基址）就是畸形值。
uint32_t TlsVaToFileOffset(const PeReader& reader, uint32_t imageBase, uint64_t va, size_t length)
{
    if (va < imageBase) return 0;
    const uint64_t rva = va - imageBase;
    if (rva > 0xFFFFFFFFull) return 0;
    return reader.RvaToOffset(static_cast<uint32_t>(rva), length);
}
}

PackerDetector::PackerDetector(const PeReader& reader) : m_reader(reader) {}

PackVariant PackerDetector::Detect()
{
    auto* nt = m_reader.NtHeaders();
    if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
        return PackVariant::None;

    auto* bindSection = m_reader.SectionHeader(".bind");
    if (!bindSection || memcmp(bindSection->Name, ".bind", 5) != 0) {
        m_lastError = L"PE 文件中未找到 .bind 节";
        return PackVariant::None;
    }

    uint8_t headerBuf[sizeof(PackShellHeader)];
    bool headerFound = false;

    // 路径 1：壳头在入口点前 sizeof(PackShellHeader) 字节处。
    // 只需定位 EP 那 1 字节；前向 HEADER_SIZE 在 EP 靠节尾时本来就不存在，
    // 用 (ep, HEADER_SIZE) 去校验会把合法样本误拒。
    uint32_t entryPoint = nt->OptionalHeader.AddressOfEntryPoint;
    uint32_t aepFileOffset = m_reader.RvaToOffset(entryPoint, 1u);
    if (aepFileOffset >= sizeof(PackShellHeader)) {
        const uint32_t headerFileOffset = aepFileOffset - sizeof(PackShellHeader);
        if (headerFileOffset <= m_reader.DataSize() &&
            sizeof(PackShellHeader) <= m_reader.DataSize() - headerFileOffset) {
            memcpy(headerBuf, m_reader.Data() + headerFileOffset, sizeof(headerBuf));
            PackUtil::XorDecodeChained(headerBuf, sizeof(headerBuf), 0);
            headerFound = reinterpret_cast<const PackShellHeader*>(headerBuf)->Signature == 0xC0DEC0DF;
        }
    }

    // 路径 2：壳头在 TLS 回调前 sizeof(PackShellHeader) 字节处（与脱壳引擎 Step1 保持一致）。
    if (!headerFound) {
        auto tlsDir = m_reader.GetDataDirectory(IMAGE_DIRECTORY_ENTRY_TLS);
        if (tlsDir.VirtualAddress && tlsDir.Size >= sizeof(IMAGE_TLS_DIRECTORY32)) {
            const uint32_t toff = m_reader.RvaToOffset(tlsDir.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY32));
            if (toff) {
                const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY32*>(m_reader.Data() + toff);
                const uint32_t imageBase = nt->OptionalHeader.ImageBase;
                const uint32_t cboff = TlsVaToFileOffset(m_reader, imageBase, tls->AddressOfCallBacks, sizeof(uint32_t));
                if (cboff) {
                    uint32_t cbVa = 0;
                    memcpy(&cbVa, m_reader.Data() + cboff, sizeof(cbVa));
                    if (cbVa) {
                        const uint32_t cbOff = TlsVaToFileOffset(m_reader, imageBase, cbVa, 1u);
                        if (cbOff >= sizeof(PackShellHeader) &&
                            cbOff - sizeof(PackShellHeader) <= m_reader.DataSize() &&
                            sizeof(PackShellHeader) <= m_reader.DataSize() - (cbOff - sizeof(PackShellHeader))) {
                            const uint32_t headerOffset = cbOff - sizeof(PackShellHeader);
                            memcpy(headerBuf, m_reader.Data() + headerOffset, sizeof(headerBuf));
                            PackUtil::XorDecodeChained(headerBuf, sizeof(headerBuf), 0);
                            headerFound = reinterpret_cast<const PackShellHeader*>(headerBuf)->Signature == 0xC0DEC0DF;
                        }
                    }
                }
            }
        }
    }

    if (!headerFound) {
        m_lastError = L"头部魔数不匹配";
        return PackVariant::None;
    }

    auto* header = reinterpret_cast<PackShellHeader*>(headerBuf);
    if (header->XorKey == 0) {
        m_lastError = L"XorKey 为空";
        return PackVariant::None;
    }

    return PackVariant::V31x86;
}

const std::wstring& PackerDetector::GetLastError() const { return m_lastError; }
