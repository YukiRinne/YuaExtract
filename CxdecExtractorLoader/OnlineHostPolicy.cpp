// 在线主机策略的实现。口径见 OnlineHostPolicy.h。
//
// 两条实现上的"不要"：
//   · HostIsBlocked 绝不发网络请求（连 DNS 都不做），它只认字符串与 IP 字面量；
//   · ResolveBlocked 只做一次 DNS 解析（GetAddrInfoW），把解析出的每个地址再交给
//     HostIsBlocked 判 —— 绝不在这里发 HTTP，也不做连通性探测。
//
// IPv4 / IPv6 的字面量认定统一走 InetPtonW：它比"自己数点几段"严格得多，能顺手挡掉
// "010.1.1.1" 这类八进制坑与残缺地址；认不出的名字只有 localhost 规则命中才在字符串
// 层拒绝，其余交给 ResolveBlocked 的 DNS 结果定夺。

#include "OnlineHostPolicy.h"

// InetPtonW / InetNtopW / GetAddrInfoW 的声明受 _WIN32_WINNT 门槛保护（Vista 起），
// 不定义的话 ws2tcpip.h 不给声明。按最低支持系统 Win7 定。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <cstring>

// 链接项留在源码里（与 OnlineUpdate.cpp 的 winhttp.lib 同一手法），不动 vcxproj。
#pragma comment(lib, "ws2_32.lib")

namespace Yua { namespace Online
{
    namespace
    {
        bool ContainsByte(const std::wstring& text, wchar_t ch)
        {
            return text.find(ch) != std::wstring::npos;
        }

        // ASCII 小写比较：主机名只关心 ASCII 字母的大小写，不去碰 locale。
        wchar_t ToLowerAscii(wchar_t ch)
        {
            return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
        }

        bool EqualsIgnoreCaseAscii(const std::wstring& text, const wchar_t* lower)
        {
            const std::wstring::size_type length = std::wcslen(lower);
            if (text.size() != length)
            {
                return false;
            }
            for (std::wstring::size_type i = 0; i < length; ++i)
            {
                if (ToLowerAscii(text[i]) != lower[i])
                {
                    return false;
                }
            }
            return true;
        }

        bool EndsWithIgnoreCaseAscii(const std::wstring& text, const wchar_t* lower)
        {
            const std::wstring::size_type length = std::wcslen(lower);
            if (text.size() < length)
            {
                return false;
            }
            const std::wstring::size_type offset = text.size() - length;
            for (std::wstring::size_type i = 0; i < length; ++i)
            {
                if (ToLowerAscii(text[offset + i]) != lower[i])
                {
                    return false;
                }
            }
            return true;
        }

        // URL 层给 IPv6 字面量套的方括号在这里剥掉："::1" 与 "[::1]" 是同一个地址。
        // 只有成对的方括号才剥；"[::1" 这种残缺的剥不动，交给 InetPtonW 判成非字面量。
        std::wstring StripBrackets(const std::wstring& host)
        {
            if (host.size() >= 2u && host.front() == L'[' && host.back() == L']')
            {
                return host.substr(1u, host.size() - 2u);
            }
            return host;
        }

        // ---- IPv4 保留段（任务书列的全集，含 /10 /12 /15 这种非整字节掩码）----
        // 传入网络序的 4 个字节。返回 nullptr = 不在保留段。
        const wchar_t* Ipv4BlockedReason(const unsigned char b[4])
        {
            if (b[0] == 0u)                                   return L"0.0.0.0/8（本网络）";               // 0.0.0.0/8
            if (b[0] == 10u)                                  return L"10.0.0.0/8（内网）";                // 10.0.0.0/8
            if (b[0] == 127u)                                 return L"127.0.0.0/8（本机回环）";           // 127.0.0.0/8
            if (b[0] == 100u && b[1] >= 64u && b[1] <= 127u)  return L"100.64.0.0/10（运营商级 NAT）";     // 100.64.0.0/10
            if (b[0] == 169u && b[1] == 254u)                 return L"169.254.0.0/16（链路本地）";        // 169.254.0.0/16
            if (b[0] == 172u && (b[1] & 0xF0u) == 0x10u)      return L"172.16.0.0/12（内网）";             // 172.16.0.0/12
            if (b[0] == 192u && b[1] == 0u && b[2] == 0u)     return L"192.0.0.0/24（IETF 协议分配）";     // 192.0.0.0/24
            if (b[0] == 192u && b[1] == 0u && b[2] == 2u)     return L"192.0.2.0/24（文档专用段）";        // 192.0.2.0/24
            if (b[0] == 192u && b[1] == 168u)                 return L"192.168.0.0/16（内网）";            // 192.168.0.0/16
            if (b[0] == 198u && (b[1] & 0xFEu) == 18u)        return L"198.18.0.0/15（基准测试段）";       // 198.18.0.0/15
            if (b[0] == 198u && b[1] == 51u && b[2] == 100u)  return L"198.51.100.0/24（文档专用段）";     // 198.51.100.0/24
            if (b[0] == 203u && b[1] == 0u && b[2] == 113u)   return L"203.0.113.0/24（文档专用段）";      // 203.0.113.0/24
            if ((b[0] & 0xF0u) == 0xE0u)                      return L"224.0.0.0/4（组播）";               // 224.0.0.0/4
            if ((b[0] & 0xF0u) == 0xF0u)                      return L"240.0.0.0/4（保留 / 广播）";        // 240.0.0.0/4
            return nullptr;
        }

        // ::1 = 前 15 字节全 0、末字节为 1。单独成函数免得把上面那段读成一笔糊涂账。
        bool AllZeroExceptLast(const unsigned char b[16], unsigned char last)
        {
            for (int i = 0; i < 15; ++i)
            {
                if (b[i] != 0u)
                {
                    return false;
                }
            }
            return b[15] == last;
        }

        // ---- IPv6 保留段 ----
        // 传入 16 字节地址。返回 nullptr = 不在保留段。
        const wchar_t* Ipv6BlockedReason(const unsigned char b[16])
        {
            // IPv4 映射（::ffff:0:0/96）：剥掉前缀，按里面的 IPv4 再判一遍。
            static const unsigned char kV4MappedPrefix[12] =
                { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0xFFu, 0xFFu };
            if (std::memcmp(b, kV4MappedPrefix, sizeof(kV4MappedPrefix)) == 0)
            {
                return Ipv4BlockedReason(b + 12);
            }

            bool allZero = true;
            for (int i = 0; i < 16; ++i)
            {
                if (b[i] != 0u)
                {
                    allZero = false;
                    break;
                }
            }
            if (allZero)                                             return L"::（未指定地址）";      // ::
            if (AllZeroExceptLast(b, 1u))                            return L"::1（本机回环）";       // ::1
            if ((b[0] & 0xFEu) == 0xFCu)                             return L"fc00::/7（内网唯一本地）"; // fc00::/7
            if (b[0] == 0xFEu && (b[1] & 0xC0u) == 0x80u)            return L"fe80::/10（链路本地）"; // fe80::/10
            if (b[0] == 0xFFu)                                       return L"ff00::/8（组播）";      // ff00::/8
            if (b[0] == 0x20u && b[1] == 0x01u && b[2] == 0x0Du && b[3] == 0xB8u)
                                                                     return L"2001:db8::/32（文档专用段）"; // 2001:db8::/32
            return nullptr;
        }

        // 数字字面量落在保留段里吗？不是字面量返回 nullptr（交给 DNS 那条路）。
        const wchar_t* NumericBlockedReason(const std::wstring& literal)
        {
            IN_ADDR v4 = {};
            if (::InetPtonW(AF_INET, literal.c_str(), &v4) == 1)
            {
                return Ipv4BlockedReason(reinterpret_cast<const unsigned char*>(&v4));
            }
            IN6_ADDR v6 = {};
            if (::InetPtonW(AF_INET6, literal.c_str(), &v6) == 1)
            {
                return Ipv6BlockedReason(reinterpret_cast<const unsigned char*>(&v6));
            }
            return nullptr;
        }

        // GetAddrInfoW 要求进程先 WSAStartup。整份工具没人显式起过 Winsock（联网走
        // WinHTTP，它自己那份不算数），这里用魔法静变量保证只做一次、线程安全。
        void EnsureWinsockInitialized()
        {
            static const bool initialized = []
            {
                WSADATA data = {};
                return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            (void)initialized;
        }
    }

    bool HostIsNumericAddress(const std::wstring& host)
    {
        if (host.empty())
        {
            return false;
        }
        const std::wstring literal = StripBrackets(host);
        IN_ADDR v4 = {};
        if (::InetPtonW(AF_INET, literal.c_str(), &v4) == 1)
        {
            return true;
        }
        IN6_ADDR v6 = {};
        return ::InetPtonW(AF_INET6, literal.c_str(), &v6) == 1;
    }

    bool HostIsBlocked(const std::wstring& host, std::wstring& error)
    {
        error.clear();

        if (host.empty())
        {
            error = L"主机名是空的。";
            return true;
        }
        if (ContainsByte(host, L'@') || ContainsByte(host, L'%'))
        {
            // '@' 是 URL 里的用户信息分隔符（user@host —— 攻击者借此把连接引到别的主机），
            // '%' 在主机里只该出现在 IPv6 的 zone id（fe80::1%eth0），两种都不当普通主机名。
            error = L"主机名里带 '@' 或 '%'（用户信息 / IPv6 zone id），不当作普通主机名。";
            return true;
        }
        if (EqualsIgnoreCaseAscii(host, L"localhost") || EndsWithIgnoreCaseAscii(host, L".localhost"))
        {
            error = L"localhost 指向本机，不允许访问。";
            return true;
        }

        const std::wstring literal = StripBrackets(host);
        const wchar_t* reason = NumericBlockedReason(literal);
        if (reason != nullptr)
        {
            error = std::wstring(L"这是保留段地址（") + reason + L"），不允许访问。";
            return true;
        }
        return false;
    }

    bool ResolveBlocked(const std::wstring& host, std::wstring& error)
    {
        error.clear();

        // 字符串层先过一遍：该拒的在这里就拒，一个包都不发。
        if (HostIsBlocked(host, error))
        {
            return true;
        }
        // 数字字面量在 HostIsBlocked 里已经判过了，不值得再去"解析"它一遍。
        if (HostIsNumericAddress(host))
        {
            return false;
        }

        EnsureWinsockInitialized();

        addrinfoW hints = {};
        hints.ai_family = AF_UNSPEC;       // v4 / v6 都要，哪个落在保留段都算挡住
        hints.ai_socktype = SOCK_STREAM;
        addrinfoW* result = nullptr;
        const int rc = ::GetAddrInfoW(host.c_str(), nullptr, &hints, &result);
        if (rc != 0)
        {
            // 解析失败 = 一个地址都拿不到 = 无从证明它安全，按拒绝处理（fail closed）。
            error = L"域名解析失败（Winsock 错误码 " + std::to_wstring(rc)
                  + L"），一个地址都拿不到，按拒绝处理。";
            return true;
        }

        unsigned addressCount = 0u;
        std::wstring blockedDetail;
        for (const addrinfoW* ai = result; ai != nullptr; ai = ai->ai_next)
        {
            if (ai->ai_addr == nullptr)
            {
                continue;
            }
            wchar_t text[INET6_ADDRSTRLEN] = {};
            if (ai->ai_addr->sa_family == AF_INET && ai->ai_addrlen >= sizeof(sockaddr_in))
            {
                const sockaddr_in* in4 = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
                if (::InetNtopW(AF_INET, &in4->sin_addr, text, INET6_ADDRSTRLEN) == nullptr)
                {
                    continue;
                }
            }
            else if (ai->ai_addr->sa_family == AF_INET6 && ai->ai_addrlen >= sizeof(sockaddr_in6))
            {
                const sockaddr_in6* in6 = reinterpret_cast<const sockaddr_in6*>(ai->ai_addr);
                if (::InetNtopW(AF_INET6, &in6->sin6_addr, text, INET6_ADDRSTRLEN) == nullptr)
                {
                    continue;
                }
            }
            else
            {
                continue;
            }

            ++addressCount;
            std::wstring checkError;
            if (HostIsBlocked(text, checkError))
            {
                blockedDetail = std::wstring(L"解析到 ") + text + L"（" + checkError + L"）";
                break;
            }
        }
        ::FreeAddrInfoW(result);

        if (!blockedDetail.empty())
        {
            error = L"域名 \"" + host + L"\" " + blockedDetail + L"，不允许访问。";
            return true;
        }
        if (addressCount == 0u)
        {
            error = L"域名 \"" + host + L"\" 没有解析出任何可用地址，按拒绝处理。";
            return true;
        }
        return false;
    }
}}
