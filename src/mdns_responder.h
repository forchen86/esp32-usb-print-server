#pragma once
// 被动 mDNS responder（UDP 5353，224.0.0.251）。
//
// 只做「有人问才答」：不探测、不周期 announce、不主动广播。
// 这样空口上除了真正的查询/应答之外没有任何多余流量，Wi-Fi 不会被 mDNS
// 的定时广播反复唤醒。
//
// 应答范围：
//   _services._dns-sd._udp.local   → 服务枚举
//   _ipp._tcp.local                → AirPrint / IPP
//   _pdl-datastream._tcp.local     → 9100 RAW
//   _http._tcp.local               → 配置页
//   实例的 SRV / TXT、主机名 A 记录（含 additional，减少客户端二次查询）
void mdnsBegin();
// 双面开关变化时刷新 TXT（duplex / URF），不重启 responder
void mdnsSetDuplex(bool on);
