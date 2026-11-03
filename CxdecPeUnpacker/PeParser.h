#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 轻量级 PE32 解析器，仅支持 x86
class PeReader {
public:
    explicit PeReader(const std::wstring& filePath);
    ~PeReader();

    bool Load();
    void Close();

    const IMAGE_DOS_HEADER*       DosHeader() const;
    const IMAGE_NT_HEADERS32*     NtHeaders() const;
    const IMAGE_SECTION_HEADER*   SectionHeader(const char* name) const;
    const IMAGE_SECTION_HEADER*   SectionHeader(size_t index) const;
    size_t                        SectionCount() const;

    // rva → 文件偏移。只返回**文件里真实存在**的位置（节虚拟填充区不算），
    // 并且会校验从该位置起 length 字节都在文件内；失败返回 0。
    uint32_t RvaToOffset(uint32_t rva, size_t length = 1) const;
    uint32_t OffsetToRva(uint32_t offset) const;

    const uint8_t* Data() const;
    size_t         DataSize() const;

    IMAGE_DATA_DIRECTORY GetDataDirectory(int index) const;

private:
    std::wstring          m_filePath;
    std::vector<uint8_t>  m_fileData;
    IMAGE_DOS_HEADER*     m_dosHeader;
    IMAGE_NT_HEADERS32*   m_ntHeaders;
    uint32_t              m_rvaCount = 0;   // OptionalHeader.NumberOfRvaAndSizes（已按 16 夹紧）
};

// PE32 重建器：从原始 PE 生成脱壳后的 PE
class PeRebuilder {
public:
    explicit PeRebuilder(const PeReader& source);

    void SetEntryPoint(uint32_t oep);
    bool ReplaceSectionData(const char* sectionName, const std::vector<uint8_t>& newData);
    bool ZeroSectionData(const char* sectionName);
    bool RemoveSection(const char* sectionName);
    void SetOverlayData(const std::vector<uint8_t>& overlay);
    void ZeroDosStub();
    bool Save(const std::wstring& outputPath);

private:
    const PeReader&      m_source;
    std::vector<uint8_t> m_outputData;
    IMAGE_DOS_HEADER*    m_dosHeader;
    IMAGE_NT_HEADERS32*  m_ntHeaders;
    bool                 m_zeroDosStub;
};
