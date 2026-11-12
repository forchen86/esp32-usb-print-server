#include "mdns_responder.h"
#include "config.h"
#include "printer_cfg.h"
#include "printer_identity.h"
#include "sysmon.h"

#include <Arduino.h>
#include <WiFi.h>
#include <errno.h>
#include <esp_mac.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ============================================================================
// 被动 mDNS responder
//
// 与 ESPmDNS 的区别只有一个但很关键：ESPmDNS 会按 TTL/2 定时发 unsolicited
// announce（TTL 硬编码 120s，也就是每 60 秒一轮组播），这会让 Wi-Fi 空口和
// 省电状态被无谓唤醒。本实现除了「回查询」之外不发任何包，也从不 probe。
//
// 代价是必须自己处理 DNS 报文：name 编解码、PTR/SRV/TXT/A 四种记录、
// 服务枚举。范围严格限定在本机提供的三个服务上，不做通用 resolver。
// ============================================================================
namespace {

constexpr uint16_t T_A     = 1;
constexpr uint16_t T_PTR   = 12;
constexpr uint16_t T_TXT   = 16;
constexpr uint16_t T_SRV   = 33;
constexpr uint16_t T_ANY   = 255;
constexpr uint16_t C_IN    = 1;
constexpr uint32_t TTL     = 4500;              // 75 分钟，减少客户端重复查询
constexpr size_t   kOutMax = 1400;              // 保守取值，绝不触发 IP 分片

const char *const kHost     = FW_HOSTNAME ".local";
const char *const kEnumName = "_services._dns-sd._udp.local";
const char *const kIppType  = "_ipp._tcp.local";
const char *const kRawType  = "_pdl-datastream._tcp.local";
const char *const kHttpType = "_http._tcp.local";

// 实例名在「设备识别完成后」才知道，所以不固化在 mdnsBegin()，
// 而是每次应答前按 printerIdName() 刷新一次（识别没好时先用 USBPrinter）。
char g_ippInst[96]  = "USBPrinter._ipp._tcp.local";
char g_rawInst[96]  = "USBPrinter._pdl-datastream._tcp.local";
char g_httpInst[96] = "USBPrinter._http._tcp.local";
char g_uuid[64]     = "";
char g_lastName[40] = "";
uint8_t g_mac[6]    = {0};

void refreshInstanceNames() {
  const char *name = printerIdName();
  bool changed = (strcmp(g_lastName, name) != 0);
  if (!changed && g_ippInst[0]) return;
  strlcpy(g_lastName, name, sizeof(g_lastName));
  snprintf(g_ippInst, sizeof(g_ippInst), "%s-%02X%02X%02X._ipp._tcp.local",
           name, g_mac[3], g_mac[4], g_mac[5]);
  snprintf(g_rawInst, sizeof(g_rawInst), "%s-%02X%02X%02X._pdl-datastream._tcp.local",
           name, g_mac[3], g_mac[4], g_mac[5]);
  snprintf(g_httpInst, sizeof(g_httpInst), "%s-%02X%02X%02X._http._tcp.local",
           name, g_mac[3], g_mac[4], g_mac[5]);
  if (changed) {
    FWLOGD("[mdns] 实例名：%s / %s / %s\n", g_ippInst, g_rawInst, g_httpInst);
  }
}

int s_sock = -1;

// ---- DNS name 编解码 -------------------------------------------------------
size_t putName(uint8_t *p, const char *name) {
  size_t n = 0;
  const char *s = name;
  while (*s) {
    const char *dot = strchr(s, '.');
    const size_t l = dot ? (size_t)(dot - s) : strlen(s);
    if (!l || l > 63) break;
    p[n++] = (uint8_t)l;
    memcpy(p + n, s, l);
    n += l;
    if (!dot) break;
    s = dot + 1;
  }
  p[n++] = 0;
  return n;
}

bool readName(const uint8_t *b, size_t len, size_t &pos, char *out, size_t cap) {
  size_t k = 0, p = pos;
  bool jumped = false;
  int guard = 0;
  while (p < len && guard++ < 32) {
    const uint8_t l = b[p];
    if (l == 0) { ++p; break; }
    if ((l & 0xC0) == 0xC0) {                      // 压缩指针
      if (p + 1 >= len) return false;
      const size_t target = ((size_t)(l & 0x3F) << 8) | b[p + 1];
      if (!jumped) pos = p + 2;
      jumped = true;
      p = target;
      continue;
    }
    if (l > 63 || p + 1 + l > len) return false;
    if (k + (size_t)l + 2 < cap) {
      if (k) out[k++] = '.';
      memcpy(out + k, b + p + 1, l);
      k += l;
    }
    p += 1 + (size_t)l;
  }
  out[k] = '\0';
  if (!jumped) pos = p;
  return true;
}

// ---- 输出缓冲 --------------------------------------------------------------
struct Out {
  uint8_t b[kOutMax];
  size_t  n = 0;
  bool    over = false;

  void u8(uint8_t v) { if (n < sizeof(b)) b[n++] = v; else over = true; }
  void u16(uint16_t v) { u8((uint8_t)(v >> 8)); u8((uint8_t)v); }
  void u32(uint32_t v) { u16((uint16_t)(v >> 16)); u16((uint16_t)v); }
  void bytes(const void *p, size_t l) {
    if (n + l <= sizeof(b)) { memcpy(b + n, p, l); n += l; }
    else over = true;
  }
  void name(const char *s) {
    uint8_t t[256];
    bytes(t, putName(t, s));
  }
  void rr(const char *name, uint16_t type, bool flush, const uint8_t *rd, size_t rl) {
    this->name(name);
    u16(type);
    u16(flush ? (C_IN | 0x8000) : C_IN);
    u32(TTL);
    u16((uint16_t)rl);
    bytes(rd, rl);
  }
};

uint32_t localIpv4() {
  IPAddress ip = WiFi.localIP();
  if (ip[0] == 0 && ip[1] == 0) ip = WiFi.softAPIP();
  return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
         ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}

// ---- 记录内容 --------------------------------------------------------------
size_t buildTxt(uint8_t *out, size_t cap) {
  const bool duplex = cfgDuplexEnabled();
  size_t n = 0;
  auto add = [&](const char *k, const char *v) {
    char t[340];
    const int l = snprintf(t, sizeof(t), "%s=%s", k, v);
    if (l <= 0 || l > 255 || n + 1 + (size_t)l + 1 > cap) return;
    out[n++] = (uint8_t)l;
    memcpy(out + n, t, (size_t)l);
    n += (size_t)l;
  };
  char urf[96];
  cfgUrfString(duplex, urf, sizeof(urf));
  char product[128];
  snprintf(product, sizeof(product), "(%s)", printerIdModel());
  add("txtvers", "1");
  add("qtotal", "1");
  add("rp", "ipp/print");
  add("ty", printerIdMakeModel());
  add("pdl", printerIdPdl());
  // 只有设备自己声明支持 URF 才发布 URF 键，否则 iOS 会把它当 AirPrint 打印机
  // 并发来它根本不认识的 raster 数据。
  if (printerIdSupportsUrf()) add("URF", urf);
  add("kind", "document");
  add("PaperMax", "legal-A4");
  add("duplex", duplex ? "T" : "F");
  add("color", printerIdSupportsColor() ? "T" : "F");
  add("note", "ESP32-S3 USB print bridge");
  add("adminurl", "http://" FW_HOSTNAME ".local/");
  add("priority", "50");
  add("product", product);
  add("UUID", g_uuid);
  out[n++] = 0;                       // TXT 以空字符串项结尾
  return n;
}

void buildSrv(uint8_t *out, size_t &n, uint16_t port) {
  n = 0;
  out[n++] = 0; out[n++] = 0;         // priority
  out[n++] = 0; out[n++] = 0;         // weight
  out[n++] = (uint8_t)(port >> 8);
  out[n++] = (uint8_t)port;
  n += putName(out + n, kHost);
}

bool ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

// 没有任何可用 IPv4（STA 未连且 AP 已关）时，宁可不回 A 记录，
// 也不要回 0.0.0.0 —— 那会让客户端把打印机记成一个无效地址。
bool haveIpv4() { return localIpv4() != 0; }

// 单个 question 的应答：answer + additional（SRV/TXT/A 一并给出，省掉客户端
// 的第二轮查询 —— 查询越少，空口越安静，这也正符合「被动响应」的目标）
bool answerOne(Out &o, const char *qname, uint16_t qtype, uint16_t qclass) {
  if ((qclass & 0x7FFF) != C_IN) return false;
  const bool any = (qtype == T_ANY);
  uint8_t rd[512];
  size_t rl = 0;

  if (ieq(qname, kEnumName) && (any || qtype == T_PTR)) {
    const char *svc[3] = {kIppType, kRawType, kHttpType};
    for (int i = 0; i < 3; ++i) {
      rl = putName(rd, svc[i]);
      o.rr(kEnumName, T_PTR, false, rd, rl);
    }
    return true;
  }
  if (ieq(qname, kIppType) && (any || qtype == T_PTR)) {
    rl = putName(rd, g_ippInst);
    o.rr(kIppType, T_PTR, false, rd, rl);
    buildSrv(rd, rl, IPP_PORT);
    o.rr(g_ippInst, T_SRV, true, rd, rl);
    rl = buildTxt(rd, sizeof(rd));
    o.rr(g_ippInst, T_TXT, true, rd, rl);
    const uint32_t ip = htonl(localIpv4());
    o.rr(kHost, T_A, true, reinterpret_cast<const uint8_t *>(&ip), 4);
    return true;
  }
  if (ieq(qname, kRawType) && (any || qtype == T_PTR)) {
    rl = putName(rd, g_rawInst);
    o.rr(kRawType, T_PTR, false, rd, rl);
    buildSrv(rd, rl, PRINT_PORT);
    o.rr(g_rawInst, T_SRV, true, rd, rl);
    rl = buildTxt(rd, sizeof(rd));
    o.rr(g_rawInst, T_TXT, true, rd, rl);
    const uint32_t ip = htonl(localIpv4());
    o.rr(kHost, T_A, true, reinterpret_cast<const uint8_t *>(&ip), 4);
    return true;
  }
  if (ieq(qname, kHttpType) && (any || qtype == T_PTR)) {
    rl = putName(rd, g_httpInst);
    o.rr(kHttpType, T_PTR, false, rd, rl);
    buildSrv(rd, rl, 80);
    o.rr(g_httpInst, T_SRV, true, rd, rl);
    return true;
  }
  if (ieq(qname, g_ippInst) && (any || qtype == T_SRV)) {
    buildSrv(rd, rl, IPP_PORT);
    o.rr(g_ippInst, T_SRV, true, rd, rl);
    const uint32_t ip = htonl(localIpv4());
    o.rr(kHost, T_A, true, reinterpret_cast<const uint8_t *>(&ip), 4);
    return true;
  }
  if (ieq(qname, g_rawInst) && (any || qtype == T_SRV)) {
    buildSrv(rd, rl, PRINT_PORT);
    o.rr(g_rawInst, T_SRV, true, rd, rl);
    return true;
  }
  if (ieq(qname, g_httpInst) && (any || qtype == T_SRV)) {
    buildSrv(rd, rl, 80);
    o.rr(g_httpInst, T_SRV, true, rd, rl);
    return true;
  }
  if ((ieq(qname, g_ippInst) || ieq(qname, g_rawInst) ||
       ieq(qname, g_httpInst)) && (any || qtype == T_TXT)) {
    rl = buildTxt(rd, sizeof(rd));
    o.rr(qname, T_TXT, true, rd, rl);
    return true;
  }
  if (ieq(qname, kHost) && (any || qtype == T_A)) {
    const uint32_t ip = htonl(localIpv4());
    o.rr(kHost, T_A, true, reinterpret_cast<const uint8_t *>(&ip), 4);
    return true;
  }
  return false;
}

void handleQuery(const uint8_t *b, size_t len, const struct sockaddr_in *from,
                 socklen_t fromLen) {
  if (len < 12) return;
  // 设备识别可能晚于网络启动，每次应答前刷一次实例名
  refreshInstanceNames();
  // 没有可用 IPv4 时不应答：回了 A=0.0.0.0 只会让客户端记住一个坏地址
  if (!haveIpv4()) return;
  if (b[2] & 0x80) return;                       // 是响应，不是查询：忽略
  const uint16_t qd = ((uint16_t)b[4] << 8) | b[5];
  if (!qd) return;

  size_t pos = 12;
  char qname[128];
  const size_t qStart = pos;
  if (!readName(b, len, pos, qname, sizeof(qname))) return;
  if (pos + 4 > len) return;
  const uint16_t qtype = ((uint16_t)b[pos] << 8) | b[pos + 1];
  const uint16_t qclass = ((uint16_t)b[pos + 2] << 8) | b[pos + 3];
  pos += 4;
  const size_t qEnd = pos;

  Out o;
  o.u16(0);                                      // id（组播响应恒为 0）
  o.u16(0x8400);                                 // QR + AA
  o.u16(1);                                      // qdcount：只回显这一条
  const size_t anPos = o.n;
  o.u16(0);                                      // ancount 占位
  o.u16(0);                                      // nscount
  const size_t arPos = o.n;
  o.u16(0);                                      // arcount 占位
  o.bytes(b + qStart, qEnd - qStart);            // 原样回显 question

  const size_t ansStart = o.n;
  if (!answerOne(o, qname, qtype, qclass)) return;
  const size_t ansTotal = o.n - ansStart;

  // answer 与 additional 在同一段里连续写，这里按「answer 数」简单折算：
  // 每条 answer 之后紧跟的 SRV/TXT/A 都算 additional。
  uint16_t an = 0, ar = 0;
  if (ieq(qname, kEnumName)) an = 3;
  else if (ieq(qname, kIppType) || ieq(qname, kRawType)) { an = 1; ar = 3; }
  else if (ieq(qname, kHttpType)) { an = 1; ar = 1; }
  else if (ieq(qname, g_ippInst) || ieq(qname, g_rawInst) || ieq(qname, g_httpInst)) {
    if (qtype == T_TXT) an = 1; else { an = 1; ar = 1; }
  } else an = 1;

  o.b[anPos] = (uint8_t)(an >> 8); o.b[anPos + 1] = (uint8_t)an;
  o.b[arPos] = (uint8_t)(ar >> 8); o.b[arPos + 1] = (uint8_t)ar;

  if (o.over || o.n > kOutMax) {
    FWLOGE_LN("[mdns] 应答超长，已丢弃");
    return;
  }
  (void)ansTotal;
  sendto(s_sock, o.b, o.n, 0,
         reinterpret_cast<const struct sockaddr *>(from), fromLen);
  // 标准 mDNS 客户端（Bonjour / AirPrint / dns-sd）用 5353 作源端口。留一条
  // 限频日志确认设备确实应答了这类查询：PC 上 5353 常被浏览器 / IM 占用，
  // 单播应答可能被别的进程收走，只看「客户端有没有收到」会误判成设备没应答。
  if (ntohs(from->sin_port) == 5353) {
    static uint32_t lastLog = 0;
    const uint32_t now = millis();
    if (now - lastLog > 1000) {
      lastLog = now;
      FWLOGD("[mdns] 应答标准 5353 查询 q=%s bytes=%u\n",
                    qname, (unsigned)o.n);
    }
  }
}

void mdnsTask(void *) {
  sysMonTaskRegister();
  for (;;) {
    sysMonFeed();
    if (s_sock < 0) {
      const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
      int yes = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
      struct sockaddr_in a = {};
      a.sin_family = AF_INET;
      a.sin_port = htons(5353);
      a.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) < 0) {
        FWLOGE("[mdns] bind 5353 failed (errno=%d), retry\n", errno);
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      struct ip_mreq mreq = {};
      mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
      mreq.imr_interface.s_addr = htonl(INADDR_ANY);
      if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        FWLOGE("[mdns] join 224.0.0.251 failed (errno=%d)\n", errno);
      }
      fcntl(fd, F_SETFL, O_NONBLOCK);
      s_sock = fd;
      FWLOGI("[mdns] 被动应答已启动（%s / %s / %s，不主动广播）\n",
                    kIppType, kRawType, kHttpType);
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s_sock, &rfds);
    struct timeval tv = {0, 200000};
    const int r = select(s_sock + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0) continue;

    static uint8_t pkt[1200];
    struct sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    const int n = recvfrom(s_sock, pkt, sizeof(pkt), 0,
                           reinterpret_cast<struct sockaddr *>(&from), &fromLen);
    if (n <= 0) continue;
    // 不能按「源端口 == 5353」过滤！Bonjour / Avahi / dns-sd / iOS AirPrint
    // 的标准查询就是从 5353 发出的，过滤掉会导致设备永远不被 AirPrint 发现
    // （实测 0/8 应答）。区分「别人的响应」交给 handleQuery 里的 QR 位判断；
    // 别人的查询 QR=0 但服务名不匹配，answerOne() 会直接返回 false 不应答。
    // 本 responder 只做单播应答，也不会收到自己发出的组播包。
    handleQuery(pkt, (size_t)n, &from, fromLen);
  }
}

}  // namespace

void mdnsBegin() {
  esp_read_mac(g_mac, ESP_MAC_WIFI_STA);
  snprintf(g_uuid, sizeof(g_uuid),
           "%02X%02X%02X%02X-%02X%02X-4%02X%02X-8%02X%02X-%02X%02X%02X%02X%02X%02X",
           g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5],
           g_mac[0], g_mac[1], g_mac[2], g_mac[3],
           g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
  refreshInstanceNames();   // 先用默认名；识别完成后 handleQuery 会自动更新

  if (xTaskCreatePinnedToCore(mdnsTask, "mdns5353", 5120, nullptr, 2, nullptr,
                              CORE_NET) != pdPASS) {
    sysMonRequestRestart("mdns task failed");
  }
}
