#pragma once

// 在线主机策略：本机 / 内网 / 保留段地址一律不许被在线更新那一层访问。
//
// 为什么单独一层：HttpGet（真正发请求）与 SetRepositoryBase（把基址写进 ini）是两个
// 互不相干的入口，但"哪些主机不许访问"必须只有一套答案 —— 收在这里，两边都调它。
//
//   · HostIsBlocked        只看字符串：空主机、带 @ / %、localhost、数字 IP 落在
//                          保留段（回环 / 内网 / 链路本地 / 文档段 / 组播等）。不碰网络。
//   · HostIsNumericAddress 主机是不是数字 IP 字面量（可带方括号的 IPv4 / IPv6）。
//                          调用方用它决定要不要再去做 DNS（字面量不用解析）。
//   · ResolveBlocked       在 HostIsBlocked 之上再解析一遍域名（GetAddrInfoW）：
//                          解析出的任何一个地址被拒、或一个地址都解析不出来，都算拒绝。
//                          这一步挡的是 DNS 重绑定 —— 域名看着无害，解析出来是 127.0.0.1。
//
// 约定：返回 true = 拒绝；拒绝时 error 里写一句中文原因。这两个判定函数都不会发 HTTP。

#include <string>

namespace Yua { namespace Online {

bool HostIsNumericAddress(const std::wstring& host);
bool HostIsBlocked(const std::wstring& host, std::wstring& error);
bool ResolveBlocked(const std::wstring& host, std::wstring& error);

}}
