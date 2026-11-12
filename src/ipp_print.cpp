#include "ipp_print.h"
#include "config.h"
#include "net_print.h"
#include "printer_cfg.h"
#include "printer_identity.h"
#include "sysmon.h"
#include "usb_printer.h"
#include "wifi_manager.h"

#include <Arduino.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// 打印机状态：全固件唯一口径（IPP 属性与 SNMP 代理都读它）
//   RFC 8011 printer-state：3 = idle，4 = processing，5 = stopped
// SNMP 的 hrPrinterStatus 直接由它映射，绝不再另建一套状态机 —— 否则 Web、
// IPP、SNMP 三处对「现在到底在不在打印」会给出互相矛盾的答案。
// ---------------------------------------------------------------------------
uint8_t ippPrinterState() {
  if (!usbPrinterReady()) return 5;   // stopped：没插 / 掉线 / 故障
  if (usbPrinterPaperEmpty()) return 5; // stopped：缺纸时引擎停，bulk OUT 会一直 NAK
  if (netPrintBusy()) return 4;       // processing：正在收或正在送 USB
  return 3;                           // idle
}

// ---------------------------------------------------------------------------
// IPP 1.1 操作码 / 状态码（RFC 8011）
// ---------------------------------------------------------------------------
enum : uint16_t {
  IPP_PRINT_JOB              = 0x0002,
  IPP_VALIDATE_JOB           = 0x0004,
  IPP_CREATE_JOB             = 0x0005,
  IPP_SEND_DOCUMENT          = 0x0006,
  IPP_CANCEL_JOB             = 0x0008,
  IPP_GET_JOB_ATTRIBUTES     = 0x0009,
  IPP_GET_JOBS               = 0x000A,
  IPP_GET_PRINTER_ATTRIBUTES = 0x000B,
  IPP_IDENTIFY_PRINTER       = 0x003C,

  IPP_OK                     = 0x0000,
  IPP_OK_SUBSTITUTED         = 0x0001,
  IPP_ERR_BAD_REQUEST        = 0x0400,
  IPP_ERR_FORMAT             = 0x0405,
  IPP_ERR_INTERNAL           = 0x0500,
  IPP_ERR_NOT_SUPPORTED      = 0x0501,
  IPP_ERR_DEVICE             = 0x0504,   // server-error-device-error：打印机没插好
  IPP_ERR_TEMPORARY          = 0x0505,   // server-error-temporary-error
  IPP_ERR_BUSY               = 0x0507,   // server-error-busy：真有作业在跑
};

// value-tag（RFC 8010）
enum : uint8_t {
  TAG_OP_ATTRS      = 0x01,
  TAG_JOB_ATTRS     = 0x02,
  TAG_END_ATTRS     = 0x03,
  TAG_PRINTER_ATTRS = 0x04,
  TAG_UNSUPPORTED   = 0x05,
  VT_INTEGER        = 0x21,
  VT_BOOLEAN        = 0x22,
  VT_ENUM           = 0x23,
  VT_DATETIME       = 0x31,
  VT_TEXT           = 0x41,
  VT_NAME           = 0x42,
  VT_KEYWORD        = 0x44,
  VT_URI            = 0x45,
  VT_CHARSET        = 0x47,
  VT_LANGUAGE       = 0x48,
  VT_MIME           = 0x49,
};

namespace {

constexpr uint32_t IPP_HANDSHAKE_TIMEOUT_MS = 30000;
constexpr uint32_t IPP_ATTR_TIMEOUT_MS      = 30000;   // 属性区「整体」上限，不是每字节
constexpr size_t   IPP_MAX_ATTR_BYTES       = 32768;   // 属性区上限，防畸形包
constexpr size_t   IPP_HTTP_LINE_MAX        = 8192;    // 单行上限：防「永不换行」拖死任务
constexpr size_t   IPP_HDR_BUF              = 1024;
constexpr size_t   IPP_RESP_BUF             = 8192;

int s_listener = -1;

// newlib 不保证提供 strcasestr，自带一个大小写无关子串查找
bool containsNoCase(const char *hay, const char *needle) {
  if (!hay || !needle || !*needle) return false;
  for (const char *h = hay; *h; ++h) {
    const char *p = h, *q = needle;
    while (*p && *q && tolower((unsigned char)*p) == tolower((unsigned char)*q)) {
      ++p; ++q;
    }
    if (!*q) return true;
  }
  return false;
}

// ---- 二进制响应构造器（固定缓冲，绝不动态分配）---------------------------
struct IppBuilder {
  uint8_t  b[IPP_RESP_BUF];
  size_t   n = 0;
  bool     overflow = false;

  void put8(uint8_t v)  { if (n < sizeof(b)) b[n++] = v; else overflow = true; }
  void put16(uint16_t v){ put8((uint8_t)(v >> 8)); put8((uint8_t)v); }
  void put32(uint32_t v){ put16((uint16_t)(v >> 16)); put16((uint16_t)v); }
  void put(const void *p, size_t l) {
    if (n + l <= sizeof(b)) { memcpy(b + n, p, l); n += l; }
    else overflow = true;
  }
  void name(const char *s) {
    const size_t l = s ? strlen(s) : 0;
    put16((uint16_t)l);
    if (l) put(s, l);
  }
  void strAttr(uint8_t tag, const char *attrName, const char *v) {
    const size_t l = v ? strlen(v) : 0;
    put8(tag); name(attrName); put16((uint16_t)l);
    if (l) put(v, l);
  }
  void extraString(uint8_t tag, const char *v) { strAttr(tag, "", v); }
  void intAttr(const char *attrName, int32_t v) {
    put8(VT_INTEGER); name(attrName); put16(4); put32((uint32_t)v);
  }
  void enumAttr(const char *attrName, int32_t v) {
    put8(VT_ENUM); name(attrName); put16(4); put32((uint32_t)v);
  }
  void boolAttr(const char *attrName, bool v) {
    put8(VT_BOOLEAN); name(attrName); put16(1); put8(v ? 0x01 : 0x00);
  }
  void dateAttr(const char *attrName) {
    // 无 RTC，给一个合法固定时间；客户端只用它做展示，不影响打印
    static const uint8_t d[11] = {0x07, 0xEA, 1, 1, 0, 0, 0, 0, '+', 0, 0};
    put8(VT_DATETIME); name(attrName); put16(11); put(d, 11);
  }
  // resolution: cross-feed(4) + feed(4) + units(1, 3=dpi)
  void resolutionAttr(const char *attrName, int32_t dpi) {
    put8(0x32); name(attrName); put16(9);
    put32((uint32_t)dpi); put32((uint32_t)dpi); put8(3);
  }
  void header(uint16_t version, uint16_t status, uint32_t reqId) {
    put16(version); put16(status); put32(reqId); put8(TAG_OP_ATTRS);
  }
  void commonOpAttrs(uint16_t status) {
    strAttr(VT_CHARSET,  "attributes-charset", "utf-8");
    strAttr(VT_LANGUAGE, "attributes-natural-language", "en");
    if (status != IPP_OK) strAttr(VT_TEXT, "status-message", "see printer status page");
  }
  void finish() { put8(TAG_END_ATTRS); }
};

// 8KB 响应缓冲放静态区：既不能压在 12KB 任务栈上，也避免动态分配。
// 两个任务（属性查询 / 作业回执）都会用它，必须由 s_respMux 串行化。
IppBuilder s_resp;
SemaphoreHandle_t s_respMux = nullptr;
// s_jobReply 的读写跨任务，用一个短临界区保护（不嵌套 s_respMux，避免顺序问题）
portMUX_TYPE s_jobMux = portMUX_INITIALIZER_UNLOCKED;

// ---- 连接上下文：HTTP 头 + IPP 属性 + 文档 framing ------------------------
struct IppCtx {
  int      fd;
  uint8_t  buf[IPP_HDR_BUF];
  size_t   len = 0, pos = 0;

  bool     chunked = false;
  long     bodyRemain = -1;      // Content-Length 剩余（含 IPP 属性区）；-1 = chunked
  long     chunkRemain = 0;
  bool     chunkEof = false;
  bool     pendingCrlf = false;
  bool     bodyEof = false;

  char     host[96] = "";
  char     path[96] = "";
  uint16_t version = 0x0101;
  uint16_t operation = 0;
  uint32_t requestId = 0;
  char     jobName[64] = "job";
  char     docFormat[64] = "application/octet-stream";
  char     sides[24] = "one-sided";     // 客户端请求的 sides（未发就是 one-sided）
  bool     sidesDuplexReq = false;      // 客户端是否明确要了双面
  bool     sidesPresent = false;        // 客户端是否显式带了 sides 属性
  // IPP 的 sides 是「作业属性」，拆封装后不会进入文档字节流，打印机看不到。
  // 收到双面请求时，在文档前注入一段 PJL 作业控制头，把这个属性翻译过去。
  char     pjlPrefix[160] = "";
  size_t   pjlLen = 0;
  size_t   pjlSent = 0;
  size_t   attrBytes = 0;
  uint32_t attrDeadline = 0;     // 属性区整体截止时刻（防慢速滴灌长期占用）
};

// 从 socket 补数据到 buf。wait=true 时最多等 IPP_HANDSHAKE_TIMEOUT_MS。
// 返回 1 有数据 / 0 对端关闭 / -1 EAGAIN(仅 wait=false) / -2 错误或超时。
int rawFill(IppCtx &c, bool wait) {
  if (c.pos < c.len) return 1;
  c.pos = c.len = 0;
  const uint32_t t0 = millis();
  for (;;) {
    const int n = recv(c.fd, c.buf, sizeof(c.buf), 0);
    if (n > 0) { c.len = (size_t)n; return 1; }
    if (n == 0) return 0;
    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
      if (!wait) return -1;
      if (millis() - t0 > IPP_HANDSHAKE_TIMEOUT_MS) return -2;
      sysMonFeed();
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    return -2;
  }
}

// 读一行（\r\n 或 \n 结尾），用于 HTTP 头。只在握手阶段调用，允许等待。
// 关键：必须有「整行总字节」上限。否则客户端只要一直发不含 \n 的数据，
// rawFill 每次都能拿到数据、永不超时，这个 for(;;) 就永远不会退出，
// 而它每轮都喂狗 —— 看门狗也救不了，IPP 631 会被单条连接长期占死。
int rawLine(IppCtx &c, char *out, size_t cap) {
  size_t k = 0, consumed = 0;
  for (;;) {
    const int f = rawFill(c, true);
    if (f != 1) return f;
    const uint8_t ch = c.buf[c.pos++];
    if (++consumed > IPP_HTTP_LINE_MAX) return -2;   // 超长行：直接判坏请求
    if (ch == '\n') break;
    if (ch != '\r' && k + 1 < cap) out[k++] = (char)ch;
  }
  out[k] = '\0';
  return 1;
}

// chunked: 跳过上一个 chunk 尾部的 CRLF，再读 chunk-size 行。
// 返回 1 = 已设置 chunkRemain / 0 = 流结束 / -2 = 格式错误或超时。
int readChunkHeader(IppCtx &c) {
  if (c.pendingCrlf) {
    // 标准 CRLF；也容忍只发 LF 的实现
    for (int i = 0; i < 2; ++i) {
      const int f = rawFill(c, true);
      if (f != 1) return -2;
      const uint8_t ch = c.buf[c.pos++];
      if (ch == '\n') { c.pendingCrlf = false; break; }
      if (ch != '\r') return -2;
      if (i == 1) c.pendingCrlf = false;
    }
  }
  char line[48];
  size_t k = 0;
  for (;;) {
    const int f = rawFill(c, true);
    if (f != 1) return -2;
    const uint8_t ch = c.buf[c.pos++];
    if (ch == '\n') break;
    if (ch != '\r' && k + 1 < sizeof(line)) line[k++] = (char)ch;
  }
  line[k] = '\0';
  char *end = nullptr;
  const long sz = strtol(line, &end, 16);
  if (end == line || sz < 0) return -2;
  if (sz == 0) {
    // 结束 chunk：吞掉可能的 trailer 行，直到空行
    for (int guard = 0; guard < 16; ++guard) {
      char t[128];
      const int f = rawLine(c, t, sizeof(t));
      if (f != 1) break;
      if (t[0] == '\0') break;
    }
    c.bodyEof = true;
    return 0;
  }
  c.chunkRemain = sz;
  return 1;
}

// 从「文档体」读最多 max 字节。framing 已处理完毕（HTTP 头、IPP 属性已剥掉）。
// 返回 >0 / 0 EOF / -1 暂无数据 / -2 错误。
int bodyRead(IppCtx &c, uint8_t *dst, size_t max, bool wait) {
  if (c.bodyEof) return 0;
  if (max == 0) return -1;

  if (c.chunked) {
    for (;;) {
      if (c.bodyEof) return 0;
      if (c.chunkRemain == 0) {
        const int s = readChunkHeader(c);
        if (s == 0) return 0;
        if (s < 0) return -2;
        continue;
      }
      const int f = rawFill(c, wait);   // wait=false 时立即返回 -1，由作业泵轮询重试
      if (f != 1) return f == 0 ? -2 : -1;
      size_t n = c.len - c.pos;
      if (n > max) n = max;
      if ((long)n > c.chunkRemain) n = (size_t)c.chunkRemain;
      memcpy(dst, c.buf + c.pos, n);
      c.pos += n;
      c.chunkRemain -= (long)n;
      if (c.chunkRemain == 0) c.pendingCrlf = true;
      return (int)n;
    }
  }

  // Content-Length：优先消费 HTTP 头解析时预读的字节，之后直接 recv 进 spool（零拷贝）
  if (c.pos < c.len) {
    size_t n = c.len - c.pos;
    if (n > max) n = max;
    if (c.bodyRemain >= 0 && (long)n > c.bodyRemain) n = (size_t)c.bodyRemain;
    memcpy(dst, c.buf + c.pos, n);
    c.pos += n;
    if (c.bodyRemain >= 0) c.bodyRemain -= (long)n;
    if (c.bodyRemain == 0) c.bodyEof = true;
    return (int)n;
  }
  if (c.bodyRemain == 0) { c.bodyEof = true; return 0; }

  size_t want = max;
  if (c.bodyRemain > 0 && (long)want > c.bodyRemain) want = (size_t)c.bodyRemain;
  const uint32_t t0 = millis();
  for (;;) {
    const int n = recv(c.fd, dst, want, 0);
    if (n > 0) {
      if (c.bodyRemain > 0) c.bodyRemain -= n;
      if (c.bodyRemain == 0) c.bodyEof = true;
      return n;
    }
    if (n == 0) return -2;            // 未收满就断开，视为异常
    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
      if (!wait) return -1;           // 文档阶段：让作业泵稍后重试，不阻塞
      if (millis() - t0 > IPP_HANDSHAKE_TIMEOUT_MS) return -2;
      sysMonFeed();                   // 握手阶段：等待属性区收齐
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    return -2;
  }
}

// 逐字节读 IPP 属性区，直到 0x03。顺带抓 job-name / document-format。
// 所有读取都经过 rd8()，因此「单字节 30s 超时」不会退化成「每个字节都重开
// 30s 计时器」——整个属性区有 30s 总预算，慢速滴灌的客户端会被直接断开。
bool parseIppAttributes(IppCtx &c) {
  c.attrDeadline = millis() + IPP_ATTR_TIMEOUT_MS;
  uint8_t tmp[4];

  auto rd8 = [&](uint8_t &v) -> bool {
    if (c.attrBytes > IPP_MAX_ATTR_BYTES) return false;
    if ((int32_t)(millis() - c.attrDeadline) >= 0) return false;
    const int n = bodyRead(c, &v, 1, true);
    if (n != 1) return false;
    ++c.attrBytes;
    return true;
  };
  auto rd16 = [&](uint16_t &v) -> bool {
    if (!rd8(tmp[0]) || !rd8(tmp[1])) return false;
    v = ((uint16_t)tmp[0] << 8) | tmp[1];
    return true;
  };
  auto rd32 = [&](uint32_t &v) -> bool {
    if (!rd8(tmp[0]) || !rd8(tmp[1]) || !rd8(tmp[2]) || !rd8(tmp[3])) return false;
    v = ((uint32_t)tmp[0] << 24) | ((uint32_t)tmp[1] << 16) |
        ((uint32_t)tmp[2] << 8) | tmp[3];
    return true;
  };

  if (!rd16(c.version) || !rd16(c.operation) || !rd32(c.requestId)) return false;

  uint8_t tag;
  while (rd8(tag)) {
    if (tag == TAG_END_ATTRS) return true;
    if (tag <= TAG_UNSUPPORTED) continue;   // 属性组分隔符

    uint16_t nameLen;
    if (!rd16(nameLen)) return false;
    if (nameLen > 256) return false;
    char attrName[257];
    attrName[0] = '\0';
    for (uint16_t i = 0; i < nameLen; ++i) {
      uint8_t ch;
      if (!rd8(ch)) return false;
      if (i < sizeof(attrName) - 1) { attrName[i] = (char)ch; attrName[i + 1] = '\0'; }
    }
    uint16_t valLen;
    if (!rd16(valLen)) return false;
    static uint8_t value[512];
    const bool wantValue = (nameLen > 0) &&
        (!strcmp(attrName, "job-name") || !strcmp(attrName, "document-format") ||
         !strcmp(attrName, "sides"));
    uint16_t captured = 0;
    for (uint16_t i = 0; i < valLen; ++i) {
      uint8_t ch;
      if (!rd8(ch)) return false;
      if (wantValue && captured + 1 < sizeof(value)) value[captured++] = ch;
    }
    if (wantValue) {
      value[captured < sizeof(value) ? captured : sizeof(value) - 1] = '\0';
      if (!strcmp(attrName, "job-name")) {
        strlcpy(c.jobName, (const char *)value, sizeof(c.jobName));
      } else if (!strcmp(attrName, "sides")) {
        // 只保留已知取值，防止客户端塞超长串把日志/回执搞乱
        strlcpy(c.sides, (const char *)value, sizeof(c.sides));
        c.sidesDuplexReq = (strncmp(c.sides, "two-sided", 9) == 0);
        c.sidesPresent = true;
      } else {
        strlcpy(c.docFormat, (const char *)value, sizeof(c.docFormat));
      }
    }
    if (c.attrBytes > IPP_MAX_ATTR_BYTES) return false;
  }
  return false;
}

// ---- 发送 ------------------------------------------------------------------
bool sendAll(int fd, const uint8_t *data, size_t len) {
  const uint32_t t0 = millis();
  size_t off = 0;
  while (off < len) {
    const int n = send(fd, data + off, len - off, 0);
    if (n > 0) { off += (size_t)n; continue; }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)) {
      if (millis() - t0 > 10000) return false;
      sysMonFeed();
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    return false;
  }
  return true;
}

void sendIpp(int fd, uint16_t version, uint16_t status, uint32_t reqId,
             void (*fill)(IppBuilder &, uint32_t), const char *note = nullptr,
             const char *extraHdr = nullptr) {
  // s_resp 是 8KB 静态缓冲，但 IPP 有两个任务会发响应：
  //   · ippTask      —— 属性查询 / Get-Jobs（打印期间 Windows 会持续轮询）
  //   · ippJobTask   —— Print-Job 的早期回执
  // 不加锁时两者会同时往同一块缓冲里写，响应内容互相踩踏（客户端看到乱码属性）。
  if (s_respMux) xSemaphoreTake(s_respMux, portMAX_DELAY);
  IppBuilder &b = s_resp;
  b.n = 0;
  b.overflow = false;
  b.header(version, status, reqId);
  b.commonOpAttrs(status);
  if (note && note[0]) b.strAttr(VT_TEXT, "status-message", note);
  if (fill) fill(b, reqId);
  b.finish();
  if (b.overflow) {
    // 绝不能只发 HTTP 头不发球体：客户端会一直等。重建一个最小错误响应。
    FWLOGE_LN("[ipp] response buffer overflow, sending minimal error");
    b.n = 0; b.overflow = false;
    b.header(version, IPP_ERR_INTERNAL, reqId);
    b.commonOpAttrs(IPP_ERR_INTERNAL);
    b.finish();
  }

  char hdr[320];
  const int h = snprintf(hdr, sizeof(hdr),
      "HTTP/1.1 200 OK\r\n"
      "Server: USB-PrintServer/1.0\r\n"
      "Content-Type: application/ipp\r\n"
      "Content-Length: %u\r\n"
      "Connection: close\r\n"
      "Cache-Control: no-store\r\n"
      "%s"
      "\r\n", (unsigned)b.n, extraHdr ? extraHdr : "");
  if (h > 0) sendAll(fd, (const uint8_t *)hdr, (size_t)h);
  sendAll(fd, b.b, b.n);
  if (s_respMux) xSemaphoreGive(s_respMux);
}

// duplex 由调用方显式传入（而不是内部去读 cfg）：开机自检要把「开 / 关」两种
// 响应各构造一遍，用实测字节数证明 8KB 缓冲在更大的那一档也装得下。
void fillPrinterAttrsImpl(IppBuilder &b, bool duplex) {
  WifiStatus w = wifiManagerStatus();
  char uri[160];
  const char *host = w.staIp[0] ? w.staIp : w.apIp;
  snprintf(uri, sizeof(uri), "ipp://%s:%d%s", host, IPP_PORT, IPP_URI_PATH);

  b.put8(TAG_PRINTER_ATTRS);
  b.strAttr(VT_URI,      "printer-uri-supported", uri);
  char mdnsUri[160];
  snprintf(mdnsUri, sizeof(mdnsUri), "ipp://%s.local:%d%s",
           FW_HOSTNAME, IPP_PORT, IPP_URI_PATH);
  b.extraString(VT_URI,  mdnsUri);
  b.strAttr(VT_KEYWORD,  "uri-authentication-supported", "none");
  b.strAttr(VT_KEYWORD,  "uri-security-supported", "none");
  // 机型相关字段一律来自 USB 1284 Device ID 的自动识别结果
  b.strAttr(VT_NAME,     "printer-name", printerIdName());
  b.strAttr(VT_TEXT,     "printer-info", printerIdInfo());
  b.strAttr(VT_TEXT,     "printer-make-and-model", printerIdMakeModel());
  b.strAttr(VT_TEXT,     "printer-location", "");
  b.strAttr(VT_KEYWORD,  "printer-kind", "document");
  b.strAttr(VT_KEYWORD,  "ipp-features-supported", "ipp-everywhere");
  if (printerIdSupportsUrf()) b.extraString(VT_KEYWORD, "airprint-1.7");
  b.strAttr(VT_TEXT,     "printer-device-id", printerIdDeviceId());
  b.strAttr(VT_KEYWORD,  "which-jobs-supported", "completed");
  b.extraString(VT_KEYWORD, "not-completed");
  // 一次取值：state 与 reasons 必须来自同一时刻，否则可能报出
  // 「idle + media-empty」这种自相矛盾的组合
  const uint8_t pstate = ippPrinterState();
  b.enumAttr("printer-state", pstate);
  b.strAttr(VT_KEYWORD,  "printer-state-reasons",
            pstate == 5 ? (usbPrinterPaperEmpty() ? "media-empty" : "none") : "none");
  b.boolAttr("printer-is-accepting-jobs",
             usbPrinterReady() && !usbPrinterPaperEmpty() && !netPrintBusy());
  b.intAttr("queued-job-count", netPrintBusy() ? 1 : 0);
  b.dateAttr("printer-current-time");
  b.intAttr("printer-up-time", (int32_t)(millis() / 1000));

  b.strAttr(VT_KEYWORD, "ipp-versions-supported", "1.0");
  b.extraString(VT_KEYWORD, "1.1");
  b.extraString(VT_KEYWORD, "2.0");
  b.enumAttr("operations-supported", IPP_PRINT_JOB);
  b.enumAttr("", IPP_VALIDATE_JOB);
  b.enumAttr("", IPP_CANCEL_JOB);
  b.enumAttr("", IPP_GET_JOB_ATTRIBUTES);
  b.enumAttr("", IPP_GET_JOBS);
  b.enumAttr("", IPP_GET_PRINTER_ATTRIBUTES);
  b.enumAttr("", 0x003C);              // Identify-Printer（IPP Everywhere 要求）
  b.boolAttr("multiple-document-jobs-supported", false);

  b.strAttr(VT_CHARSET,  "charset-configured", "utf-8");
  b.strAttr(VT_CHARSET,  "charset-supported", "utf-8");
  b.strAttr(VT_LANGUAGE, "natural-language-configured", "en");
  b.strAttr(VT_LANGUAGE, "generated-natural-language-supported", "en");

  // 文档格式列表完全由 1284 Device ID 的 CMD 推导：客户端按「第一条」挑默认
  // PDL。识别到 PDF 时 PDF 排第一（体积小、USB Full Speed 下最快）；
  // 只支持 PCL/PS 的机器绝不会被误导去收 URF/PWG。
  char pdl[256];
  strlcpy(pdl, printerIdPdl(), sizeof(pdl));
  bool firstFmt = true;
  char *fmtSave = nullptr;
  for (char *tok = strtok_r(pdl, ",", &fmtSave); tok;
       tok = strtok_r(nullptr, ",", &fmtSave)) {
    if (firstFmt) {
      b.strAttr(VT_MIME, "document-format-supported", tok);
      firstFmt = false;
    } else {
      b.extraString(VT_MIME, tok);
    }
  }
  if (firstFmt) b.strAttr(VT_MIME, "document-format-supported", "application/octet-stream");
  b.strAttr(VT_MIME, "document-format-default", printerIdDefaultFormat());
  b.strAttr(VT_KEYWORD, "pdl-override-supported", "attempted");
  b.strAttr(VT_KEYWORD, "compression-supported", "none");

  // PWG Raster / URF 能力只在设备确实声明支持时上报，
  // 否则 AirPrint 会发出打印机根本不认识的 raster 数据。
  if (printerIdSupportsUrf()) {
    b.strAttr(VT_KEYWORD, "pwg-raster-document-sheet-back", "normal");
    if (duplex) {                 // 背面套印方式只在有双面时才成立
      b.extraString(VT_KEYWORD, "rotated");
      b.extraString(VT_KEYWORD, "flipped");
    }
    b.strAttr(VT_KEYWORD, "pwg-raster-document-type-supported", "sgray_8");
    b.extraString(VT_KEYWORD, "srgb_8");
    b.extraString(VT_KEYWORD, "sgray_16");
    b.strAttr(VT_KEYWORD, "pwg-raster-document-resolution-supported", "300dpi");
    b.extraString(VT_KEYWORD, "600dpi");
    b.extraString(VT_KEYWORD, "1200dpi");
    char urf[96];
    cfgUrfString(duplex, urf, sizeof(urf));
    b.strAttr(VT_KEYWORD, "urf-supported", urf);
  }

  // 彩色能力同样只在 Device ID 明确声明时才上报；默认按单色
  const bool color = printerIdSupportsColor();
  b.boolAttr("color-supported", color);
  b.strAttr(VT_KEYWORD, "print-color-mode-supported", color ? "color" : "monochrome");
  b.extraString(VT_KEYWORD, color ? "monochrome" : "auto-monochrome");
  b.strAttr(VT_KEYWORD, "print-color-mode-default", color ? "auto" : "monochrome");
  // 双面：由 Web 开关决定是否对外声明。关着只报 one-sided —— 客户端根本不会
  // 出现「双面」选项，也就不存在「选了双面却出单面纸」的静默降级。
  // 开着则把默认值也设成双面长边（用户打开这个开关，意思就是要默认双面）。
  b.strAttr(VT_KEYWORD, "sides-supported", "one-sided");
  if (duplex) {
    b.extraString(VT_KEYWORD, "two-sided-long-edge");
    b.extraString(VT_KEYWORD, "two-sided-short-edge");
    b.strAttr(VT_KEYWORD, "sides-default", "two-sided-long-edge");
  } else {
    b.strAttr(VT_KEYWORD, "sides-default", "one-sided");
  }
  b.strAttr(VT_KEYWORD, "media-supported", "iso_a4_210x297mm");
  b.extraString(VT_KEYWORD, "na_letter_8.5x11in");
  b.extraString(VT_KEYWORD, "iso_a5_148x210mm");
  b.strAttr(VT_KEYWORD, "media-default", "iso_a4_210x297mm");
  b.strAttr(VT_KEYWORD, "media-source-supported", "main");
  b.extraString(VT_KEYWORD, "manual");
  b.resolutionAttr("printer-resolution-default", 600);
  b.resolutionAttr("printer-resolution-supported", 300);
  b.resolutionAttr("", 600);
  b.resolutionAttr("", 1200);
  b.enumAttr("print-quality-supported", 3);
  b.enumAttr("", 4);
  b.enumAttr("", 5);
  b.enumAttr("print-quality-default", 4);
  b.intAttr("copies-default", 1);
  b.enumAttr("orientation-requested-supported", 3);
  b.enumAttr("", 4);
  b.strAttr(VT_KEYWORD, "job-creation-attributes-supported", "job-name");
  b.extraString(VT_KEYWORD, "document-format");
  b.extraString(VT_KEYWORD, "copies");
  if (duplex) b.extraString(VT_KEYWORD, "sides");
  b.extraString(VT_KEYWORD, "media");
}

void fillPrinterAttrs(IppBuilder &b, uint32_t) {
  fillPrinterAttrsImpl(b, cfgDuplexEnabled());
}

// 打印作业进行中的属性查询用「瘦身应答」：只回状态轮询真正需要的 6 个字段
// （几百字节），不构造 8KB 全量属性包 —— 既隔离了轮询与作业的资源竞争，
// 又不至于让 Windows / AirPrint 的状态监视把打印机当成离线。
void fillMinimalPrinterAttrs(IppBuilder &b, uint32_t) {
  char uri[64];
  snprintf(uri, sizeof(uri), "ipp://%s.local:631/ipp/print", FW_HOSTNAME);
  const uint8_t pstate = ippPrinterState();
  b.strAttr(VT_URI,      "printer-uri-supported", uri);
  b.strAttr(VT_NAME,     "printer-name", printerIdName());
  b.enumAttr("printer-state", pstate);
  b.strAttr(VT_KEYWORD,  "printer-state-reasons",
            pstate == 5 ? (usbPrinterPaperEmpty() ? "media-empty" : "marker-supply-low")
                        : "none");
  b.boolAttr("printer-is-accepting-jobs", usbPrinterReady() && !netPrintBusy());
  b.intAttr("queued-job-count", netPrintBusy() ? 1 : 0);
  b.intAttr("printer-up-time", (int32_t)(millis() / 1000));
}

// fillJobAttrs 是函数指针，拿不到本次请求的上下文；IPP 631 是单连接串行处理，
// 用文件作用域的「本次回执参数」传递即可（写与读都发生在 sendIpp 前后的同一任务）。
struct JobReplyInfo {
  bool substituted = false;
  int  state = 5;                        // RFC 8011：5=processing 8=aborted 9=completed
  char stateReason[24] = "none";
  char accepted[24]  = "one-sided";      // 实际采用值
  char requested[24] = "one-sided";      // 客户端原样请求值（降级时用于说明）
};
JobReplyInfo s_jobReply;

// 统一写入口：所有字段更新都在同一临界区里完成，避免与 Get-Job-Attributes
// 的快照读到「一半新一半旧」。字符串都很短，临界区开销可忽略。
void setJobReply(bool substituted, int state, const char *reason,
                 const char *accepted, const char *requested) {
  portENTER_CRITICAL(&s_jobMux);
  s_jobReply.substituted = substituted;
  s_jobReply.state = state;
  strlcpy(s_jobReply.stateReason, reason, sizeof(s_jobReply.stateReason));
  strlcpy(s_jobReply.accepted, accepted, sizeof(s_jobReply.accepted));
  strlcpy(s_jobReply.requested, requested, sizeof(s_jobReply.requested));
  portEXIT_CRITICAL(&s_jobMux);
}

void fillJobAttrs(IppBuilder &b, uint32_t reqId) {
  // 取一份快照：作业回执可能在另一个任务里同时更新这些字段
  JobReplyInfo jr;
  portENTER_CRITICAL(&s_jobMux);
  jr = s_jobReply;
  portEXIT_CRITICAL(&s_jobMux);

  WifiStatus w = wifiManagerStatus();
  const char *host = w.staIp[0] ? w.staIp : w.apIp;
  char uri[160];
  snprintf(uri, sizeof(uri), "ipp://%s:%d%s", host, IPP_PORT, IPP_URI_PATH);
  b.put8(TAG_JOB_ATTRS);
  b.intAttr("job-id", (int32_t)(reqId ? reqId : 1));
  char jobUri[192];
  snprintf(jobUri, sizeof(jobUri), "%s/%lu", uri, (unsigned long)(reqId ? reqId : 1));
  b.strAttr(VT_URI, "job-uri", jobUri);
  b.strAttr(VT_URI, "job-printer-uri", uri);
  b.enumAttr("job-state", jr.state);
  b.strAttr(VT_KEYWORD, "job-state-reasons", jr.stateReason);
  // 报「实际接受的值」，不是客户端要的值：双面开关关着时一律按 one-sided 走。
  b.strAttr(VT_KEYWORD, "sides", jr.accepted);
  b.strAttr(VT_NAME, "job-name", "job");
  b.intAttr("job-k-octets", 0);
  b.intAttr("time-at-creation", (int32_t)(millis() / 1000));
  b.intAttr("time-at-processing", (int32_t)(millis() / 1000));
  b.intAttr("time-at-completed", (int32_t)(millis() / 1000));
  if (jr.substituted) {
    // RFC 8011：成功但改写了属性时，必须用 0x0001 + unsupported-attributes 组
    // 明确告诉客户端「sides 被降级了」，否则客户端会以为真的双面。
    b.put8(TAG_UNSUPPORTED);
    b.strAttr(VT_KEYWORD, "sides", jr.requested);
  }
}

// 作业数据源：先吐出 PJL 双面作业头（如果有），再喂 IPP 文档区（0x03 之后）
int ippDocumentRead(void *ctx, uint8_t *buf, size_t max) {
  IppCtx *c = static_cast<IppCtx *>(ctx);
  if (c->pjlSent < c->pjlLen) {
    size_t n = c->pjlLen - c->pjlSent;
    if (n > max) n = max;
    memcpy(buf, c->pjlPrefix + c->pjlSent, n);
    c->pjlSent += n;
    return (int)n;
  }
  return bodyRead(*c, buf, max, false);
}

struct IppReplyCtx {
  int      fd;
  uint16_t version;
  uint32_t requestId;
  bool     replied;
  uint16_t status;              // IPP_OK / IPP_OK_SUBSTITUTED
  bool     substituted;         // sides 被降级（双面开关关着但客户端要了双面）
  char     requested[24];       // 客户端原始 sides 请求值
};

// 文档字节收齐即回 successful-ok，不等 USB 把几十 MB raster 慢慢吐完。
// 否则客户端 HTTP 超时会重发整份作业，导致重复打印。
void ippEarlyReply(void *p) {
  IppReplyCtx *r = static_cast<IppReplyCtx *>(p);
  r->replied = true;
  setJobReply(r->substituted, 5, "none",
              r->substituted ? "one-sided" : r->requested, r->requested);
  sendIpp(r->fd, r->version, r->status, r->requestId, fillJobAttrs);
}

// ---- 单作业并发控制 --------------------------------------------------------
// accept/解析与「跑作业」必须分开：否则第一份作业正在收数据时，第二个连接
// 只能堵在 listen backlog 里（占内存、还让客户端以为连上了）。这里把作业投给
// 独立任务，队列深度 1 —— 第二个 Print-Job 立刻拿到 busy + Retry-After。
struct IppJobReq {
  int      fd;
  uint16_t version;
  uint32_t requestId;
  bool     substitute;      // 双面被降级（投递时的判定，避免中途改开关影响回执）
  char     src[16];         // 来源 IP：乱码页归责的第一手证据
};

IppCtx        s_jobCtx;     // 队列深度 1，同一时刻只可能有一份作业用它

// ---- 来源追溯 + 属性查询限流 ------------------------------------------------
// s_connSrc / s_connIp 只在 ippTask 的 accept 里写、handleConnection 里读
// （单连接串行），不需要加锁。
char     s_connSrc[16] = "";   // 当前连接来源 IP（字符串）
uint32_t s_connIp   = 0;       // 当前连接来源 IP（网络序 raw）

// 同一 IP 的属性查询最小间隔：局域网里有的设备（手机打印 App、其它电脑的
// 状态监视）每 1~2 秒就拉一次 Get-Printer-Attributes，每次都要花 8KB 缓冲
// 构造完整属性包。限流后超频查询直接回 HTTP 503 + Retry-After，不构造属性。
#define IPP_ATTR_MIN_INTERVAL_MS 3000
struct AttrClient {
  uint32_t ip;                // 网络序
  uint32_t lastMs;            // 上次放行的时刻
};
AttrClient    s_attrClients[8];
uint32_t      s_attrBlocked = 0;
uint32_t      s_attrOldest = 0;    // 淘汰指针（简单轮转替换）

bool attrQueryAllowed(uint32_t ipNet) {
  const uint32_t now = millis();
  int slot = -1;
  for (int i = 0; i < (int)(sizeof(s_attrClients) / sizeof(s_attrClients[0])); ++i) {
    if (s_attrClients[i].ip == ipNet) { slot = i; break; }
  }
  if (slot < 0) {
    // 新 IP：抢占最旧的槽
    slot = (int)(s_attrOldest % (sizeof(s_attrClients) / sizeof(s_attrClients[0])));
    ++s_attrOldest;
    s_attrClients[slot].ip = ipNet;
    s_attrClients[slot].lastMs = 0;
  }
  if (s_attrClients[slot].lastMs && now - s_attrClients[slot].lastMs < IPP_ATTR_MIN_INTERVAL_MS) {
    ++s_attrBlocked;
    return false;
  }
  s_attrClients[slot].lastMs = now;
  return true;
}

// 限流响应：HTTP 503 + Retry-After，客户端（轮询器）会按此退避
void sendThrottled(int fd) {
  static const char body[] = "attr query rate limited";
  char hdr[192];
  const int h = snprintf(hdr, sizeof(hdr),
      "HTTP/1.1 503 Service Unavailable\r\n"
      "Server: USB-Print-Server\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: %u\r\n"
      "Retry-After: %d\r\n"
      "Connection: close\r\n\r\n",
      (unsigned)(sizeof(body) - 1), (int)(IPP_ATTR_MIN_INTERVAL_MS / 1000));
  if (h > 0) sendAll(fd, (const uint8_t *)hdr, (size_t)h);
  sendAll(fd, (const uint8_t *)body, sizeof(body) - 1);
}
QueueHandle_t s_jobQ = nullptr;

void ippJobTask(void *) {
  sysMonTaskRegister();
  for (;;) {
    IppJobReq req;
    if (xQueueReceive(s_jobQ, &req, pdMS_TO_TICKS(500)) != pdTRUE) {
      sysMonFeed();                 // 空闲也要喂狗，不能因为没作业被看门狗打死
      continue;
    }
    IppReplyCtx reply = {req.fd, req.version, req.requestId, false,
                         (uint16_t)(req.substitute ? IPP_OK_SUBSTITUTED : IPP_OK),
                         req.substitute, ""};
    strlcpy(reply.requested, s_jobCtx.sides, sizeof(reply.requested));
    netPrintSetJobSource(req.src);   // 归责：这一页是谁打的

    // 回执字段跨请求复用，每次作业前先复位，避免上一份的 aborted 状态漏下来
    setJobReply(false, 5, "none", "one-sided", s_jobCtx.sides);

    const bool ok = netPrintRunJob(ippDocumentRead, &s_jobCtx, "IPP",
                                   ippEarlyReply, &reply);
    // 只有「一个字节都没收」的路径才回错误；一旦开始送 USB 就不再回错误码，
    // 因为已送出的字节无法回滚，回错误会让打印队列重发整份文档。
    if (!reply.replied) {
      // 一个字节都没收就中止：回 aborted(8) + 临时错误，客户端会重试，
      // 而且作业状态不会停在 processing 上误导人。
      setJobReply(reply.substituted, 8, "aborted-by-system", "one-sided",
                  reply.requested);
      sendIpp(req.fd, req.version, IPP_ERR_TEMPORARY, req.requestId, fillJobAttrs,
              "作业已中止");
    } else {
      // 已经回了「接收成功」：把作业状态更新成最终值（9 completed / 8 aborted），
      // 否则后续 Get-Job-Attributes 会一直报 processing。
      setJobReply(reply.substituted, ok ? 9 : 8,
                  ok ? "none" : "aborted-by-system",
                  reply.substituted ? "one-sided" : reply.requested,
                  reply.requested);
    }
    close(req.fd);
    if (!ok) {
      FWLOGE_LN("[ipp] 作业在 USB 阶段异常，已回成功以避免客户端重发（请看串口）");
    }
    sysMonFeed();
  }
}

// 返回 true 表示 fd 的所有权已交给作业任务，调用方不要再 close。
bool handlePrintJob(int fd, IppCtx &c) {
  if (!usbPrinterReady()) {
    // 设备问题（没插/掉线）：回 device-error，客户端会保留作业等待重试，
    // 而不是当成「忙」。用 busy 会让 Windows 队列显示「正在打印」却不动。
    sendIpp(fd, c.version, IPP_ERR_DEVICE, c.requestId, nullptr, "打印机未就绪");
    return false;
  }
  if (usbPrinterPaperEmpty()) {
    // 缺纸时引擎停摆：bulk OUT 会一直 NAK，收进来也只能等到超时。
    // 直接回 device-error + 明确原因，客户端会保留作业，装纸后自动重试。
    sendIpp(fd, c.version, IPP_ERR_DEVICE, c.requestId, nullptr,
            "打印机缺纸，请装纸后重试");
    return false;
  }
  // 单作业并发：不排队。忙就立刻回 server-error-busy + Retry-After，
  // 让客户端按自己的退避策略重试 —— 比在 backlog 里堆连接干净得多。
  if (netPrintBusy() || uxQueueSpacesAvailable(s_jobQ) < 1) {
    sendIpp(fd, c.version, IPP_ERR_BUSY, c.requestId, nullptr,
            "设备正忙，请稍后重试", "Retry-After: 30\r\n");
    return false;
  }
  // 双面开关（自动/强制）关着、客户端却明确要双面：按 RFC 8011 回
  // successful-ok-ignored-or-substituted-attributes 并把 sides 列进
  // unsupported-attributes —— 让客户端看到「已降级」，而不是静默出单面纸。
  const bool duplexEffective = cfgDuplexEnabled();
  // 客户端没带 sides 时，按 sides-default 走：报了「默认双面」就得真的双面。
  const bool wantDuplex = c.sidesPresent ? c.sidesDuplexReq : duplexEffective;
  const bool substitute = c.sidesPresent && c.sidesDuplexReq && !duplexEffective;

  // 支持双面时，把 IPP 的 sides 作业属性翻译成打印机能懂的 PJL 作业控制头。
  // 纯透传会丢掉这个属性（它不在文档字节流里），这正是「选了双面却出单面」
  // 的根因。只在设备能理解 PJL 时注入，RAW 通路不注入（原厂驱动自己会发）。
  const bool inject = wantDuplex && duplexEffective && cfgPjlDuplexEnabled();
  // 客户端没显式给 sides 时，回执里的 sides 也要反映实际执行的默认值，
  // 否则会出现「按双面打印、回执却报 one-sided」。
  if (wantDuplex && !c.sidesPresent) {
    strlcpy(c.sides, "two-sided-long-edge", sizeof(c.sides));
  }
  if (inject) {
    const bool shortEdge = (strstr(c.sides, "short-edge") != nullptr);
    const int n = snprintf(c.pjlPrefix, sizeof(c.pjlPrefix),
        "\x1b%%-12345X@PJL\r\n"
        "@PJL SET DUPLEX=ON\r\n"
        "@PJL SET BINDING=%s\r\n"
        "\x1b%%-12345X",
        shortEdge ? "SHORTEDGE" : "LONGEDGE");
    c.pjlLen = (n > 0) ? (size_t)n : 0;
    if (c.pjlLen >= sizeof(c.pjlPrefix)) c.pjlLen = sizeof(c.pjlPrefix) - 1;
    c.pjlSent = 0;
  }

  FWLOGD("[ipp] Print-Job '%s' fmt=%s sides=%s%s%s attrs=%u B\n",
                c.jobName, c.docFormat, c.sides,
                substitute ? "（不支持双面，降级为 one-sided）" : "",
                inject ? "（注入 PJL 双面作业头）" : "",
                (unsigned)c.attrBytes);

  s_jobCtx = c;                 // 拷贝到静态实例：队列深度 1，不会互相踩
  s_jobCtx.fd = fd;
  IppJobReq req = {fd, c.version, c.requestId, substitute, {0}};
  strlcpy(req.src, s_connSrc, sizeof(req.src));
  if (xQueueSend(s_jobQ, &req, 0) != pdTRUE) {   // 极小概率竞态：仍按忙处理
    sendIpp(fd, c.version, IPP_ERR_BUSY, c.requestId, nullptr,
            "设备正忙，请稍后重试", "Retry-After: 30\r\n");
    return false;
  }
  return true;
}

void sendInfoPage(int fd) {
  static const char page[] =
      "<!doctype html><meta charset=utf-8><title>USB Print Server IPP</title>"
      "<h3>USB 打印服务 · IPP 631</h3>"
      "<p>打印 URL: <code>ipp://&lt;设备地址&gt;:631/ipp/print</code></p>"
      "<p>另有 RAW 9100 通路；配置页在 80 端口。</p>";
  char hdr[192];
  const int h = snprintf(hdr, sizeof(hdr),
      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
      "Content-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
      (unsigned)(sizeof(page) - 1));
  if (h > 0) sendAll(fd, (const uint8_t *)hdr, (size_t)h);
  sendAll(fd, (const uint8_t *)page, sizeof(page) - 1);
}

void handleConnection(int fd) {
  IppCtx c;
  c.fd = fd;
  // 注意：绝不能在这里重置 s_jobReply —— 它属于「正在跑的作业」，
  // 由 ippJobTask 独占维护；属性查询只读取快照。
  fcntl(fd, F_SETFL, O_NONBLOCK);
  // 打印数据是单向大块流：Nagle 会把小尾巴（最后几 KB / 作业结束标记）攒着
  // 等 ACK，让客户端和打印机都白等一个 RTT。这里直接关掉。
  int nodelay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  // 尽量拉大接收缓冲（上限受 lwIP 的 TCP_WND 约束，见 platformio.ini 的
  // custom_sdkconfig）；设不上也不影响功能，只是仍用默认窗口。
  int rcvbuf = 32768;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  char line[256];
  if (rawLine(c, line, sizeof(line)) != 1) { close(fd); return; }
  if (strncmp(line, "POST", 4) != 0) {
    // GET / HEAD：浏览器探测，回一个说明页（Windows 有时会先 GET 探测 URL）
    sendInfoPage(fd);
    close(fd);
    return;
  }
  char *sp = strchr(line, ' ');
  if (sp) {
    char *ep = strchr(sp + 1, ' ');
    const size_t l = ep ? (size_t)(ep - (sp + 1)) : strlen(sp + 1);
    const size_t k = min(l, sizeof(c.path) - 1);
    memcpy(c.path, sp + 1, k);
    c.path[k] = '\0';
  }

  bool expect100 = false;
  for (int guard = 0; guard < 40; ++guard) {
    if (rawLine(c, line, sizeof(line)) != 1) { close(fd); return; }
    if (line[0] == '\0') break;
    if (!strncasecmp(line, "Host:", 5)) {
      const char *v = line + 5;
      while (*v == ' ') ++v;
      strlcpy(c.host, v, sizeof(c.host));
    } else if (!strncasecmp(line, "Content-Length:", 15)) {
      c.bodyRemain = atol(line + 15);
    } else if (!strncasecmp(line, "Transfer-Encoding:", 18)) {
      if (containsNoCase(line + 18, "chunked")) { c.chunked = true; c.bodyRemain = -1; }
    } else if (!strncasecmp(line, "Expect:", 7)) {
      if (containsNoCase(line + 7, "100-continue")) expect100 = true;
    }
  }
  if (expect100) {
    static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
    sendAll(fd, (const uint8_t *)cont, sizeof(cont) - 1);
  }
  if (!c.chunked && c.bodyRemain < 0) {
    // 无长度、无 chunked：IPP 请求不合法，但 Windows 有时发空探测包
    sendIpp(fd, 0x0101, IPP_ERR_BAD_REQUEST, 0, nullptr, "missing body length");
    close(fd);
    return;
  }

  if (!parseIppAttributes(c)) {
    FWLOGE_LN("[ipp] bad IPP attribute block");
    sendIpp(fd, 0x0101, IPP_ERR_BAD_REQUEST, c.requestId, nullptr, "malformed IPP request");
    close(fd);
    return;
  }
  FWLOGD("[ipp] op=0x%04X ver=0x%04X id=%lu path=%s body=%s sides=%s\n",
                c.operation, c.version, (unsigned long)c.requestId, c.path,
                c.chunked ? "chunked" : "content-length", c.sides);

  bool fdTaken = false;
  switch (c.operation) {
    case IPP_GET_PRINTER_ATTRIBUTES:
      // 同 IP 限流：超频的属性查询不构造 8KB 属性包，直接 503 + Retry-After
      if (!attrQueryAllowed(s_connIp)) {
        FWLOGD("[ipp] 属性查询限流 ip=%s（累计 %lu 次）\n",
               s_connSrc, (unsigned long)s_attrBlocked);
        sendThrottled(fd);
        break;
      }
      // 与打印作业隔离：作业进行中只回瘦身应答（几百字节、6 个核心字段），
      // 不构造全量属性包 —— 轮询设备拿到的状态仍是实时的。
      if (netPrintBusy()) {
        sendIpp(fd, c.version, IPP_OK, c.requestId, fillMinimalPrinterAttrs);
        break;
      }
      sendIpp(fd, c.version, IPP_OK, c.requestId, fillPrinterAttrs);
      break;

    case IPP_PRINT_JOB:
      fdTaken = handlePrintJob(fd, c);
      break;

    case IPP_VALIDATE_JOB:
      sendIpp(fd, c.version, IPP_OK, c.requestId, nullptr);
      break;

    case IPP_GET_JOBS:
      sendIpp(fd, c.version, IPP_OK, c.requestId, nullptr);
      break;

    case IPP_GET_JOB_ATTRIBUTES:
    case IPP_CANCEL_JOB:
      sendIpp(fd, c.version, IPP_OK, c.requestId, fillJobAttrs);
      break;

    case IPP_IDENTIFY_PRINTER:
      sendIpp(fd, c.version, IPP_OK, c.requestId, nullptr);
      break;

    case IPP_CREATE_JOB:
    case IPP_SEND_DOCUMENT:
      // 刻意不支持多文档作业：Windows 使用单文档 Print-Job，
      // 支持 Create-Job/Send-Document 会带来跨连接作业状态，稳定性代价过高。
      sendIpp(fd, c.version, IPP_ERR_NOT_SUPPORTED, c.requestId, nullptr,
              "use single-document Print-Job");
      break;

    default:
      sendIpp(fd, c.version, IPP_ERR_NOT_SUPPORTED, c.requestId, nullptr,
              "operation not supported");
      break;
  }
  // Print-Job 的 fd 已交给作业任务，这里不能关（否则会掐断正在收的数据）
  if (!fdTaken) close(fd);
}

bool ensureListener() {
  if (s_listener >= 0) return true;
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return false;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(IPP_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  // backlog 8：打印期间本任务不 accept，靠队列兜住客户端的并发探测/状态查询，
  // 太小会让新连接被直接丢弃，客户端可能判定打印机离线。
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) < 0 ||
      listen(fd, 8) < 0 ||
      fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
    FWLOGE("[ipp] listen on %d failed (errno=%d), retrying\n", IPP_PORT, errno);
    close(fd);
    return false;
  }
  s_listener = fd;
  netPrintSetIppListening(true);
  char m[96];
  snprintf(m, sizeof(m), "IPP 打印服务已监听 TCP %d %s", IPP_PORT, IPP_URI_PATH);
  FWLOGI("[ipp] %s\n", m);
  return true;
}

void ippTask(void *) {
  sysMonTaskRegister();
  for (;;) {
    sysMonFeed();
    if (!ensureListener()) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    // 用 select 等连接：有 SYN 时立刻醒（连接建立零延迟），没事时最多睡 200ms。
    // 原来「accept 失败 → delay(10ms)」是每秒 100 次空转唤醒，这里降到 5 次。
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s_listener, &rfds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    const int r = select(s_listener + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0) continue;                 // 超时 / EINTR：回去喂狗再等
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    const int fd = accept(s_listener, reinterpret_cast<struct sockaddr *>(&sa), &sl);
    if (fd >= 0) {
      // 记录来源：局域网里若有设备每 2s 轮询属性（手机打印 App / 其它电脑），
      // 串口一行就能定位是谁 —— 排查「轮询与打印并发」类问题必需。
      const uint8_t *a = reinterpret_cast<const uint8_t *>(&sa.sin_addr.s_addr);
      snprintf(s_connSrc, sizeof(s_connSrc), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
      s_connIp = sa.sin_addr.s_addr;   // 限流用（网络序 raw）
      FWLOGD("[ipp] conn from %s:%u\n", s_connSrc, (unsigned)ntohs(sa.sin_port));
      handleConnection(fd);    // 单连接串行，杜绝两作业交错进 USB
    }
  }
}
}  // namespace

void ippPrintBegin() {
  // s_resp 由「属性查询」和「作业回执」两个任务共用，必须先有锁
  s_respMux = xSemaphoreCreateMutex();
  if (!s_respMux) {
    sysMonRequestRestart("ipp mutex failed");
    return;
  }
  // 启动自检：把「Get-Printer-Attributes」响应完整构造一遍，确认 8KB 缓冲
  // 装得下。双面开关会改变属性条数，所以开 / 关两种都构造一遍，取大的那个
  // 来证明余量足够。一旦溢出，sendIpp 只能退化成最小错误响应，AirPrint /
  // Windows 会认为这台打印机不可用 —— 这类问题只有在真机上量过才知道。
  for (int d = 0; d <= 1; ++d) {
    s_resp.n = 0;
    s_resp.overflow = false;
    s_resp.header(0x0101, IPP_OK, 1);
    s_resp.commonOpAttrs(IPP_OK);
    fillPrinterAttrsImpl(s_resp, d != 0);
    s_resp.finish();
    FWLOGD("[ipp] Get-Printer-Attributes 响应（双面%s）%u / %u B%s\n",
                  d ? "开" : "关", (unsigned)s_resp.n, (unsigned)IPP_RESP_BUF,
                  s_resp.overflow ? "  >>> 溢出！" : "");
  }

  // 作业队列深度 1：允许「一个在跑」，第二个请求直接 busy + Retry-After。
  s_jobQ = xQueueCreate(1, sizeof(IppJobReq));
  if (!s_jobQ) {
    sysMonRequestRestart("ipp queue failed");
    return;
  }

  // 网络侧任务固定在 CORE_NET：与 Wi-Fi/LwIP 同核，绝不与 USB 抢核 1
  if (xTaskCreatePinnedToCore(ippJobTask, "ipp-job", 8192, nullptr, 3, nullptr,
                              CORE_NET) != pdPASS ||
      xTaskCreatePinnedToCore(ippTask, "ipp631", 12288, nullptr, 3, nullptr,
                              CORE_NET) != pdPASS) {
    sysMonRequestRestart("ipp task failed");
  }
}
