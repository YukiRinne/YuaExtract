#include "pch.h"
#include "PeParser.h"

#include <cstddef>

// ---- PE 读取 ----

PeReader::PeReader(const std::wstring& filePath)
    : m_filePath(filePath), m_dosHeader(nullptr), m_ntHeaders(nullptr) {}

PeReader::~PeReader() { Close(); }

bool PeReader::Load()
{
    Close();
    HANDLE hFile = CreateFileW(m_filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < static_cast<LONGLONG>(sizeof(IMAGE_DOS_HEADER)) ||
        static_cast<unsigned long long>(fileSize.QuadPart) > static_cast<unsigned long long>(SIZE_MAX)) {
        CloseHandle(hFile); return false;
    }

    m_fileData.resize(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    const DWORD toRead = static_cast<DWORD>(m_fileData.size());
    if (!ReadFile(hFile, m_fileData.data(), toRead, &bytesRead, nullptr) || bytesRead != toRead) {
        CloseHandle(hFile); m_fileData.clear(); return false;
    }
    CloseHandle(hFile);

    const size_t size = m_fileData.size();
    m_dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(m_fileData.data());
    if (m_dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;

    const size_t e_lfanew = static_cast<size_t>(m_dosHeader->e_lfanew);
    if (e_lfanew < sizeof(IMAGE_DOS_HEADER) || e_lfanew > size) return false;
    if (size - e_lfanew < 4u + sizeof(IMAGE_FILE_HEADER)) return false;

    m_ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS32*>(m_fileData.data() + e_lfanew);
    if (m_ntHeaders->Signature != IMAGE_NT_SIGNATURE) return false;
    if (m_ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return false;

    const size_t optionalOffset = e_lfanew + 4u + sizeof(IMAGE_FILE_HEADER);
    const size_t optionalSize = m_ntHeaders->FileHeader.SizeOfOptionalHeader;
    constexpr size_t kMinimumOptional = offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
    if (optionalSize < kMinimumOptional || optionalSize > size - optionalOffset) return false;
    if (m_ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;

    uint32_t rvaCount = m_ntHeaders->OptionalHeader.NumberOfRvaAndSizes;
    if (rvaCount > IMAGE_NUMBEROF_DIRECTORY_ENTRIES) rvaCount = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    if (kMinimumOptional + static_cast<size_t>(rvaCount) * sizeof(IMAGE_DATA_DIRECTORY) > optionalSize) {
        return false;
    }
    m_rvaCount = rvaCount;

    const size_t sectionOffset = optionalOffset + optionalSize;
    const size_t sectionCount = m_ntHeaders->FileHeader.NumberOfSections;
    if (sectionCount > (size - sectionOffset) / sizeof(IMAGE_SECTION_HEADER)) return false;

    // 每个节的原始数据范围必须在文件里；否则后续 RVA→偏移会算出越界位置。
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(m_fileData.data() + sectionOffset);
    for (size_t i = 0; i < sectionCount; ++i) {
        const size_t rawStart = sections[i].PointerToRawData;
        const size_t rawSize = sections[i].SizeOfRawData;
        if (rawSize == 0) continue;
        if (rawStart > size || rawSize > size - rawStart) return false;
    }
    return true;
}

void PeReader::Close() {
    m_fileData.clear(); m_dosHeader = nullptr; m_ntHeaders = nullptr; m_rvaCount = 0;
}

const IMAGE_DOS_HEADER* PeReader::DosHeader() const { return m_dosHeader; }
const IMAGE_NT_HEADERS32* PeReader::NtHeaders() const { return m_ntHeaders; }

const IMAGE_SECTION_HEADER* PeReader::SectionHeader(const char* name) const
{
    if (!m_ntHeaders) return nullptr;
    auto* sections = IMAGE_FIRST_SECTION(m_ntHeaders);
    for (WORD i = 0; i < m_ntHeaders->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sections[i].Name, name, std::min<size_t>(strlen(name), IMAGE_SIZEOF_SHORT_NAME)) == 0)
            return &sections[i];
    }
    return nullptr;
}

const IMAGE_SECTION_HEADER* PeReader::SectionHeader(size_t index) const
{
    if (!m_ntHeaders || index >= m_ntHeaders->FileHeader.NumberOfSections) return nullptr;
    return &IMAGE_FIRST_SECTION(m_ntHeaders)[index];
}

size_t PeReader::SectionCount() const {
    return m_ntHeaders ? m_ntHeaders->FileHeader.NumberOfSections : 0;
}

uint32_t PeReader::RvaToOffset(uint32_t rva, size_t length) const
{
    if (!m_ntHeaders || m_fileData.empty()) return 0;
    auto* sections = IMAGE_FIRST_SECTION(m_ntHeaders);
    for (WORD i = 0; i < m_ntHeaders->FileHeader.NumberOfSections; ++i) {
        uint32_t vsize = sections[i].Misc.VirtualSize;
        if (vsize == 0) vsize = sections[i].SizeOfRawData;
        if (rva < sections[i].VirtualAddress) continue;
        const uint32_t delta = rva - sections[i].VirtualAddress;
        if (delta >= vsize) continue;

        // 只有落在节的**原始数据**里才有文件字节；虚拟填充区没有对应内容。
        if (delta >= sections[i].SizeOfRawData) return 0;
        const uint64_t offset = static_cast<uint64_t>(sections[i].PointerToRawData) + delta;
        if (offset >= m_fileData.size()) return 0;
        if (static_cast<uint64_t>(length) > m_fileData.size() - offset) return 0;
        return static_cast<uint32_t>(offset);
    }
    return 0;
}

uint32_t PeReader::OffsetToRva(uint32_t offset) const
{
    if (!m_ntHeaders) return 0;
    auto* sections = IMAGE_FIRST_SECTION(m_ntHeaders);
    for (WORD i = 0; i < m_ntHeaders->FileHeader.NumberOfSections; ++i) {
        if (offset >= sections[i].PointerToRawData &&
            offset < sections[i].PointerToRawData + sections[i].SizeOfRawData) {
            return offset - sections[i].PointerToRawData + sections[i].VirtualAddress;
        }
    }
    return 0;
}

const uint8_t* PeReader::Data() const { return m_fileData.data(); }
size_t PeReader::DataSize() const { return m_fileData.size(); }

IMAGE_DATA_DIRECTORY PeReader::GetDataDirectory(int index) const
{
    if (!m_ntHeaders || index < 0 || index >= IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        static_cast<uint32_t>(index) >= m_rvaCount) {
        return {};
    }
    return m_ntHeaders->OptionalHeader.DataDirectory[index];
}

// ---- PE 重建 ----

PeRebuilder::PeRebuilder(const PeReader& source)
    : m_source(source), m_dosHeader(nullptr), m_ntHeaders(nullptr), m_zeroDosStub(false)
{
    if (source.Data() != nullptr && source.DataSize() != 0) {
        m_outputData.assign(source.Data(), source.Data() + source.DataSize());
    }
    if (m_outputData.size() < sizeof(IMAGE_DOS_HEADER)) return;
    m_dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(m_outputData.data());
    const size_t e_lfanew = static_cast<size_t>(m_dosHeader->e_lfanew);
    if (e_lfanew > m_outputData.size() || m_outputData.size() - e_lfanew < sizeof(IMAGE_NT_HEADERS32)) return;
    m_ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS32*>(m_outputData.data() + e_lfanew);
}

void PeRebuilder::SetEntryPoint(uint32_t oep) {
    if (m_ntHeaders) m_ntHeaders->OptionalHeader.AddressOfEntryPoint = oep;
}

bool PeRebuilder::ReplaceSectionData(const char* sectionName, const std::vector<uint8_t>& newData)
{
    if (!m_ntHeaders) return false;

    auto* srcSection = m_source.SectionHeader(sectionName);
    if (!srcSection) return false;
    uint32_t fileOffset = srcSection->PointerToRawData;
    if (fileOffset + newData.size() > m_outputData.size()) return false;
    memcpy(m_outputData.data() + fileOffset, newData.data(), newData.size());
    return true;
}

bool PeRebuilder::ZeroSectionData(const char* sectionName)
{
    auto* section = m_source.SectionHeader(sectionName);
    if (!section || !m_ntHeaders) return false;
    uint32_t fileOffset = section->PointerToRawData;
    uint32_t rawSize = section->SizeOfRawData;
    if (fileOffset + rawSize > m_outputData.size()) return false;
    memset(m_outputData.data() + fileOffset, 0, rawSize);
    return true;
}

bool PeRebuilder::RemoveSection(const char* sectionName)
{
    if (!m_ntHeaders) return false;
    auto* sections = IMAGE_FIRST_SECTION(m_ntHeaders);
    WORD numSections = m_ntHeaders->FileHeader.NumberOfSections;
    WORD targetIndex = 0xFFFF;
    for (WORD i = 0; i < numSections; ++i) {
        if (memcmp(sections[i].Name, sectionName, std::min<size_t>(strlen(sectionName), IMAGE_SIZEOF_SHORT_NAME)) == 0) {
            targetIndex = i; break;
        }
    }
    if (targetIndex == 0xFFFF) return false;
    for (WORD i = targetIndex; i < numSections - 1; ++i)
        sections[i] = sections[i + 1];
    memset(&sections[numSections - 1], 0, sizeof(IMAGE_SECTION_HEADER));
    m_ntHeaders->FileHeader.NumberOfSections--;
    return true;
}

void PeRebuilder::SetOverlayData(const std::vector<uint8_t>& overlay)
{
    // 仅当 overlay 非空时追加重叠数据
    if (!overlay.empty())
        m_outputData.insert(m_outputData.end(), overlay.begin(), overlay.end());
}

void PeRebuilder::ZeroDosStub() { m_zeroDosStub = true; }

bool PeRebuilder::Save(const std::wstring& outputPath)
{
    if (m_outputData.empty()) return false;

    if (m_zeroDosStub && m_dosHeader && m_ntHeaders) {
        uint32_t stubStart = sizeof(IMAGE_DOS_HEADER);
        uint32_t stubEnd   = static_cast<uint32_t>(m_dosHeader->e_lfanew);
        if (stubEnd > stubStart)
            memset(m_outputData.data() + stubStart, 0, stubEnd - stubStart);
    }

    HANDLE hFile = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(hFile, m_outputData.data(), static_cast<DWORD>(m_outputData.size()), &bytesWritten, nullptr);
    CloseHandle(hFile);
    return ok && bytesWritten == m_outputData.size();
}
