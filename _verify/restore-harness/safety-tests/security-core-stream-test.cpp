// security-core-stream-test
// 回归：CxdecExtractor::TryDecryptText 的短读/Seek 失败/偶数字节边界。
// 用内存 IStream，不接触真实游戏文件。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objidl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../../CxdecExtractor/ExtractCore.h"

namespace Engine {
struct ExtractCoreTestAccess
{
    static bool TryDecryptText(IStream* stream, std::vector<uint8_t>& out)
    {
        return ExtractCore::TryDecryptText(stream, out);
    }

    static bool ExtractFile(ExtractCore& core, IStream* stream, const std::wstring& path)
    {
        return core.ExtractFile(stream, path, L"fixture");
    }
};
}

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


class MemoryStream : public IStream
{
public:
    MemoryStream(std::vector<uint8_t> bytes, ULONGLONG declaredSize = 0, bool failSeek = false,
                 bool seekReturnsFalse = false)
        : bytes_(std::move(bytes)), declaredSize_(declaredSize == 0 ? bytes_.size() : declaredSize),
          failSeek_(failSeek), seekReturnsFalse_(seekReturnsFalse) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override {
        if (!object) return E_POINTER;
        *object = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }

    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG count, ULONG* read) override {
        if (read) *read = 0;
        if (!buffer && count) return E_POINTER;
        const ULONGLONG available = position_ < bytes_.size() ? bytes_.size() - position_ : 0;
        const ULONG take = static_cast<ULONG>(available < count ? available : count);
        if (take) std::memcpy(buffer, bytes_.data() + position_, take);
        position_ += take;
        if (read) *read = take;
        return take == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override {
        if (failSeek_) return STG_E_ACCESSDENIED;
        if (seekReturnsFalse_) return S_FALSE;
        LONGLONG base = 0;
        if (origin == STREAM_SEEK_CUR) base = static_cast<LONGLONG>(position_);
        else if (origin == STREAM_SEEK_END) base = static_cast<LONGLONG>(declaredSize_);
        else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
        const LONGLONG next = base + move.QuadPart;
        if (next < 0) return STG_E_INVALIDFUNCTION;
        position_ = static_cast<ULONGLONG>(next);
        if (newPos) newPos->QuadPart = position_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (!stat) return E_POINTER;
        std::memset(stat, 0, sizeof(*stat));
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = declaredSize_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    std::vector<uint8_t> bytes_;
    ULONGLONG declaredSize_ = 0;
    ULONGLONG position_ = 0;
    ULONG refs_ = 1;
    bool failSeek_ = false;
    bool seekReturnsFalse_ = false;
};

std::vector<uint8_t> Header(uint8_t mode) {
    return {0xFE, 0xFE, mode, 0xFF, 0xFE};
}

void AppendUtf16(std::vector<uint8_t>& out, wchar_t ch) {
    out.push_back(static_cast<uint8_t>(ch));
    out.push_back(static_cast<uint8_t>(ch >> 8));
}

std::vector<uint8_t> MakeMode0(const wchar_t* text) {
    std::vector<uint8_t> data = Header(0);
    for (const wchar_t* p = text; *p; ++p) AppendUtf16(data, *p);
    return data;
}

} // namespace

int wmain() {
    {
        std::vector<uint8_t> out;
        MemoryStream stream(std::vector<uint8_t>{0xFE});
        Check(!Engine::ExtractCoreTestAccess::TryDecryptText(&stream, out),
              L"1-byte short stream rejected");
    }
    {
        std::vector<uint8_t> out;
        MemoryStream stream(std::vector<uint8_t>{0xFE, 0xFE});
        Check(!Engine::ExtractCoreTestAccess::TryDecryptText(&stream, out),
              L"missing mode byte rejected");
    }
    {
        std::vector<uint8_t> out;
        MemoryStream stream(std::vector<uint8_t>{0xFE, 0xFE, 0x03, 0xFF, 0xFE, 0x41, 0x00});
        Check(!Engine::ExtractCoreTestAccess::TryDecryptText(&stream, out),
              L"invalid mode rejected");
    }
    {
        std::vector<uint8_t> data = Header(0);
        data.insert(data.end(), {0x41, 0x00, 0x42});
        std::vector<uint8_t> out;
        MemoryStream stream(data);
        Check(!Engine::ExtractCoreTestAccess::TryDecryptText(&stream, out),
              L"odd payload bytes rejected");
    }
    {
        // 声明长度大于实际可读：旧实现会零填后成功。
        std::vector<uint8_t> data = Header(0);
        data.insert(data.end(), {0x41, 0x00});
        std::vector<uint8_t> out;
        MemoryStream stream(data, data.size() + 8);
        Check(!Engine::ExtractCoreTestAccess::TryDecryptText(&stream, out),
              L"short IStream vs declared length rejected");
    }
    {
        std::vector<uint8_t> out;
        MemoryStream stream(MakeMode0(L"AB"), 0, true);
        Check(!Engine::ExtractCoreTestAccess::TryDecryptText(&stream, out),
              L"seek failure rejected");
    }
    {
        std::vector<uint8_t> out;
        MemoryStream stream(MakeMode0(L"AB"));
        const bool ok = Engine::ExtractCoreTestAccess::TryDecryptText(&stream, out);
        Check(ok && out.size() == 6u && out[0] == 0xFF && out[1] == 0xFE,
              L"mode 0 valid header output");
        if (ok) {
            wchar_t a = 0x41;
            const wchar_t expectedA = a >= 0x20 ? (wchar_t)(a ^ (((a & 0xFE) << 8) ^ 1)) : a;
            wchar_t gotA = static_cast<wchar_t>(out[2] | (out[3] << 8));
            Check(gotA == expectedA, L"mode 0 transform applied");
        }
    }
    {
        std::vector<uint8_t> data = Header(1);
        AppendUtf16(data, 0x0041);
        AppendUtf16(data, 0x0042);
        std::vector<uint8_t> out;
        MemoryStream stream(data);
        const bool ok = Engine::ExtractCoreTestAccess::TryDecryptText(&stream, out);
        Check(ok && out.size() == 6u, L"mode 1 valid output size");
        if (ok) {
            const uint16_t got = static_cast<uint16_t>(out[2] | (out[3] << 8));
            const uint16_t expected = static_cast<uint16_t>(((0x0041 & 0xAAAA) >> 1) | ((0x0041 & 0x5555) << 1));
            Check(got == expected, L"mode 1 swap applied");
        }
    }
    {
        // mode 2 头部不足（缺少两个 8 字节长度）：必须失败，不能读到未初始化零。
        std::vector<uint8_t> data = Header(2);
        data.insert(data.end(), {0x01, 0x02, 0x03});
        std::vector<uint8_t> out;
        MemoryStream stream(data);
        Check(!Engine::ExtractCoreTestAccess::TryDecryptText(&stream, out),
              L"mode 2 truncated header rejected");
    }
    {
        // mode 2 声明 64 MiB 以上：不先按声明分配，直接失败。
        std::vector<uint8_t> data = Header(2);
        const uint64_t huge = 65ull * 1024ull * 1024ull;
        for (int i = 0; i < 8; ++i) data.push_back(static_cast<uint8_t>(huge >> (8 * i)));
        const uint64_t smallValue = 4;
        for (int i = 0; i < 8; ++i) data.push_back(static_cast<uint8_t>(smallValue >> (8 * i)));
        std::vector<uint8_t> out;
        MemoryStream stream(data);
        Check(!Engine::ExtractCoreTestAccess::TryDecryptText(&stream, out),
              L"mode 2 huge declared sizes rejected before allocation");
    }
    {
        Engine::ExtractCore core;
        Check(!Engine::ExtractCoreTestAccess::ExtractFile(core, nullptr, L""),
              L"ExtractFile rejects null stream");
        MemoryStream failSeek(std::vector<uint8_t>{1, 2, 3, 4}, 0, true);
        Check(!Engine::ExtractCoreTestAccess::ExtractFile(core, &failSeek, L""),
              L"ExtractFile rejects length/seek failure");
        MemoryStream falseSeek(std::vector<uint8_t>{1, 2, 3, 4}, 0, false, true);
        Check(!Engine::ExtractCoreTestAccess::ExtractFile(core, &falseSeek, L""),
              L"ExtractFile treats Seek S_FALSE as failure");
    }

    std::printf("RESULT checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
