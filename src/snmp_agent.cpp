#include "snmp_agent.h"
#include "config.h"
#include "ipp_print.h"
#include "printer_identity.h"
#include "sysmon.h"
#include "usb_printer.h"

#include <Arduino.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <string.h>

// ============================================================================
// 极简 SNMP v1/v2c 状态代理
//
// 目的只有一个：让 Windows 的「TCP/IP 端口 → SNMP 状态」和各类网管能问到
// 「这台打印机在不在、忙不忙」。因此：
//   · 只读：GET / GETNEXT，SET 直接丢弃；
//   · 状态唯一来源 = ippPrinterState()（IPP 的 printer-state），
//     绝不在这里另建一套状态判断；
//   · 独立 UDP socket + 独立任务，和 9100/631 完全不复用。
//
// 不在板端做任何格式转换：本代理只回答「状态」，不参与打印数据。
// ============================================================================
namespace {

constexpr size_t kReqMax  = 512;
constexpr size_t kRespMax = 768;
constexpr int    kMaxVb   = SNMP_MAX_VARBINDS;

// ---- BER 读写 --------------------------------------------------------------
struct Reader {
  const uint8_t *p;
  size_t len;
  size_t pos = 0;
};

bool rdTlv(Reader &r, uint8_t &tag, const uint8_t *&val, size_t &vlen) {
  if (r.pos + 2 > r.len) return false;
  tag = r.p[r.pos++];
  size_t l = r.p[r.pos++];
  if (l & 0x80) {
    const int n = l & 0x7F;
    if (n == 0 || n > 3 || r.pos + (size_t)n > r.len) return false;
    l = 0;
    for (int i = 0; i < n; ++i) l = (l << 8) | r.p[r.pos++];
  }
  if (r.pos + l > r.len) return false;
  val = r.p + r.pos;
  vlen = l;
  r.pos += l;
  return true;
}

size_t putLen(uint8_t *p, size_t l) {
  if (l < 0x80) { p[0] = (uint8_t)l; return 1; }
  if (l <= 0xFF) { p[0] = 0x81; p[1] = (uint8_t)l; return 2; }
  p[0] = 0x82; p[1] = (uint8_t)(l >> 8); p[2] = (uint8_t)l; return 3;
}

struct Writer {
  uint8_t *b;
  size_t   cap;
  size_t   n = 0;
  bool     over = false;

  void raw(const void *d, size_t l) {
    if (n + l <= cap) { memcpy(b + n, d, l); n += l; }
    else over = true;
  }
  void tlv(uint8_t tag, const void *d, size_t l) {
    uint8_t h[4];
    const size_t hl = putLen(h, l);
    raw(&tag, 1);
    raw(h, hl);
    if (l) raw(d, l);
  }
  // 先写内容，再在 mark 处插入 TLV 头（内容整体后移）
  void wrap(uint8_t tag, size_t mark) {
    const size_t content = n - mark;
    uint8_t h[4];
    const size_t hl = putLen(h, content);
    if (n + 1 + hl > cap) { over = true; return; }
    memmove(b + mark + 1 + hl, b + mark, content);
    b[mark] = tag;
    memcpy(b + mark + 1, h, hl);
    n += 1 + hl;
  }
  void integer(int32_t v) {
    uint8_t t[4] = {(uint8_t)((uint32_t)v >> 24), (uint8_t)((uint32_t)v >> 16),
                    (uint8_t)((uint32_t)v >> 8), (uint8_t)v};
    int i = 0;
    while (i < 3 && ((t[i] == 0x00 && !(t[i + 1] & 0x80)) ||
                     (t[i] == 0xFF && (t[i + 1] & 0x80)))) ++i;
    tlv(0x02, t + i, (size_t)(4 - i));
  }
  void ticks(uint32_t v) {
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    int i = 0;
    while (i < 3 && t[i] == 0) ++i;
    tlv(0x43, t + i, (size_t)(4 - i));      // TimeTicks
  }
  void counter(uint32_t v) {
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    int i = 0;
    while (i < 3 && t[i] == 0) ++i;
    tlv(0x41, t + i, (size_t)(4 - i));      // Counter32
  }
  void str(const char *s) { tlv(0x04, s, strlen(s)); }
};

int32_t beInt(const uint8_t *p, size_t n) {
  if (n > 4) { p += n - 4; n = 4; }        // 只取低 32 位
  int32_t v = (n && (p[0] & 0x80)) ? -1 : 0;
  for (size_t i = 0; i < n; ++i) v = (int32_t)(((uint32_t)v << 8) | p[i]);
  return v;
}

// ---- OID 表（必须按字典序排列，GETNEXT 依赖这个顺序）-----------------------
const uint8_t O_SYS_DESCR[]    = {0x2B, 6, 1, 2, 1, 1, 1, 0};
const uint8_t O_SYS_OBJID[]    = {0x2B, 6, 1, 2, 1, 1, 2, 0};
const uint8_t O_SYS_UPTIME[]   = {0x2B, 6, 1, 2, 1, 1, 3, 0};
const uint8_t O_SYS_CONTACT[]  = {0x2B, 6, 1, 2, 1, 1, 4, 0};
const uint8_t O_SYS_NAME[]     = {0x2B, 6, 1, 2, 1, 1, 5, 0};
const uint8_t O_SYS_LOCATION[] = {0x2B, 6, 1, 2, 1, 1, 6, 0};
const uint8_t O_SYS_SERVICES[] = {0x2B, 6, 1, 2, 1, 1, 7, 0};
const uint8_t O_HR_DEVICE[]    = {0x2B, 6, 1, 2, 1, 25, 3, 2, 1, 5, 1};
const uint8_t O_HR_PRINTER[]   = {0x2B, 6, 1, 2, 1, 25, 3, 5, 1, 1, 1};
const uint8_t O_HR_ERROR[]     = {0x2B, 6, 1, 2, 1, 25, 3, 5, 1, 2, 1};
const uint8_t O_PRT_NAME[]     = {0x2B, 6, 1, 2, 1, 43, 5, 1, 1, 16, 1};
const uint8_t O_PRT_SERIAL[]   = {0x2B, 6, 1, 2, 1, 43, 5, 1, 1, 17, 1};
const uint8_t O_PRT_PAGES[]    = {0x2B, 6, 1, 2, 1, 43, 10, 2, 1, 4, 1, 1};
const uint8_t O_PRT_TONER[]    = {0x2B, 6, 1, 2, 1, 43, 11, 1, 1, 9, 1, 1};

enum VarId {
  V_SYS_DESCR = 0, V_SYS_OBJID, V_SYS_UPTIME, V_SYS_CONTACT, V_SYS_NAME,
  V_SYS_LOCATION, V_SYS_SERVICES, V_HR_DEVICE, V_HR_PRINTER, V_HR_ERROR,
  V_PRT_NAME, V_PRT_SERIAL, V_PRT_PAGES, V_PRT_TONER, V_COUNT
};

struct VarEntry {
  const uint8_t *oid;
  uint8_t        len;
};

const VarEntry kVar[V_COUNT] = {
    {O_SYS_DESCR, sizeof(O_SYS_DESCR)}, {O_SYS_OBJID, sizeof(O_SYS_OBJID)},
    {O_SYS_UPTIME, sizeof(O_SYS_UPTIME)}, {O_SYS_CONTACT, sizeof(O_SYS_CONTACT)},
    {O_SYS_NAME, sizeof(O_SYS_NAME)}, {O_SYS_LOCATION, sizeof(O_SYS_LOCATION)},
    {O_SYS_SERVICES, sizeof(O_SYS_SERVICES)}, {O_HR_DEVICE, sizeof(O_HR_DEVICE)},
    {O_HR_PRINTER, sizeof(O_HR_PRINTER)}, {O_HR_ERROR, sizeof(O_HR_ERROR)},
    {O_PRT_NAME, sizeof(O_PRT_NAME)}, {O_PRT_SERIAL, sizeof(O_PRT_SERIAL)},
    {O_PRT_PAGES, sizeof(O_PRT_PAGES)}, {O_PRT_TONER, sizeof(O_PRT_TONER)},
};

int oidCmp(const uint8_t *a, size_t al, const uint8_t *b, size_t bl) {
  const size_t n = al < bl ? al : bl;
  const int c = memcmp(a, b, n);
  if (c) return c;
  if (al == bl) return 0;
  return al < bl ? -1 : 1;
}

int findExact(const uint8_t *oid, size_t len) {
  for (int i = 0; i < V_COUNT; ++i) {
    if (kVar[i].len == len && memcmp(kVar[i].oid, oid, len) == 0) return i;
  }
  return -1;
}

// 字典序上第一个「严格大于」的项；没有则返回 -1（endOfMibView）
int findNext(const uint8_t *oid, size_t len) {
  for (int i = 0; i < V_COUNT; ++i) {
    if (oidCmp(kVar[i].oid, kVar[i].len, oid, len) > 0) return i;
  }
  return -1;
}

// ---- 值生成：状态一律来自 ippPrinterState() --------------------------------
void writeValue(Writer &w, int id) {
  const uint8_t st = ippPrinterState();      // 3 idle / 4 processing / 5 stopped
  switch (id) {
    case V_SYS_DESCR:
      w.str(printerIdInfo());
      break;
    case V_SYS_OBJID: {
      static const uint8_t o[] = {0x2B, 6, 1, 4, 1, 11, 2, 3, 9, 1};   // hp + printer
      w.tlv(0x06, o, sizeof(o));
      break;
    }
    case V_SYS_UPTIME:
      w.ticks(millis() / 10);                // TimeTicks = 1/100 s
      break;
    case V_SYS_CONTACT:
      w.str("");
      break;
    case V_SYS_NAME:
      w.str(FW_HOSTNAME);
      break;
    case V_SYS_LOCATION:
      w.str("");
      break;
    case V_SYS_SERVICES:
      w.integer(72);                         // 应用层 + 端到端
      break;
    case V_HR_DEVICE:
      // 缺纸时引擎停摆，但设备本身没坏 —— 报 warning(3) 而不是 down(5)
      w.integer(usbPrinterPaperEmpty() ? 3 : (st == 5 ? 5 : 2));
      break;
    case V_HR_PRINTER:
      // 3 idle / 4 printing；缺纸或 stopped 归到 1 (other)
      if (usbPrinterPaperEmpty()) w.integer(1);
      else w.integer(st == 4 ? 4 : (st == 3 ? 3 : 1));
      break;
    case V_HR_ERROR: {
      uint8_t e[2] = {0, 0};
      // RFC 2790 位序是 MSB first：bit0 lowPaper=0x80, bit1 noPaper=0x40,
      // bit6 offline=0x02
      if (usbPrinterPaperEmpty()) e[0] |= 0x40;
      else if (st == 5) e[0] |= 0x02;
      w.tlv(0x04, e, sizeof(e));
      break;
    }
    case V_PRT_NAME:
      w.str(printerIdModel());
      break;
    case V_PRT_SERIAL:
      w.str(printerIdSerial());
      break;
    case V_PRT_PAGES:
      w.counter(0);                          // 同上：拿不到真实页数就报 0
      break;
    case V_PRT_TONER:
      w.integer(-3);                         // RFC 3805: -3 = unknown
      break;
    default:
      w.tlv(0x05, nullptr, 0);               // NULL
      break;
  }
}

void writeVarBind(Writer &w, int id) {
  size_t m = w.n;
  w.tlv(0x06, kVar[id].oid, kVar[id].len);
  writeValue(w, id);
  w.wrap(0x30, m);
}

// ---- 报文处理 --------------------------------------------------------------
int s_sock = -1;

void handlePacket(const uint8_t *pkt, size_t len, const struct sockaddr_in *from,
                  socklen_t fromLen) {
  Reader msg{pkt, len};
  uint8_t tag;
  const uint8_t *val;
  size_t vlen;
  if (!rdTlv(msg, tag, val, vlen) || tag != 0x30) return;
  msg = Reader{val, vlen};

  const uint8_t *verPtr;
  size_t verLen;
  if (!rdTlv(msg, tag, verPtr, verLen) || tag != 0x02) return;
  const int version = (int)beInt(verPtr, verLen);       // 0=v1, 1=v2c
  if (version != 0 && version != 1) return;             // v3 报文结构不同，直接丢弃

  const uint8_t *comm;
  size_t commLen;
  if (!rdTlv(msg, tag, comm, commLen) || tag != 0x04) return;
  const size_t wantComm = strlen(SNMP_COMMUNITY);
  if (commLen != wantComm || memcmp(comm, SNMP_COMMUNITY, commLen) != 0) return;

  const uint8_t *pdu;
  size_t pduLen;
  if (!rdTlv(msg, tag, pdu, pduLen)) return;
  const bool isNext = (tag == 0xA1);
  if (tag != 0xA0 && tag != 0xA1) return;               // 只处理 GET / GETNEXT

  Reader pr{pdu, pduLen};
  const uint8_t *reqIdP;
  size_t reqIdLen;
  if (!rdTlv(pr, tag, reqIdP, reqIdLen) || tag != 0x02) return;
  const int32_t reqId = beInt(reqIdP, reqIdLen);

  const uint8_t *skip;
  size_t skipLen;
  if (!rdTlv(pr, tag, skip, skipLen) || tag != 0x02) return;   // error-status
  if (!rdTlv(pr, tag, skip, skipLen) || tag != 0x02) return;   // error-index

  const uint8_t *list;
  size_t listLen;
  if (!rdTlv(pr, tag, list, listLen) || tag != 0x30) return;
  Reader lr{list, listLen};

  // 先把请求里的 OID 全解析出来，再统一构造应答（错误码需要先知道结果）
  const uint8_t *reqOid[kMaxVb];
  size_t         reqOidLen[kMaxVb];
  int            hit[kMaxVb];
  int            count = 0;
  while (count < kMaxVb) {
    const uint8_t *vb;
    size_t vbLen;
    if (!rdTlv(lr, tag, vb, vbLen) || tag != 0x30) break;
    Reader vr{vb, vbLen};
    const uint8_t *oid;
    size_t oidLen;
    if (!rdTlv(vr, tag, oid, oidLen) || tag != 0x06) break;
    reqOid[count] = oid;
    reqOidLen[count] = oidLen;
    hit[count] = isNext ? findNext(oid, oidLen) : findExact(oid, oidLen);
    ++count;
  }
  if (!count) return;

  // v1 遇到不存在的 OID 必须回 noSuchName(2)；v2c 用 exception 值表达
  int errStatus = 0, errIndex = 0;
  if (version == 0) {
    for (int i = 0; i < count; ++i) {
      if (hit[i] < 0) { errStatus = 2; errIndex = i + 1; break; }
    }
  }

  uint8_t out[kRespMax];
  Writer w{out, kRespMax};
  size_t msgMark = w.n;
  w.integer(version);
  w.tlv(0x04, comm, commLen);

  size_t pduMark = w.n;
  w.integer(reqId);
  w.integer(errStatus);
  w.integer(errIndex);

  size_t listMark = w.n;
  for (int i = 0; i < count; ++i) {
    if (errStatus) {
      // v1 出错：回显请求的 OID + NULL，方便客户端对齐下标
      size_t m = w.n;
      w.tlv(0x06, reqOid[i], reqOidLen[i]);
      w.tlv(0x05, nullptr, 0);
      w.wrap(0x30, m);
    } else if (hit[i] < 0) {
      // v2c: endOfMibView（GETNEXT 越界）
      size_t m = w.n;
      w.tlv(0x06, reqOid[i], reqOidLen[i]);
      w.tlv(0x82, nullptr, 0);
      w.wrap(0x30, m);
    } else {
      writeVarBind(w, hit[i]);
    }
  }
  w.wrap(0x30, listMark);
  w.wrap(0xA2, pduMark);        // GetResponse
  w.wrap(0x30, msgMark);

  if (w.over) {
    FWLOGE_LN("[snmp] 响应缓冲溢出，已丢弃");
    return;
  }
  sendto(s_sock, out, w.n, 0,
         reinterpret_cast<const struct sockaddr *>(from), fromLen);
}

void snmpTask(void *) {
  sysMonTaskRegister();
  for (;;) {
    sysMonFeed();
    if (s_sock < 0) {
      const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
      struct sockaddr_in a = {};
      a.sin_family = AF_INET;
      a.sin_port = htons(SNMP_PORT);
      a.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) < 0) {
        FWLOGE("[snmp] bind %d failed (errno=%d), retry\n", SNMP_PORT, errno);
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      fcntl(fd, F_SETFL, O_NONBLOCK);
      s_sock = fd;
      FWLOGI("[snmp] 状态代理已监听 UDP %d（community=%s，只读）\n",
                    SNMP_PORT, SNMP_COMMUNITY);
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s_sock, &rfds);
    struct timeval tv = {0, 200000};
    const int r = select(s_sock + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0) continue;

    static uint8_t pkt[kReqMax];
    struct sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    const int n = recvfrom(s_sock, pkt, sizeof(pkt), 0,
                           reinterpret_cast<struct sockaddr *>(&from), &fromLen);
    if (n > 0) handlePacket(pkt, (size_t)n, &from, fromLen);
  }
}

}  // namespace

bool snmpAgentListening() { return s_sock >= 0; }

void snmpAgentBegin() {
  // 网络侧任务：固定 CORE_NET，与 Wi-Fi/LwIP 同核，不碰 USB 所在的核 1
  if (xTaskCreatePinnedToCore(snmpTask, "snmp161", 4096, nullptr, 2, nullptr,
                              CORE_NET) != pdPASS) {
    sysMonRequestRestart("snmp task failed");
  }
}
