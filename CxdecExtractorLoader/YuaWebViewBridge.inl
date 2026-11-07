// YuaExtract 主界面的 WebView2 桥（实现文件，被 YuaExtractMainWindow.cpp 包含）。
//
// 放在 .inl 而不是独立 .cpp：这里要用到 YuaExtractMainWindow.cpp 匿名命名空间里的
// UiContext 与全部动作函数（OnStep1 / OnSetGenerate / OnAuxClean ...）。另起一个 cpp
// 就得把这些全部导出成公开接口，改动面反而更大。
//
// 设计约束（与 ARCHITECTURE.md §1.2 一致）：
//   · 所有 ICoreWebView2* 调用都只发生在 UI 线程（窗口过程里）；后台线程只往
//     CRITICAL_SECTION 保护的队列里塞 JSON，再 PostMessage 叫醒 UI 线程。
//   · 只信任 https://yua.local/ 这一个源；导航 / 新窗口 / 资源 / 消息四处都查。
//   · 外壳缺失、WebView2 起不来时**退回纯 Win32 界面**，不伪造任何宿主事件。
//   · 传给 JS 的每条事件都是 C++ 侧的真实状态（探测结果 / 任务进度 / 库清单），
//     前端据此渲染；这里不造演示数据。

#include "RestoreReportStats.h"

namespace Bridge
{
    constexpr UINT kPushMessage = WM_APP + 0x5D0; // UI 线程派发队列里的 HostEvent
    constexpr UINT kWorkMessage = WM_APP + 0x5D1; // 后台网络任务完成，回 UI 线程收尾
    constexpr size_t kMaxMessageBytes = 262144;   // 单条 JS→C++ 消息上限（256 KiB）
    constexpr wchar_t kVirtualHost[] = L"yua.local";
    constexpr wchar_t kOriginPrefix[] = L"https://yua.local/";
    constexpr wchar_t kHostClass[] = L"YuaExtractWebHostWindow";
    constexpr int kLegacyOffsetX = -32000;

    // ---------- UTF-8 / UTF-16 ----------

    std::string ToUtf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return std::string();
        }
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (size <= 0)
        {
            return std::string();
        }
        std::string out(static_cast<size_t>(size), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, nullptr,
                              nullptr);
        return out;
    }

    std::wstring ToWide(const std::string& text)
    {
        if (text.empty())
        {
            return std::wstring();
        }
        const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0)
        {
            return std::wstring();
        }
        std::wstring out(static_cast<size_t>(size), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
        return out;
    }

    void AppendUtf8CodePoint(std::string& out, unsigned int code)
    {
        if (code < 0x80u)
        {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800u)
        {
            out.push_back(static_cast<char>(0xC0u | (code >> 6)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        }
        else if (code < 0x10000u)
        {
            out.push_back(static_cast<char>(0xE0u | (code >> 12)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0u | (code >> 18)));
            out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        }
    }

    // ---------- 极简 JSON（只覆盖本协议用得到的子集） ----------
    //
    // 不引第三方库：请求体是我们自己前端发出的扁平对象，事件体由本文件拼装，
    // 语法范围固定（对象 / 数组 / 字符串 / 数字 / 布尔 / null）。

    struct JsonValue
    {
        enum class Kind
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object
        };

        Kind kind = Kind::Null;
        bool boolean = false;
        double number = 0.0;
        std::string text; // UTF-8
        std::vector<JsonValue> items;
        // 对象成员用两个等长数组而不是 vector<pair<string, JsonValue>>：
        // JsonValue 在自身定义内部还是不完整类型，vector 允许，pair 不保证。
        std::vector<std::string> keys;
        std::vector<JsonValue> values;

        const JsonValue* Member(const char* key) const
        {
            for (size_t index = 0; index < keys.size() && index < values.size(); ++index)
            {
                if (keys[index] == key)
                {
                    return &values[index];
                }
            }
            return nullptr;
        }

        std::wstring MemberText(const char* key) const
        {
            const JsonValue* value = Member(key);
            return value != nullptr && value->kind == Kind::String ? ToWide(value->text) : std::wstring();
        }

        bool HasMember(const char* key) const { return Member(key) != nullptr; }

        bool MemberBool(const char* key, bool fallback) const
        {
            const JsonValue* value = Member(key);
            return value != nullptr && value->kind == Kind::Bool ? value->boolean : fallback;
        }

        double MemberNumber(const char* key, double fallback) const
        {
            const JsonValue* value = Member(key);
            return value != nullptr && value->kind == Kind::Number ? value->number : fallback;
        }
    };

    struct JsonParser
    {
        const std::string& Text;
        size_t Pos = 0;
        int Depth = 0;

        explicit JsonParser(const std::string& text) : Text(text) {}

        void SkipSpace()
        {
            while (Pos < Text.size())
            {
                const char ch = Text[Pos];
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
                {
                    ++Pos;
                }
                else
                {
                    break;
                }
            }
        }

        bool Literal(const char* word)
        {
            const size_t length = ::strlen(word);
            if (Text.compare(Pos, length, word) != 0)
            {
                return false;
            }
            Pos += length;
            return true;
        }

        bool ParseString(std::string& out)
        {
            if (Pos >= Text.size() || Text[Pos] != '"')
            {
                return false;
            }
            ++Pos;
            out.clear();
            while (Pos < Text.size())
            {
                const char ch = Text[Pos++];
                if (ch == '"')
                {
                    return true;
                }
                if (ch != '\\')
                {
                    out.push_back(ch);
                    continue;
                }
                if (Pos >= Text.size())
                {
                    return false;
                }
                const char escape = Text[Pos++];
                switch (escape)
                {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                    {
                        if (Pos + 4 > Text.size())
                        {
                            return false;
                        }
                        unsigned int code = 0;
                        for (int i = 0; i < 4; ++i)
                        {
                            const char digit = Text[Pos++];
                            code <<= 4;
                            if (digit >= '0' && digit <= '9')
                            {
                                code |= static_cast<unsigned int>(digit - '0');
                            }
                            else if (digit >= 'a' && digit <= 'f')
                            {
                                code |= static_cast<unsigned int>(digit - 'a' + 10);
                            }
                            else if (digit >= 'A' && digit <= 'F')
                            {
                                code |= static_cast<unsigned int>(digit - 'A' + 10);
                            }
                            else
                            {
                                return false;
                            }
                        }
                        // 代理对：高代理后面紧跟 \uDC00-\uDFFF 才算一个码点。
                        if (code >= 0xD800u && code <= 0xDBFFu && Pos + 6 <= Text.size() &&
                            Text[Pos] == '\\' && Text[Pos + 1] == 'u')
                        {
                            Pos += 2;
                            unsigned int low = 0;
                            for (int i = 0; i < 4; ++i)
                            {
                                const char digit = Text[Pos++];
                                low <<= 4;
                                if (digit >= '0' && digit <= '9')
                                {
                                    low |= static_cast<unsigned int>(digit - '0');
                                }
                                else if (digit >= 'a' && digit <= 'f')
                                {
                                    low |= static_cast<unsigned int>(digit - 'a' + 10);
                                }
                                else if (digit >= 'A' && digit <= 'F')
                                {
                                    low |= static_cast<unsigned int>(digit - 'A' + 10);
                                }
                                else
                                {
                                    return false;
                                }
                            }
                            if (low >= 0xDC00u && low <= 0xDFFFu)
                            {
                                code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                            }
                        }
                        AppendUtf8CodePoint(out, code);
                        break;
                    }
                    default:
                        return false;
                }
            }
            return false;
        }

        bool ParseValue(JsonValue& out)
        {
            if (Depth > 24)
            {
                return false;
            }
            SkipSpace();
            if (Pos >= Text.size())
            {
                return false;
            }
            const char ch = Text[Pos];
            if (ch == '{')
            {
                ++Pos;
                ++Depth;
                out.kind = JsonValue::Kind::Object;
                SkipSpace();
                if (Pos < Text.size() && Text[Pos] == '}')
                {
                    ++Pos;
                    --Depth;
                    return true;
                }
                for (;;)
                {
                    SkipSpace();
                    std::string key;
                    if (!ParseString(key))
                    {
                        return false;
                    }
                    SkipSpace();
                    if (Pos >= Text.size() || Text[Pos] != ':')
                    {
                        return false;
                    }
                    ++Pos;
                    JsonValue value;
                    if (!ParseValue(value))
                    {
                        return false;
                    }
                    out.keys.emplace_back(std::move(key));
                    out.values.push_back(std::move(value));
                    SkipSpace();
                    if (Pos < Text.size() && Text[Pos] == ',')
                    {
                        ++Pos;
                        continue;
                    }
                    if (Pos < Text.size() && Text[Pos] == '}')
                    {
                        ++Pos;
                        --Depth;
                        return true;
                    }
                    return false;
                }
            }
            if (ch == '[')
            {
                ++Pos;
                ++Depth;
                out.kind = JsonValue::Kind::Array;
                SkipSpace();
                if (Pos < Text.size() && Text[Pos] == ']')
                {
                    ++Pos;
                    --Depth;
                    return true;
                }
                for (;;)
                {
                    JsonValue value;
                    if (!ParseValue(value))
                    {
                        return false;
                    }
                    out.items.push_back(std::move(value));
                    SkipSpace();
                    if (Pos < Text.size() && Text[Pos] == ',')
                    {
                        ++Pos;
                        continue;
                    }
                    if (Pos < Text.size() && Text[Pos] == ']')
                    {
                        ++Pos;
                        --Depth;
                        return true;
                    }
                    return false;
                }
            }
            if (ch == '"')
            {
                out.kind = JsonValue::Kind::String;
                return ParseString(out.text);
            }
            if (ch == 't')
            {
                out.kind = JsonValue::Kind::Bool;
                out.boolean = true;
                return Literal("true");
            }
            if (ch == 'f')
            {
                out.kind = JsonValue::Kind::Bool;
                out.boolean = false;
                return Literal("false");
            }
            if (ch == 'n')
            {
                out.kind = JsonValue::Kind::Null;
                return Literal("null");
            }
            // 数字：协议里只用整数（step / threads / protocolVersion）。
            const size_t start = Pos;
            if (ch == '-')
            {
                ++Pos;
            }
            bool digits = false;
            while (Pos < Text.size() && Text[Pos] >= '0' && Text[Pos] <= '9')
            {
                ++Pos;
                digits = true;
            }
            if (Pos < Text.size() && Text[Pos] == '.')
            {
                ++Pos;
                while (Pos < Text.size() && Text[Pos] >= '0' && Text[Pos] <= '9')
                {
                    ++Pos;
                }
            }
            if (!digits)
            {
                Pos = start;
                return false;
            }
            out.kind = JsonValue::Kind::Number;
            out.number = ::strtod(Text.substr(start, Pos - start).c_str(), nullptr);
            return true;
        }
    };

    bool ParseJson(const std::string& text, JsonValue& out)
    {
        JsonParser parser(text);
        if (!parser.ParseValue(out))
        {
            return false;
        }
        parser.SkipSpace();
        return parser.Pos == text.size();
    }

    // ---------- JSON 输出 ----------

    std::string JsonString(const std::wstring& value)
    {
        const std::string utf8 = ToUtf8(value);
        std::string out;
        out.reserve(utf8.size() + 2);
        out.push_back('"');
        for (char ch : utf8)
        {
            const unsigned char byte = static_cast<unsigned char>(ch);
            switch (byte)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (byte < 0x20u)
                    {
                        char buffer[8]{};
                        ::sprintf_s(buffer, "\\u%04x", static_cast<unsigned int>(byte));
                        out += buffer;
                    }
                    else
                    {
                        out.push_back(ch);
                    }
                    break;
            }
        }
        out.push_back('"');
        return out;
    }

    std::string JsonString(const wchar_t* value) { return JsonString(std::wstring(value ? value : L"")); }

    std::string JsonNumber(long long value)
    {
        char buffer[32]{};
        ::sprintf_s(buffer, "%lld", value);
        return buffer;
    }

    std::string JsonBool(bool value) { return value ? std::string("true") : std::string("false"); }

    std::string JsonDouble(double value)
    {
        char buffer[64]{};
        ::sprintf_s(buffer, "%.4f", value);
        // 去掉多余的尾零，避免 0.5000 这种前端格式化时看着别扭。
        std::string text = buffer;
        if (text.find('.') != std::string::npos)
        {
            while (!text.empty() && text.back() == '0')
            {
                text.pop_back();
            }
            if (!text.empty() && text.back() == '.')
            {
                text.pop_back();
            }
        }
        return text.empty() ? std::string("0") : text;
    }

    std::string JsonObject(const std::vector<std::pair<std::string, std::string>>& members)
    {
        std::string out = "{";
        bool first = true;
        for (const auto& member : members)
        {
            if (!first)
            {
                out += ",";
            }
            first = false;
            out += JsonString(ToWide(member.first));
            out += ":";
            out += member.second;
        }
        out += "}";
        return out;
    }

    std::string JsonArray(const std::vector<std::string>& items)
    {
        std::string out = "[";
        for (size_t index = 0; index < items.size(); ++index)
        {
            if (index != 0)
            {
                out += ",";
            }
            out += items[index];
        }
        out += "]";
        return out;
    }

    long long NowMilliseconds()
    {
        FILETIME fileTime{};
        ::GetSystemTimeAsFileTime(&fileTime);
        const unsigned long long ticks =
            (static_cast<unsigned long long>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
        return static_cast<long long>(ticks / 10000ull) - 11644473600000ll;
    }

    // ---------- 状态 ----------

    struct State
    {
        bool forcedLegacy = false;
        bool attached = false;
        bool comInitialized = false;
        bool hostCreated = false;
        bool ready = false;
        bool resourceFilterAdded = false;
        HWND main = nullptr;
        HWND host = nullptr;
        ICoreWebView2Environment* environment = nullptr;
        ICoreWebView2Controller* controller = nullptr;
        ICoreWebView2* webview = nullptr;
        UiContext* context = nullptr;
        CRITICAL_SECTION lock{};
        bool lockReady = false;
        std::vector<std::string> pending;
        std::wstring lastWorkspace;
        std::wstring lastLibrary;
        // 最近一次**成功**拉取的远端索引（仅 UI 线程读写，不加新锁；失败不清空旧值）。
        // remoteCatalogSnapshot 是拉索引那一刻的本地库快照，供 downloadRemote 里的
        // Online::Classify 使用，保证「列表显示的状态」和「下载时判定的状态」同一口径。
        std::vector<ExtensionSet::LibraryEntry> remoteEntries;
        ExtensionSet::LibraryCatalog remoteCatalogSnapshot;
    };

    State& S()
    {
        static State state;
        return state;
    }

    bool UsingWebView()
    {
        return S().hostCreated && !S().forcedLegacy;
    }

    int LayoutOffsetX()
    {
        return UsingWebView() ? kLegacyOffsetX : 0;
    }

    // ---------- 发出事件（任意线程） ----------

    void Emit(const std::string& json)
    {
        State& state = S();
        if (!state.attached || !state.hostCreated || state.main == nullptr || !state.lockReady)
        {
            return;
        }
        ::EnterCriticalSection(&state.lock);
        state.pending.push_back(json);
        ::LeaveCriticalSection(&state.lock);
        ::PostMessageW(state.main, kPushMessage, 0, 0);
    }

    // 队列在 UI 线程排空，只有这里碰 ICoreWebView2::PostWebMessageAsJson。
    void HandlePush(UiContext& context)
    {
        (void)context;
        State& state = S();
        if (!state.lockReady)
        {
            return;
        }
        std::vector<std::string> batch;
        ::EnterCriticalSection(&state.lock);
        batch.swap(state.pending);
        ::LeaveCriticalSection(&state.lock);

        if (!state.ready || state.webview == nullptr)
        {
            return;
        }
        for (const std::string& json : batch)
        {
            state.webview->PostWebMessageAsJson(ToWide(json).c_str());
        }
    }

    // ---------- 快照 ----------

    std::wstring FileLabel(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    const wchar_t* ModeText(ActiveMode mode)
    {
        switch (mode)
        {
            case ActiveMode::SetExtract: return L"extension";
            case ActiveMode::SetCrack: return L"studio";
            default: return L"classic";
        }
    }

    const wchar_t* JobKindText(JobKind kind)
    {
        switch (kind)
        {
            case JobKind::Generate: return L"generate-set";
            case JobKind::StaticHash: return L"static-table";
            case JobKind::DryRun: return L"dry-run";
            default: return L"legacy";
        }
    }

    // ---------- 还原报告统计 ----------
    //
    // RestoreReport.txt 只有还原窗口会写；主界面只读它开头 64 KiB 抽统计三行
    // （哈希名文件 / 已还原 / 还原成功率，口径见 RestoreReportStats.h）。
    // 没有报告是常态（还没跑过还原），不算失败；报告存在但读不出 / 解析不出时
    // 如实报 absent（上层写 null/0），绝不编数字。

    struct RestoreStats
    {
        bool present = false;
        unsigned int restored = 0u;
        unsigned int hashed = 0u;
        unsigned int ratePercent = 0u;
    };

    RestoreStats LoadRestoreStats(const std::wstring& path, bool logFailures)
    {
        RestoreStats stats;
        if (path.empty() ||
            ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return stats;
        }
        const std::wstring text = Yua::ReadRestoreReportHead(path);
        bool ok = false;
        const wchar_t* why = L"";
        if (text.empty())
        {
            why = L"读不出内容";
        }
        else
        {
            Yua::ParseRestoreReportStats(text, stats.present, stats.restored, stats.hashed, stats.ratePercent);
            ok = stats.present;
            if (!ok)
            {
                why = L"统计三行解析不出";
            }
        }
        if (logFailures)
        {
            // 快照会被定时器反复推：同一份报告的失败只记一行，恢复成功后复位。
            // 本函数只在 UI 线程跑，函数内静态标记是安全的。
            static std::wstring lastFailedPath;
            static bool lastFailureLogged = false;
            if (!ok)
            {
                if (!lastFailureLogged || lastFailedPath != path)
                {
                    lastFailedPath = path;
                    lastFailureLogged = true;
                    Host::LogLine(Log::Level::Warn, std::wstring(L"[YuaExtract] 还原报告") + why + L"：" + path);
                }
            }
            else
            {
                lastFailureLogged = false;
            }
        }
        return stats;
    }

    std::string WorkspaceJson(const UiContext& context)
    {
        const bool reportExists = !context.Paths.RestoreReport.empty() &&
                                  ::GetFileAttributesW(context.Paths.RestoreReport.c_str()) !=
                                      INVALID_FILE_ATTRIBUTES;
        const wchar_t* attribution = L"none";
        if (context.LastLoadVerdict == LoadVerdict::Verified)
        {
            attribution = L"passed";
        }
        else if (context.LastLoadVerdict == LoadVerdict::Unverified)
        {
            attribution = L"forced";
        }
        else if (context.LastLoadVerdict == LoadVerdict::Rejected)
        {
            attribution = L"rejected";
        }

        const std::wstring title = context.Paths.GameExe.empty()
                                       ? std::wstring()
                                       : [&context]() {
                                             std::wstring name = FileLabel(context.Paths.GameExe);
                                             if (name.size() > 4u &&
                                                 _wcsicmp(name.c_str() + name.size() - 4u, L".exe") == 0)
                                             {
                                                 name.resize(name.size() - 4u);
                                             }
                                             return name;
                                         }();

        // 还原率只有还原窗口自己知道（它写 RestoreReport.txt）。报告存在且统计三行
        // 解析得出来时取真实数字；不存在或解析失败如实报 null/0，前端显示"尚未开始"，
        // 不编造百分比。
        const RestoreStats restore = LoadRestoreStats(context.Paths.RestoreReport, true);

        const std::string snapshot = JsonObject({
            { "type", JsonString(L"workspace") },
            { "workspace",
              JsonObject({
                  { "exe", context.Paths.GameExe.empty() ? "null" : JsonString(context.Paths.GameExe) },
                  { "title", title.empty() ? "null" : JsonString(title) },
                  { "publisher",
                    context.SelectedPublisher.empty() ? "null" : JsonString(context.SelectedPublisher) },
                  { "workspace", context.Paths.GameDir.empty() ? "null" : JsonString(context.Paths.GameDir) },
                  { "extractDir", context.Paths.ExtractDir.empty() ? "null" : JsonString(context.Paths.ExtractDir) },
                  { "hashDir", context.Paths.HashDir.empty() ? "null" : JsonString(context.Paths.HashDir) },
                  { "restoreDir", context.Paths.RestoreDir.empty() ? "null" : JsonString(context.Paths.RestoreDir) },
                  { "reportPath",
                    context.Paths.RestoreReport.empty() ? "null" : JsonString(context.Paths.RestoreReport) },
                  { "staticHashDir", context.StaticHashDir.empty() ? "null" : JsonString(context.StaticHashDir) },
                  { "extractReady", JsonBool(context.ExtractProbe.exists) },
                  { "hashReady",
                    JsonBool(context.HashProbe.exists &&
                             (context.Mapping.directoryEntries > 0 || context.Mapping.fileNameEntries > 0)) },
                  { "hashDirEntries", JsonNumber(static_cast<long long>(context.HashProbe.topLevelEntries)) },
                  { "hashFileEntries",
                    JsonNumber(static_cast<long long>(context.Mapping.directoryEntries +
                                                      context.Mapping.fileNameEntries)) },
                  { "restoreReady", JsonBool(context.RestoreProbe.exists) },
                  { "staticReady", JsonBool(StaticHashReady(context)) },
                  { "attribution", JsonString(attribution) },
                  { "restoreRate",
                    restore.present ? JsonDouble(restore.ratePercent / 100.0) : std::string("null") },
                  { "restoredCount",
                    restore.present ? JsonNumber(static_cast<long long>(restore.restored)) : JsonNumber(0) },
                  { "hashedCount",
                    restore.present ? JsonNumber(static_cast<long long>(restore.hashed)) : JsonNumber(0) },
                  { "reportReady", JsonBool(reportExists) },
              }) },
        });
        return snapshot;
    }

    std::string LibraryEntryJson(const ExtensionSet::LibraryEntry& entry)
    {
        return JsonObject({
            { "id", JsonString(entry.publisher + L"/" + entry.title) },
            { "publisher", JsonString(entry.publisher) },
            { "title", JsonString(entry.title) },
            // 档案 META（贡献者/版本/留言）只在 rules.int 里；列表页用不到，
            // 需要时走 notes（readNotes）读全文，避免每次刷新库都解一遍所有档案。
            { "contributor", JsonString(L"") },
            { "version", JsonString(L"") },
            { "date", JsonString(L"") },
            { "comment", JsonString(L"") },
            { "dirCount", JsonNumber(static_cast<long long>(entry.directoryCount)) },
            { "fileCount", JsonNumber(static_cast<long long>(entry.fileCount)) },
            { "exceptions", JsonNumber(static_cast<long long>(entry.exceptionCount)) },
            { "archiveSha", JsonString(entry.archiveDigest) },
            { "targetSha", JsonString(entry.exeDigest) },
            { "siblingSha", JsonString(entry.siblingDigest) },
            { "saltSha", JsonString(entry.saltDigest) },
            { "generatedAt", JsonString(entry.generatedAt) },
            { "installedAt", JsonString(entry.installedAt) },
            { "source", JsonString(entry.source) },
            { "path", JsonString(entry.archivePath) },
        });
    }

    std::string LibraryJson(const UiContext& context)
    {
        std::vector<std::string> entries;
        entries.reserve(context.Catalog.entries.size());
        for (const auto& entry : context.Catalog.entries)
        {
            entries.push_back(LibraryEntryJson(entry));
        }
        return JsonObject({
            { "type", JsonString(L"library") },
            { "entries", JsonArray(entries) },
        });
    }

    std::string HelloJson(const UiContext& context)
    {
        const BuildFingerprint& fingerprint = SelfFingerprint();
        return JsonObject({
            { "type", JsonString(L"hello") },
            { "info",
              JsonObject({
                  { "product", JsonString(Host::kProductName) },
                  { "version", JsonString(Host::kProductVersion) },
                  { "loaderDir", JsonString(Host::LoaderDirectory()) },
                  { "logDir", JsonString(Host::LogDirectory()) },
                  { "moduleDir", JsonString(JoinPath(Host::LoaderDirectory(), L"CxdecExtractordll")) },
                  { "libraryRoot", JsonString(context.LibraryRoot) },
                  { "fingerprint",
                    JsonObject({
                        { "fileName", JsonString(fingerprint.FileName) },
                        { "size", JsonNumber(static_cast<long long>(fingerprint.Size)) },
                        { "modified", JsonString(fingerprint.Modified) },
                        { "sha256", JsonString(fingerprint.Sha256) },
                    }) },
                  { "onlineBase", JsonString(Online::ResolveRepositoryBase()) },
              }) },
        });
    }

    std::string UiSyncJson(const std::vector<std::pair<std::string, std::string>>& patch)
    {
        return JsonObject({
            { "type", JsonString(L"uiSync") },
            { "patch", JsonObject(patch) },
        });
    }

    std::string TabIdText(int tabId)
    {
        switch (tabId)
        {
            case IDC_YUA_TAB_MAKE_SET: return "make";
            case IDC_YUA_TAB_SET: return "library";
            case IDC_YUA_TAB_ABOUT: return "about";
            default: return "home";
        }
    }

    std::wstring LogLevelText(Log::Level level)
    {
        switch (level)
        {
            case Log::Level::Debug: return L"debug";
            case Log::Level::Warn: return L"warn";
            case Log::Level::Error: return L"error";
            default: return L"info";
        }
    }

    // 通知：与真实状态比对后去重，避免定时器把同一份快照反复推给前端。
    void NotifyWorkspace(UiContext& context)
    {
        if (!UsingWebView())
        {
            return;
        }
        const std::string json = WorkspaceJson(context);
        const std::wstring wide = ToWide(json);
        if (wide == S().lastWorkspace)
        {
            return;
        }
        S().lastWorkspace = wide;
        Emit(json);
    }

    void NotifyLibrary(UiContext& context)
    {
        if (!UsingWebView())
        {
            return;
        }
        const std::string json = LibraryJson(context);
        const std::wstring wide = ToWide(json);
        if (wide == S().lastLibrary)
        {
            return;
        }
        S().lastLibrary = wide;
        Emit(json);
    }

    void NotifyUi(UiContext& context, const wchar_t* reason)
    {
        if (!UsingWebView())
        {
            return;
        }
        std::vector<std::pair<std::string, std::string>> patch;
        const std::wstring what = reason ? reason : L"";
        if (what.empty() || what == L"tab")
        {
            patch.emplace_back("tab", JsonString(ToWide(TabIdText(context.ActiveTab))));
        }
        if (what.empty() || what == L"logOpen")
        {
            patch.emplace_back("logOpen", JsonBool(context.LogVisible));
        }
        if (what.empty() || what == L"mode")
        {
            patch.emplace_back("mode", JsonString(ModeText(context.Mode)));
        }
        if (what.empty() || what == L"selection")
        {
            patch.emplace_back("selectedPublisher",
                               context.SelectedPublisher.empty() ? "null" : JsonString(context.SelectedPublisher));
            patch.emplace_back("selectedTitle",
                               context.SelectedTitle.empty() ? "null" : JsonString(context.SelectedTitle));
        }
        if (patch.empty())
        {
            return;
        }
        Emit(UiSyncJson(patch));
    }

    std::string JobJson(const wchar_t* kind, const wchar_t* status, double progress, const std::wstring& text,
                        const std::wstring& error)
    {
        std::vector<std::pair<std::string, std::string>> members{
            { "type", JsonString(L"job") },
            { "kind", JsonString(kind) },
            { "status", JsonString(status) },
            { "progress", JsonDouble(progress) },
            { "text", JsonString(text) },
        };
        if (!error.empty())
        {
            members.emplace_back("error", JsonString(error));
        }
        return JsonObject(members);
    }

    void NotifyJob(UiContext& context)
    {
        if (!UsingWebView())
        {
            return;
        }
        if (context.JobActive)
        {
            unsigned int percent = 0u;
            std::wstring stage;
            if (context.Job.LockReady)
            {
                ::EnterCriticalSection(&context.Job.Lock);
            }
            percent = static_cast<unsigned int>(context.Job.Percent);
            stage = context.Job.Stage;
            if (context.Job.LockReady)
            {
                ::LeaveCriticalSection(&context.Job.Lock);
            }
            Emit(JobJson(JobKindText(context.Job.Kind), L"running", percent / 100.0, stage, std::wstring()));
            return;
        }
        if (context.Job.Thread != nullptr)
        {
            return; // 还在收尾，等 OnJobDone 的最终快照。
        }
        if (context.Job.Kind == JobKind::None)
        {
            Emit(JobJson(L"idle", L"idle", 0.0, L"（空闲）", std::wstring()));
            return;
        }
        const wchar_t* status = context.Job.Ok ? L"success" : (context.Job.Error.empty() ? L"idle" : L"error");
        Emit(JobJson(JobKindText(context.Job.Kind), status,
                     static_cast<double>(context.Job.Percent) / 100.0, context.Job.Stage, context.Job.Error));
    }

    void NotifyRestore(UiContext& context, unsigned int state, unsigned int percent)
    {
        if (!UsingWebView())
        {
            return;
        }
        const wchar_t* status = L"running";
        std::wstring text = L"还原窗口正在跑任务";
        if (state == Restore::kStateCompleted)
        {
            status = L"success";
            text = L"还原完成，详见还原报告";
        }
        else if (state == Restore::kStateFailed)
        {
            status = L"error";
            text = L"还原失败，详见还原窗口日志";
        }
        else if (state == Restore::kStateQueued)
        {
            status = L"running";
            text = L"还原任务排队中";
        }
        Emit(JobJson(L"restore", status, static_cast<double>(percent) / 100.0, text, std::wstring()));
        // 还原窗口是独立的 Win32 窗口；主界面这边的任务列表为空是事实，
        // 不把编造的分包进度推给前端。整体统计只认 RestoreReport.txt：
        // 任务结束（成功或失败）且报告解析得出来才填真实数字；进行中不编进度，仍是 0 / null。
        const RestoreStats restore =
            (state == Restore::kStateCompleted || state == Restore::kStateFailed)
                ? LoadRestoreStats(context.Paths.RestoreReport, false)
                : RestoreStats();
        Emit(JsonObject({
            { "type", JsonString(L"restoreTasks") },
            { "tasks", "[]" },
            { "stats",
              JsonObject({
                  { "total", JsonNumber(0) },
                  { "running", JsonNumber(0) },
                  { "queued", JsonNumber(0) },
                  { "done", JsonNumber(0) },
                  { "failed", JsonNumber(0) },
                  { "restored",
                    restore.present ? JsonNumber(static_cast<long long>(restore.restored)) : JsonNumber(0) },
                  { "hashed",
                    restore.present ? JsonNumber(static_cast<long long>(restore.hashed)) : JsonNumber(0) },
                  { "kept", JsonNumber(0) },
                  { "discarded", JsonNumber(0) },
                  { "suffixFixed", JsonNumber(0) },
                  { "mined", JsonNumber(0) },
                  { "pass1Rate", "null" },
                  { "finalRate",
                    restore.present ? JsonDouble(restore.ratePercent / 100.0) : std::string("null") },
              }) },
            { "mining",
              status == std::wstring(L"running") ? JsonString(text) : std::string("null") },
        }));
    }

    void NotifyToast(const wchar_t* level, const wchar_t* title, const wchar_t* body)
    {
        if (!UsingWebView())
        {
            return;
        }
        Emit(JsonObject({
            { "type", JsonString(L"toast") },
            { "level", JsonString(level ? level : L"info") },
            { "title", JsonString(title ? title : L"") },
            { "body", JsonString(body ? body : L"") },
        }));
    }

    void NotifyLog(Log::Level level, const std::wstring& text)
    {
        if (!UsingWebView())
        {
            return;
        }
        std::wstring source = L"YuaExtract";
        std::wstring message = text;
        if (message.size() > 2u && message[0] == L'[')
        {
            const size_t close = message.find(L']');
            if (close != std::wstring::npos && close > 1u && close < 32u)
            {
                source = message.substr(1, close - 1);
                message = message.substr(close + 1u);
                while (!message.empty() && (message.front() == L' ' || message.front() == L':'))
                {
                    message.erase(message.begin());
                }
            }
        }
        Emit(JsonObject({
            { "type", JsonString(L"log") },
            { "entry",
              JsonObject({
                  { "ts", JsonNumber(NowMilliseconds()) },
                  { "level", JsonString(LogLevelText(level)) },
                  { "source", JsonString(source) },
                  { "message", JsonString(message) },
              }) },
        }));
    }

    void NotifyPicked(const wchar_t* kind, const std::wstring& path)
    {
        if (!UsingWebView())
        {
            return;
        }
        Emit(JsonObject({
            { "type", JsonString(L"picked") },
            { "kind", JsonString(kind ? kind : L"exe") },
            { "path", JsonString(path) },
        }));
    }

    void NotifySelfCheck(UiContext& context)
    {
        if (!UsingWebView() || !context.HasSelfCheck)
        {
            return;
        }
        std::vector<std::string> items;
        int index = 0;
        for (const auto& item : context.SelfCheck)
        {
            // 内核的 Severity：0 = 通过，其余 = 有问题。
            items.push_back(JsonObject({
                { "id", JsonString(Host::Format(L"%d", index++)) },
                { "title", JsonString(item.Label) },
                { "ok", JsonBool(item.Severity == 0) },
                { "detail", JsonString(item.Detail) },
            }));
        }
        Emit(JsonObject({
            { "type", JsonString(L"selfCheck") },
            { "items", JsonArray(items) },
        }));
    }

    void NotifyNotes(UiContext& context)
    {
        if (!UsingWebView())
        {
            return;
        }
        // text 必须是 OnSetShowNote 拼好的完整中文留言（壳的 store.tsx 只读 ev.text）；
        // contributor / version / date / comment 是档案 META 的原始字段，给前端自行展示。
        Emit(JsonObject({
            { "type", JsonString(L"notes") },
            { "text", JsonString(context.NoteText) },
            { "contributor", JsonString(context.NoteContributor) },
            { "version", JsonString(context.NoteVersion) },
            { "date", JsonString(context.NoteDate) },
            { "comment", JsonString(context.NoteComment) },
        }));
    }

    void NotifyOverlay(const wchar_t* overlay, bool open)
    {
        if (!UsingWebView())
        {
            return;
        }
        Emit(JsonObject({
            { "type", JsonString(L"overlayHint") },
            { "overlay", JsonString(overlay ? overlay : L"settings") },
            { "open", JsonBool(open) },
        }));
    }

    // ---------- 请求类型白名单 ----------

    bool IsKnownRequest(const std::wstring& type)
    {
        static const wchar_t* kTypes[] = {
            L"ready",           L"browseExe",      L"setTarget",      L"setMode",
            L"selectExtension", L"runStep",        L"checkStatus",    L"openWorkspace",
            L"openRestoreDir",  L"openRestoreReport", L"cleanDirs",    L"cancelJob",
            L"generateSet",     L"saveMeta",       L"dryRunSet",      L"installSet",
            L"openDraftDir",    L"openLibraryRoot", L"openSelectedSet", L"pickSourceDir",
            L"pickTestDir",     L"refreshLibrary", L"selfCheck",      L"readNotes",
            L"fetchIndex",      L"downloadRemote", L"startRestore",   L"stopRestore",
            L"startCollectDemo", L"stopGame",      L"clearCollectFiles", L"openCollectDir",
            L"enqueueXp3",      L"startBatch",     L"stopBatch",      L"openLegacy",
            L"failDemo",
        };
        for (const wchar_t* known : kTypes)
        {
            if (type == known)
            {
                return true;
            }
        }
        return false;
    }

    // ---------- 在线更新（后台线程） ----------

    struct OnlineWork
    {
        enum class Kind
        {
            Index,
            Download
        };

        Kind kind = Kind::Index;
        HWND notify = nullptr;
        std::wstring base;
        std::wstring endpoint;
        bool force = false;
        ExtensionSet::LibraryEntry entry;
        ExtensionSet::LibraryCatalog catalog;
        bool ok = false;
        std::wstring error;
        std::wstring detail;
        std::vector<ExtensionSet::LibraryEntry> entries;
        unsigned int httpStatus = 0;
    };

    std::wstring RemoteStatusText(Online::EntryState state)
    {
        switch (state)
        {
            case Online::EntryState::New: return L"new";
            case Online::EntryState::Update: return L"update";
            case Online::EntryState::LocalNewer: return L"local-richer";
            default: return L"same";
        }
    }

    DWORD WINAPI OnlineThreadProc(LPVOID parameter)
    {
        OnlineWork* work = static_cast<OnlineWork*>(parameter);
        if (work->kind == OnlineWork::Kind::Index)
        {
            Online::IndexResult info;
            std::vector<ExtensionSet::LibraryEntry> entries;
            std::wstring error;
            work->ok = Online::FetchIndex(work->base, entries, info, error);
            work->error = error;
            work->httpStatus = static_cast<unsigned int>(info.httpStatus);
            work->entries = std::move(entries);
        }
        else
        {
            Online::DownloadResult download;
            std::wstring error;
            if (!Online::DownloadArchive(work->base, work->entry, std::wstring(), download, error))
            {
                work->ok = false;
                work->error = error;
            }
            else
            {
                std::wstring detail;
                if (!Online::VerifyArchiveBytes(download.blob, work->entry, detail, error))
                {
                    work->ok = false;
                    work->error = error;
                }
                else
                {
                    std::wstring installError;
                    const std::wstring source =
                        Online::ArchiveUrl(work->base, work->entry.publisher, work->entry.title);
                    // 会社 / 作品由档案自己的 META 决定（已在上一步与索引交叉校验过），
                    // 所以这里只给库根、字节与来源标签。
                    work->ok = ExtensionSet::InstallBytesToLibrary(
                        ExtensionSet::DefaultLibraryRoot(Host::LoaderDirectory()), download.blob, installError,
                        source);
                    if (!work->ok)
                    {
                        work->error = installError;
                    }
                    work->detail = detail;
                }
                Online::CleanupStaging(download.stagingDirectory);
            }
        }
        if (work->notify != nullptr)
        {
            ::PostMessageW(work->notify, kWorkMessage, 0, reinterpret_cast<LPARAM>(work));
        }
        return 0;
    }

    void StartOnlineWork(OnlineWork* work)
    {
        HANDLE thread = ::CreateThread(nullptr, 0u, OnlineThreadProc, work, 0u, nullptr);
        if (thread == nullptr)
        {
            delete work;
            NotifyToast(L"error", L"在线更新", L"启动网络线程失败。");
            return;
        }
        ::CloseHandle(thread); // 线程自己跑完就退，句柄不需要 join（结果靠消息回来）。
    }

    void HandleOnlineWork(UiContext& context, OnlineWork* work)
    {
        const bool index = work->kind == OnlineWork::Kind::Index;
        Emit(JobJson(index ? L"online-index" : L"online-download", work->ok ? L"success" : L"error",
                     work->ok ? 1.0 : 0.0,
                     index ? std::wstring(L"索引拉取完成") : std::wstring(L"下载完成"),
                     work->ok ? std::wstring() : work->error));
        if (!work->ok)
        {
            Emit(JsonObject({
                { "type", JsonString(L"remoteIndex") },
                { "items", "[]" },
                { "base", JsonString(work->base) },
                { "error", JsonString(work->error) },
            }));
            NotifyToast(L"error", index ? L"索引拉取失败" : L"下载失败", work->error.c_str());
            delete work;
            return;
        }

        if (index)
        {
            std::vector<std::string> items;
            for (const auto& entry : work->entries)
            {
                const Online::EntryState state = Online::Classify(entry, work->catalog);
                items.push_back(JsonObject({
                    { "id", JsonString(entry.publisher + L"/" + entry.title) },
                    { "publisher", JsonString(entry.publisher) },
                    { "title", JsonString(entry.title) },
                    { "contributor", JsonString(L"") },
                    { "version", JsonString(L"") },
                    { "date", JsonString(L"") },
                    { "comment", JsonString(L"") },
                    { "dirCount", JsonNumber(static_cast<long long>(entry.directoryCount)) },
                    { "fileCount", JsonNumber(static_cast<long long>(entry.fileCount)) },
                    { "exceptions", JsonNumber(static_cast<long long>(entry.exceptionCount)) },
                    { "archiveSha", JsonString(entry.archiveDigest) },
                    { "targetSha", JsonString(entry.exeDigest) },
                    { "siblingSha", JsonString(entry.siblingDigest) },
                    { "saltSha", JsonString(entry.saltDigest) },
                    { "generatedAt", JsonString(entry.generatedAt) },
                    { "installedAt", JsonString(entry.installedAt) },
                    { "source", JsonString(entry.source) },
                    { "path", JsonString(entry.archivePath) },
                    { "remoteDirCount", JsonNumber(static_cast<long long>(entry.directoryCount)) },
                    { "remoteFileCount", JsonNumber(static_cast<long long>(entry.fileCount)) },
                    { "remoteSha", JsonString(entry.archiveDigest) },
                    { "status", JsonString(RemoteStatusText(state)) },
                }));
            }
            Emit(JsonObject({
                { "type", JsonString(L"remoteIndex") },
                { "items", JsonArray(items) },
                { "base", JsonString(work->base) },
            }));
            NotifyToast(L"info", L"索引已拉取",
                        Host::Format(L"共 %u 条远端扩展集。", static_cast<unsigned int>(items.size())).c_str());
            // 缓存落地（仅 UI 线程）：downloadRemote 只认这份远端索引；
            // 索引失败的提前 return 不经过这里，上一次成功的缓存得以保留。
            S().remoteEntries = work->entries;
            S().remoteCatalogSnapshot = work->catalog;
        }
        else
        {
            Host::LogLine(Log::Level::Info, L"[YuaExtract] 在线更新：已安装 " + work->entry.publisher + L" / " +
                                               work->entry.title);
            NotifyToast(L"info", L"已安装到扩展集库", work->detail.c_str());
            RefreshLibrary(context);
        }
        delete work;
    }

    // ---------- 请求分发（UI 线程） ----------

    // 批量解包的任务列表来自「刚刚交给解包窗口的那批文件名」。进度归解包窗口所有，
    // 主界面只知道"已排队"，所以 status/progress 如实报 queued/0，不编造百分比。
    std::string BatchSnapshotJson(const std::vector<std::wstring>& names)
    {
        std::vector<std::string> tasks;
        for (size_t index = 0; index < names.size(); ++index)
        {
            tasks.push_back(JsonObject({
                { "id", JsonString(Host::Format(L"xp3-%u", static_cast<unsigned int>(index + 1))) },
                { "file", JsonString(names[index]) },
                { "status", JsonString(L"queued") },
                { "progress", JsonNumber(0) },
                { "detail", JsonString(L"已交给解包窗口排队") },
                { "size", JsonString(L"") },
            }));
        }
        return JsonObject({
            { "type", JsonString(L"batch") },
            { "tasks", JsonArray(tasks) },
            { "stats",
              JsonObject({
                  { "total", JsonNumber(static_cast<long long>(names.size())) },
                  { "batch", JsonNumber(0) },
                  { "batchTotal", JsonNumber(static_cast<long long>(names.size())) },
                  { "running", JsonNumber(0) },
                  { "done", JsonNumber(0) },
                  { "failed", JsonNumber(0) },
              }) },
        });
    }

    // 采集控制台运行在注入模块里；主界面只报"是否已启动 + 空进度"，
    // 不复制一份假的计数。
    std::string CollectSnapshotJson(bool running)
    {
        return JsonObject({
            { "type", JsonString(L"collect") },
            { "running", JsonBool(running) },
            { "uptime", JsonNumber(0) },
            { "pid", JsonNumber(0) },
            { "dirHit", JsonNumber(0) },
            { "dirTotal", JsonNumber(0) },
            { "fileHit", JsonNumber(0) },
            { "fileTotal", JsonNumber(0) },
            { "lines", JsonObject({ { "dir", "[]" }, { "file", "[]" } }) },
        });
    }

    void RunStep(UiContext& context, int step)
    {
        switch (step)
        {
            case 1:
                if (context.Mode == ActiveMode::SetExtract)
                {
                    OnMode1Step1(context);
                }
                else if (context.Mode == ActiveMode::SetCrack)
                {
                    NotifyToast(L"warn", L"二期预留", L"会社合集是二期预留功能。");
                }
                else
                {
                    OnStep1(context);
                }
                break;
            case 2:
                if (context.Mode == ActiveMode::SetExtract)
                {
                    OnStep1(context);
                }
                else if (context.Mode == ActiveMode::SetCrack)
                {
                    NotifyToast(L"warn", L"二期预留", L"会社合集是二期预留功能。");
                }
                else
                {
                    OnStep2(context);
                }
                break;
            case 3:
                if (context.Mode == ActiveMode::SetExtract)
                {
                    OnStep3(context, context.StaticHashDir, true);
                }
                else if (context.Mode == ActiveMode::SetCrack)
                {
                    NotifyToast(L"warn", L"二期预留", L"会社合集是二期预留功能。");
                }
                else
                {
                    OnStep3(context, context.Paths.HashDir, false);
                }
                break;
            default:
                NotifyToast(L"warn", L"步骤无效", L"runStep 只接受 1 / 2 / 3。");
                break;
        }
    }

    void HandleRequest(UiContext& context, const JsonValue& document)
    {
        const std::wstring type = document.MemberText("type");

        if (type == L"ready")
        {
            Emit(HelloJson(context));
            NotifyWorkspace(context);
            NotifyLibrary(context);
            NotifyUi(context, nullptr);
            Host::LogLine(Log::Level::Info, L"[YuaExtract] WebView2 壳已就绪");
            return;
        }
        if (type == L"failDemo")
        {
            // 生产构建里没有任何演示钩子：明确拒绝，不静默。
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 拒绝 failDemo：生产构建不提供演示失败注入");
            NotifyToast(L"warn", L"不可用", L"演示失败注入只在 demo/test 构建里存在。");
            return;
        }
        if (type == L"browseExe")
        {
            OnBrowseExe(context);
            NotifyPicked(L"exe", context.ExePath);
            NotifyWorkspace(context);
            return;
        }
        if (type == L"setTarget")
        {
            const std::wstring path = document.MemberText("path");
            if (path.empty())
            {
                NotifyToast(L"error", L"目标无效", L"setTarget 需要非空 path。");
                return;
            }
            if (!Host::ValidateGameExe(context.Window, path))
            {
                NotifyToast(L"error", L"目标无效", L"不是可用的 32 位游戏主程序。");
                return;
            }
            ApplyTarget(context, path);
            Host::LogLine(Log::Level::Info, L"[YuaExtract] 目标游戏：" + path);
            return;
        }
        if (type == L"setMode")
        {
            const std::wstring mode = document.MemberText("mode");
            if (mode == L"extension")
            {
                ApplyMode(context, ActiveMode::SetExtract);
            }
            else if (mode == L"studio")
            {
                ApplyMode(context, ActiveMode::SetCrack);
            }
            else if (mode == L"classic")
            {
                ApplyMode(context, ActiveMode::Classic);
            }
            else
            {
                NotifyToast(L"error", L"模式无效", L"setMode 只接受 extension / studio / classic。");
            }
            return;
        }
        if (type == L"selectExtension")
        {
            const std::wstring publisher = document.MemberText("publisher");
            const std::wstring title = document.MemberText("title");
            context.SelectedPublisher = publisher;
            context.SelectedTitle = title;
            RefreshSetPaths(context);
            RefreshLibrary(context);
            context.RenderedOverview.clear();
            UpdateOverview(context);
            NotifyUi(context, L"selection");
            return;
        }
        if (type == L"runStep")
        {
            RunStep(context, static_cast<int>(document.MemberNumber("step", 0)));
            NotifyWorkspace(context);
            NotifyJob(context);
            return;
        }
        if (type == L"checkStatus")
        {
            OnAuxCheck(context);
            NotifyWorkspace(context);
            return;
        }
        if (type == L"openWorkspace") { OnAuxOpenWorkspace(context); return; }
        if (type == L"openRestoreDir") { OnAuxOpenRestoreDir(context); return; }
        if (type == L"openRestoreReport") { OnAuxOpenReport(context); return; }
        if (type == L"cleanDirs")
        {
            OnAuxClean(context);
            NotifyWorkspace(context);
            return;
        }
        if (type == L"cancelJob")
        {
            if (context.JobActive)
            {
                ::InterlockedExchange(&context.Job.Cancel, 1);
                SetTaskText(context, L"正在取消……（内核按条检查取消标志）");
                Host::LogLine(Log::Level::Warn, L"[YuaExtract] 已请求取消后台任务");
                Emit(JobJson(JobKindText(context.Job.Kind), L"cancelling",
                             static_cast<double>(context.Job.Percent) / 100.0, context.Job.Stage,
                             std::wstring()));
            }
            return;
        }
        if (type == L"generateSet") { OnSetGenerate(context); NotifyJob(context); return; }
        if (type == L"dryRunSet") { OnSetTest(context); NotifyJob(context); return; }
        if (type == L"installSet")
        {
            OnSetInstall(context);
            NotifyLibrary(context);
            return;
        }
        if (type == L"saveMeta")
        {
            const JsonValue* meta = document.Member("meta");
            if (meta == nullptr || meta->kind != JsonValue::Kind::Object)
            {
                NotifyToast(L"error", L"信息无效", L"saveMeta 需要 meta 对象。");
                return;
            }
            context.Meta.publisher = meta->MemberText("publisher");
            context.Meta.title = meta->MemberText("title");
            context.Meta.contributor = meta->MemberText("contributor");
            context.Meta.version = meta->MemberText("version");
            context.Meta.date = meta->MemberText("date");
            context.Meta.comment = meta->MemberText("comment");
            context.SetPageNote = L"已更新扩展集信息（生成时才会写进档案）。";
            Host::LogLine(Log::Level::Info, L"[YuaExtract] 扩展集信息已更新");
            NotifyToast(L"info", L"扩展集信息已更新", L"重新执行「生成」后新信息才会写进档案。");
            NotifyLibrary(context);
            return;
        }
        if (type == L"openDraftDir") { OnSetOpenDraft(context); return; }
        if (type == L"openLibraryRoot") { OnSetOpenLibraryRoot(context); return; }
        if (type == L"openSelectedSet") { OnSetOpenEntry(context); return; }
        if (type == L"pickSourceDir")
        {
            OnSetPickSource(context);
            NotifyPicked(L"source", context.SourceOverride);
            return;
        }
        if (type == L"pickTestDir")
        {
            OnSetPickTest(context);
            NotifyPicked(L"test", context.TestSourceOverride);
            return;
        }
        if (type == L"refreshLibrary")
        {
            OnSetRefresh(context);
            NotifyLibrary(context);
            return;
        }
        if (type == L"selfCheck")
        {
            OnSetSelfCheck(context);
            NotifySelfCheck(context);
            return;
        }
        if (type == L"readNotes")
        {
            // 只有真的读出档案才发 notes / 开留言层；失败时 OnSetShowNote 已把
            // Note* 字段清空并弹了提示，这里再把空留言推出去会把层挂成空白。
            if (OnSetShowNote(context))
            {
                NotifyNotes(context);
                NotifyOverlay(L"notes", true);
            }
            return;
        }
        if (type == L"fetchIndex")
        {
            OnlineWork* work = new OnlineWork();
            work->kind = OnlineWork::Kind::Index;
            work->notify = context.Window;
            work->catalog = context.Catalog;
            const std::wstring endpoint = document.MemberText("endpoint");
            work->endpoint = endpoint;
            if (!endpoint.empty())
            {
                std::wstring error;
                if (!Online::SetRepositoryBase(Online::NormalizeBase(endpoint), error))
                {
                    delete work;
                    NotifyToast(L"error", L"仓库地址无效", error.c_str());
                    return;
                }
            }
            work->base = Online::ResolveRepositoryBase();
            Emit(JobJson(L"online-index", L"running", 0.05, L"正在拉取远端索引……", std::wstring()));
            StartOnlineWork(work);
            return;
        }
        if (type == L"downloadRemote")
        {
            const std::wstring id = document.MemberText("entryId");
            const size_t slash = id.find(L'/');
            if (slash == std::wstring::npos)
            {
                NotifyToast(L"error", L"条目无效", L"downloadRemote 需要 publisher/title 形式的 entryId。");
                return;
            }
            const std::wstring publisher = id.substr(0, slash);
            const std::wstring title = id.substr(slash + 1);
            // 只认最近一次成功拉取的**远端索引**（S().remoteEntries，UI 线程内读写）。
            // 本地库清单不是查找来源：远端新增的条目本地根本没有。
            const State& state = S();
            ExtensionSet::LibraryEntry entry;
            bool found = false;
            for (const auto& candidate : state.remoteEntries)
            {
                if (candidate.publisher == publisher && candidate.title == title)
                {
                    entry = candidate;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                NotifyToast(L"error", L"远端条目不存在",
                            L"请先点「拉取索引」获取远端索引；若已拉取，说明索引里没有这一条"
                            L"（会社/作品需与索引完全一致）。");
                return;
            }
            // 与索引列表同一口径的状态判定：本地更全时默认不覆盖，除非显式 force。
            const Online::EntryState remoteState = Online::Classify(entry, state.remoteCatalogSnapshot);
            if (remoteState == Online::EntryState::LocalNewer && !document.MemberBool("force", false))
            {
                NotifyToast(L"warn", L"本地更全，默认不覆盖", Online::DescribeState(remoteState));
                return;
            }
            OnlineWork* work = new OnlineWork();
            work->kind = OnlineWork::Kind::Download;
            work->notify = context.Window;
            work->entry = entry;
            work->force = document.MemberBool("force", false);
            work->base = Online::ResolveRepositoryBase();
            Emit(JobJson(L"online-download", L"running", 0.05, L"正在下载并校验……", std::wstring()));
            StartOnlineWork(work);
            return;
        }
        if (type == L"startRestore")
        {
            if (context.Mode == ActiveMode::SetExtract)
            {
                OnStep3(context, context.StaticHashDir, true);
            }
            else
            {
                OnStep3(context, context.Paths.HashDir, false);
            }
            NotifyWorkspace(context);
            return;
        }
        if (type == L"stopRestore")
        {
            if (context.RestoreWindow != nullptr && ::IsWindow(context.RestoreWindow))
            {
                Restore::CloseRestoreWindow(context.RestoreWindow);
                if (!::IsWindow(context.RestoreWindow))
                {
                    context.RestoreWindow = nullptr;
                }
                NotifyToast(L"warn", L"已请求关闭还原窗口", L"还原窗口会等当前分包处理完再退出。");
            }
            return;
        }
        if (type == L"startCollectDemo")
        {
            // 真正的采集控制台是注入模块自己的窗口；这里走与原版「动态 Hash 收集」同一条路径。
            OnStep2(context);
            NotifyOverlay(L"collect", true);
            Emit(CollectSnapshotJson(context.Stage >= 2));
            NotifyToast(L"info", L"已启动 Hash 收集",
                        L"游戏与收集面板由注入模块提供，本页显示的是宿主状态。");
            return;
        }
        if (type == L"openCollectDir")
        {
            if (context.Paths.HashDir.empty() || ::GetFileAttributesW(context.Paths.HashDir.c_str()) ==
                                                     INVALID_FILE_ATTRIBUTES)
            {
                NotifyToast(L"warn", L"目录不存在", L"还没有生成 Hash 输出目录。");
                return;
            }
            Host::OpenInExplorer(context.Window, context.Paths.HashDir);
            return;
        }
        if (type == L"stopGame" || type == L"clearCollectFiles")
        {
            // 主界面不持有游戏进程句柄：进程由注入流程启动并随游戏自身退出。
            // 不做假的“已停止”，如实告诉用户去哪里操作。
            Host::LogLine(Log::Level::Warn,
                          L"[YuaExtract] stopGame / clearCollectFiles：主界面没有游戏进程句柄，请在那个窗口里操作");
            Emit(CollectSnapshotJson(false));
            NotifyToast(L"warn", L"请在收集窗口里操作",
                        L"主界面不持有游戏进程句柄，关闭游戏 / 清空文件请在注入的收集窗口里执行。");
            return;
        }
        if (type == L"enqueueXp3" || type == L"startBatch")
        {
            // 批量解包窗口活在游戏进程里（CxdecExtractorUI.dll）。这里做的是“启动并注入”，
            // 与原版「XP3 动态解包」完全同一条路径；排队在那边进行。
            OnStep1(context);
            NotifyOverlay(L"batch", true);
            std::vector<std::wstring> queued;
            const JsonValue* names = document.Member("names");
            if (names != nullptr && names->kind == JsonValue::Kind::Array)
            {
                for (const auto& item : names->items)
                {
                    if (item.kind == JsonValue::Kind::String)
                    {
                        queued.push_back(ToWide(item.text));
                    }
                }
            }
            if (!queued.empty())
            {
                std::wstring listed;
                for (const std::wstring& name : queued)
                {
                    listed += (listed.empty() ? L"" : L", ") + name;
                }
                Host::LogLine(Log::Level::Info, L"[YuaExtract] 待排队 XP3：" + listed);
                Emit(BatchSnapshotJson(queued));
            }
            Emit(JobJson(L"extract", L"running", 0.05, L"已启动解包界面，等待排队", std::wstring()));
            return;
        }
        if (type == L"stopBatch")
        {
            NotifyToast(L"warn", L"请在解包窗口里停止", L"批量解包运行在游戏进程内，主界面无法直接停止它。");
            return;
        }
        if (type == L"openLegacy")
        {
            const std::wstring action = document.MemberText("action");
            if (action.empty())
            {
                Host::ShowLegacyFunctionsWindow(context.Window);
                return;
            }
            std::wstring error;
            if (!Host::RunLegacyAction(context.Window, action, error))
            {
                NotifyToast(L"error", L"原版功能不可用", error.c_str());
                Host::LogLine(Log::Level::Error, L"[YuaExtract] 原版功能失败：" + error);
                return;
            }
            Host::LogLine(Log::Level::Info, L"[YuaExtract] 已打开原版功能：" + action);
            return;
        }

        NotifyToast(L"warn", L"未处理的请求", L"宿主没有实现这个请求类型。");
    }

    // ---------- 安全校验 ----------

    bool IsTrustedOrigin(const std::wstring& uri)
    {
        const size_t length = ::wcslen(kOriginPrefix);
        if (uri.size() < length)
        {
            return false;
        }
        return ::_wcsnicmp(uri.c_str(), kOriginPrefix, length) == 0;
    }

    std::wstring TakeCoString(LPWSTR text)
    {
        if (text == nullptr)
        {
            return std::wstring();
        }
        std::wstring value(text);
        ::CoTaskMemFree(text);
        return value;
    }

    void RejectMessage(const wchar_t* reason)
    {
        Host::LogLine(Log::Level::Warn, std::wstring(L"[YuaExtract] 拒绝 WebView2 消息：") + reason);
    }

    // ---------- COM 回调基类 ----------

    struct RefCounted
    {
        volatile LONG Count = 1;
        ULONG Add() { return static_cast<ULONG>(::InterlockedIncrement(&Count)); }
    };

    template <typename TInterface, typename TSelf>
    class HandlerBase : public TInterface
    {
    public:
        ULONG STDMETHODCALLTYPE AddRef() override { return Refs.Add(); }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const LONG now = ::InterlockedDecrement(&Refs.Count);
            if (now == 0)
            {
                delete static_cast<TSelf*>(this);
            }
            return static_cast<ULONG>(now);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
        {
            if (object == nullptr)
            {
                return E_POINTER;
            }
            if (riid == __uuidof(TInterface) || riid == IID_IUnknown)
            {
                *object = static_cast<TInterface*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

    private:
        RefCounted Refs;
    };

    // ---------- 窗口与初始化 ----------

    LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
            case WM_ERASEBKGND:
            {
                RECT client{};
                ::GetClientRect(hwnd, &client);
                HBRUSH brush = ::CreateSolidBrush(RGB(0xEE, 0xE3, 0xEB));
                ::FillRect(reinterpret_cast<HDC>(wParam), &client, brush);
                ::DeleteObject(brush);
                return 1;
            }
            case WM_SETFOCUS:
                if (S().controller != nullptr)
                {
                    S().controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                }
                return 0;
            case WM_SIZE:
                if (S().controller != nullptr)
                {
                    RECT client{};
                    ::GetClientRect(hwnd, &client);
                    S().controller->put_Bounds(client);
                }
                return 0;
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool EnsureHostClass()
    {
        static bool registered = false;
        if (registered)
        {
            return true;
        }
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = HostWndProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kHostClass;
        if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
        registered = true;
        return true;
    }

    void ResizeHostWindow()
    {
        State& state = S();
        if (!state.hostCreated || state.host == nullptr || !::IsWindow(state.host))
        {
            return;
        }
        RECT client{};
        ::GetClientRect(state.main, &client);
        ::SetWindowPos(state.host, HWND_TOP, 0, 0, client.right, client.bottom,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (state.controller != nullptr)
        {
            state.controller->put_Bounds(client);
        }
    }

    // 外壳缺失 / WebView2 运行时不可用：说清楚缺什么、去哪儿补，然后退回纯 Win32 界面。
    void FailClosed(const wchar_t* title, const std::wstring& body)
    {
        Host::ShowError(S().main, std::wstring(title) + L"\r\n\r\n" + body);
        Host::LogLine(Log::Level::Error, std::wstring(L"[YuaExtract] WebView2 不可用：") + title);
    }

    // ---------- WebView2 回调 ----------

    class EnvironmentHandler final : public HandlerBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                                                       EnvironmentHandler>
    {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* environment) override;
    };

    class ControllerHandler final
        : public HandlerBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, ControllerHandler>
    {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override;
    };

    class NavigationHandler final
        : public HandlerBase<ICoreWebView2NavigationStartingEventHandler, NavigationHandler>
    {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) override
        {
            LPWSTR raw = nullptr;
            if (args == nullptr || FAILED(args->get_Uri(&raw)))
            {
                return S_OK;
            }
            const std::wstring uri = TakeCoString(raw);
            if (!IsTrustedOrigin(uri))
            {
                args->put_Cancel(TRUE);
                Host::LogLine(Log::Level::Warn, L"[YuaExtract] 已阻止导航：" + uri);
            }
            return S_OK;
        }
    };

    class NewWindowHandler final
        : public HandlerBase<ICoreWebView2NewWindowRequestedEventHandler, NewWindowHandler>
    {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) override
        {
            if (args == nullptr)
            {
                return S_OK;
            }
            LPWSTR raw = nullptr;
            if (SUCCEEDED(args->get_Uri(&raw)))
            {
                const std::wstring uri = TakeCoString(raw);
                if (!IsTrustedOrigin(uri))
                {
                    Host::LogLine(Log::Level::Warn, L"[YuaExtract] 已阻止新窗口：" + uri);
                }
            }
            // 无论 URL 是什么都不开新窗口：本界面没有多窗口语义。
            args->put_Handled(TRUE);
            return S_OK;
        }
    };

    class ResourceHandler final
        : public HandlerBase<ICoreWebView2WebResourceRequestedEventHandler, ResourceHandler>
    {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) override
        {
            if (args == nullptr || S().environment == nullptr)
            {
                return S_OK;
            }
            ICoreWebView2WebResourceRequest* request = nullptr;
            if (FAILED(args->get_Request(&request)) || request == nullptr)
            {
                return S_OK;
            }
            LPWSTR raw = nullptr;
            const bool hasUri = SUCCEEDED(request->get_Uri(&raw));
            const std::wstring uri = hasUri ? TakeCoString(raw) : std::wstring();
            request->Release();
            if (hasUri && IsTrustedOrigin(uri))
            {
                return S_OK;
            }
            // 非 yua.local 的请求一律 403：界面是自包含的，不该有任何外网资源。
            ICoreWebView2WebResourceResponse* response = nullptr;
            if (SUCCEEDED(S().environment->CreateWebResourceResponse(
                    nullptr, 403, L"Forbidden", L"Content-Type: text/plain", &response)) &&
                response != nullptr)
            {
                args->put_Response(response);
                response->Release();
            }
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 已阻止外部资源：" + uri);
            return S_OK;
        }
    };

    class MessageHandler final
        : public HandlerBase<ICoreWebView2WebMessageReceivedEventHandler, MessageHandler>
    {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) override
        {
            State& state = S();
            if (args == nullptr || state.context == nullptr)
            {
                return S_OK;
            }
            LPWSTR rawSource = nullptr;
            if (FAILED(args->get_Source(&rawSource)))
            {
                RejectMessage(L"无法读取消息来源");
                return S_OK;
            }
            const std::wstring source = TakeCoString(rawSource);
            if (!IsTrustedOrigin(source))
            {
                RejectMessage(L"来源不是 https://yua.local/");
                return S_OK;
            }

            LPWSTR rawJson = nullptr;
            if (FAILED(args->get_WebMessageAsJson(&rawJson)))
            {
                RejectMessage(L"无法读取消息体");
                return S_OK;
            }
            const std::wstring wide = TakeCoString(rawJson);
            const std::string json = ToUtf8(wide);
            if (json.size() > kMaxMessageBytes)
            {
                RejectMessage(L"消息超过 262144 字节");
                return S_OK;
            }

            JsonValue document;
            if (!ParseJson(json, document) || document.kind != JsonValue::Kind::Object)
            {
                RejectMessage(L"消息不是合法 JSON 对象");
                return S_OK;
            }
            if (document.MemberNumber("protocolVersion", 0.0) != 1.0)
            {
                RejectMessage(L"protocolVersion 不是 1");
                return S_OK;
            }
            const std::wstring id = document.MemberText("id");
            const std::wstring type = document.MemberText("type");
            if (id.empty() || id.size() > 64u)
            {
                RejectMessage(L"id 为空或超过 64 字符");
                return S_OK;
            }
            if (type.empty() || !IsKnownRequest(type))
            {
                RejectMessage(L"type 不在允许列表里");
                return S_OK;
            }

            // 到这一步已经是可信请求：分发到真实后端。异常不允许把 UI 线程带走。
            try
            {
                HandleRequest(*state.context, document);
            }
            catch (...)
            {
                NotifyToast(L"error", L"宿主异常", L"处理请求时发生未捕获异常，详见日志。");
                Host::LogLine(Log::Level::Error, L"[YuaExtract] 处理 WebView2 请求时发生异常：" + type);
            }
            return S_OK;
        }
    };

    HRESULT STDMETHODCALLTYPE EnvironmentHandler::Invoke(HRESULT result, ICoreWebView2Environment* environment)
    {
        if (FAILED(result) || environment == nullptr)
        {
            if (S().hostCreated && ::IsWindow(S().host))
            {
                ::DestroyWindow(S().host);
            }
            S().host = nullptr;
            S().hostCreated = false;
            FailClosed(L"无法初始化 WebView2 运行时。",
                       L"请确认已安装 Microsoft Edge WebView2 Runtime（x86 版本）。\r\n"
                       L"安装后重新启动 YuaExtract.exe；在此之前界面会保持原生的 Win32 版本。");
            return S_OK;
        }
        S().environment = environment;
        environment->AddRef();
        const HRESULT created = environment->CreateCoreWebView2Controller(S().host, new ControllerHandler());
        if (FAILED(created))
        {
            FailClosed(L"无法创建 WebView2 控制器。", L"详见日志。界面会保持原生的 Win32 版本。");
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ControllerHandler::Invoke(HRESULT result, ICoreWebView2Controller* controller)
    {
        State& state = S();
        if (FAILED(result) || controller == nullptr)
        {
            FailClosed(L"无法创建 WebView2 控制器。", L"详见日志。界面会保持原生的 Win32 版本。");
            return S_OK;
        }
        state.controller = controller;
        controller->AddRef();
        controller->put_IsVisible(TRUE);
        controller->put_ZoomFactor(1.0);

        ICoreWebView2* webview = nullptr;
        if (FAILED(controller->get_CoreWebView2(&webview)) || webview == nullptr)
        {
            FailClosed(L"无法取得 WebView2 视图。", L"详见日志。界面会保持原生的 Win32 版本。");
            return S_OK;
        }
        state.webview = webview;
        webview->AddRef();

        ICoreWebView2Settings* settings = nullptr;
        if (SUCCEEDED(webview->get_Settings(&settings)) && settings != nullptr)
        {
            settings->put_AreHostObjectsAllowed(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsScriptEnabled(TRUE);
            settings->Release();
        }

        EventRegistrationToken token{};
        ICoreWebView2NavigationStartingEventHandler* navigation = new NavigationHandler();
        webview->add_NavigationStarting(navigation, &token);
        navigation->Release();
        ICoreWebView2NewWindowRequestedEventHandler* newWindow = new NewWindowHandler();
        webview->add_NewWindowRequested(newWindow, &token);
        newWindow->Release();
        ICoreWebView2WebMessageReceivedEventHandler* messages = new MessageHandler();
        webview->add_WebMessageReceived(messages, &token);
        messages->Release();
        if (!state.resourceFilterAdded)
        {
            webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
            state.resourceFilterAdded = true;
        }
        ICoreWebView2WebResourceRequestedEventHandler* resources = new ResourceHandler();
        webview->add_WebResourceRequested(resources, &token);
        resources->Release();

        // 虚拟源：https://yua.local/ → <LoaderDir>/shell/
        ICoreWebView2_3* webview3 = nullptr;
        if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview3))) && webview3 != nullptr)
        {
            const std::wstring shellDirectory = JoinPath(Host::LoaderDirectory(), L"shell");
            webview3->SetVirtualHostNameToFolderMapping(
                kVirtualHost, shellDirectory.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            webview3->Release();
        }

        state.ready = true;
        ResizeHostWindow();
        // 控件已挪到屏幕外（-32000），但它们的 HWND / 可见性 / 文本一个都没动：
        // 重排一次让偏移生效，再把 WebView2 顶到最前。
        if (state.context != nullptr)
        {
            LayoutWindow(*state.context);
            UpdateOverview(*state.context);
        }
        ::SetWindowPos(state.host, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

        const std::wstring url = std::wstring(kOriginPrefix) + L"index.html";
        webview->Navigate(url.c_str());
        return S_OK;
    }

    // ---------- 公共入口 ----------

    void Attach(UiContext& context)
    {
        State& state = S();
        if (state.attached)
        {
            return;
        }
        if (state.forcedLegacy)
        {
            Host::LogLine(Log::Level::Info, L"[YuaExtract] 已按 --legacy-win32 / YUA_LEGACY_WIN32=1 使用纯 Win32 界面");
            return;
        }
        if (!state.lockReady)
        {
            ::InitializeCriticalSection(&state.lock);
            state.lockReady = true;
        }
        state.main = context.Window;
        state.context = &context;

        const std::wstring shellDirectory = JoinPath(Host::LoaderDirectory(), L"shell");
        const std::wstring indexPath = JoinPath(shellDirectory, L"index.html");
        if (::GetFileAttributesW(indexPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            // fail-closed：缺外壳就明确报错并留在 Win32 界面，绝不假装成功。
            FailClosed(L"缺少 shell 界面资源。",
                       L"找不到：\r\n" + indexPath +
                           L"\r\n\r\n请确认发布结构为：YuaExtract.exe + shell\\（index.html / assets / brand）。\r\n"
                           L"在此之前界面会保持原生的 Win32 版本。");
            return;
        }

        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com) && com != RPC_E_CHANGED_MODE)
        {
            FailClosed(L"无法初始化 COM（STA）。", L"界面会保持原生的 Win32 版本。");
            return;
        }
        state.comInitialized = SUCCEEDED(com);

        if (!EnsureHostClass())
        {
            FailClosed(L"无法注册 WebView2 宿主窗口类。", L"界面会保持原生的 Win32 版本。");
            return;
        }

        RECT client{};
        ::GetClientRect(context.Window, &client);
        state.host = ::CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                       0, 0, client.right, client.bottom, context.Window, nullptr,
                                       ::GetModuleHandleW(nullptr), nullptr);
        if (state.host == nullptr)
        {
            FailClosed(L"无法创建 WebView2 宿主窗口。", L"界面会保持原生的 Win32 版本。");
            return;
        }

        state.attached = true;
        state.hostCreated = true;
        Host::SetLogSink(&NotifyLog);

        const std::wstring userData = JoinPath(Host::LogDirectory(), L"WebView2Data");
        const HRESULT created = ::CreateCoreWebView2EnvironmentWithOptions(
            nullptr, userData.c_str(), nullptr, new EnvironmentHandler());
        if (FAILED(created))
        {
            if (::IsWindow(state.host))
            {
                ::DestroyWindow(state.host);
            }
            state.host = nullptr;
            state.hostCreated = false;
            Host::SetLogSink(nullptr);
            FailClosed(L"无法初始化 WebView2 运行时。",
                       L"请确认已安装 Microsoft Edge WebView2 Runtime（x86 版本）。\r\n"
                       L"在此之前界面会保持原生的 Win32 版本。");
        }
    }

    void Resize(UiContext& context)
    {
        (void)context;
        ResizeHostWindow();
    }

    void Detach()
    {
        State& state = S();
        Host::SetLogSink(nullptr);
        state.ready = false;
        if (state.webview != nullptr)
        {
            state.webview->Release();
            state.webview = nullptr;
        }
        if (state.controller != nullptr)
        {
            state.controller->Close();
            state.controller->Release();
            state.controller = nullptr;
        }
        if (state.environment != nullptr)
        {
            state.environment->Release();
            state.environment = nullptr;
        }
        if (state.host != nullptr && ::IsWindow(state.host))
        {
            ::DestroyWindow(state.host);
        }
        state.host = nullptr;
        state.hostCreated = false;
        state.attached = false;
        if (state.comInitialized)
        {
            ::CoUninitialize();
            state.comInitialized = false;
        }
        if (state.lockReady)
        {
            ::DeleteCriticalSection(&state.lock);
            state.lockReady = false;
        }
        state.context = nullptr;
        state.main = nullptr;
        state.lastWorkspace.clear();
        state.lastLibrary.clear();
    }

    void HandleWork(UiContext& context, LPARAM parameter)
    {
        OnlineWork* work = reinterpret_cast<OnlineWork*>(parameter);
        if (work == nullptr)
        {
            return;
        }
        HandleOnlineWork(context, work);
    }

    // 强制纯 Win32 模式：命令行 --legacy-win32 或环境变量 YUA_LEGACY_WIN32=1。
    void SetForcedLegacy(bool enabled)
    {
        S().forcedLegacy = enabled;
    }
}
