#include "printer_identity.h"
#include "config.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace {

struct Identity {
  char make[32]         = "USB";
  char model[64]        = "USB Printer";
  char des[96]          = "";          // DES：厂商给的销售型号/描述，常比 MDL 更具体
  char pubModel[64]     = "USB Printer"; // 对外发布用的型号：DES 优先，回退 MDL
  char name[32]         = "USBPrinter";
  char info[96]         = "USB Printer via ESP32-S3";
  char makeModel[96]    = "USB Printer";
  char deviceId[512]    = "MFG:Generic;MDL:USB Printer;CLS:PRINTER;";
  char commands[256]    = "";
  char serial[48]       = "";
  char pdl[256]         = "application/pdf,application/postscript,application/octet-stream";
  char defaultFmt[64]   = "application/pdf";
  bool urf              = false;
  bool color            = false;
  bool guessed          = true;   // true = 没能从 Device ID 推导，用的是保守默认值
  bool duplex           = false;  // 自动双面单元
  bool pjl              = true;   // 能否收 PJL 作业控制指令
  char dupEvidence[64]  = "未检测到双面能力";
};

Identity s_id;
volatile bool s_ready = false;

void copyField(char *dst, size_t cap, const char *src, size_t n) {
  if (!src || !cap) return;
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
  // 1284 串里常带首尾空格，清掉
  while (n && (unsigned char)dst[n - 1] == ' ') dst[--n] = '\0';
  size_t lead = 0;
  while (dst[lead] == ' ') ++lead;
  if (lead) memmove(dst, dst + lead, n - lead + 1);
}

// 把 model 清成 DNS / IPP 安全的短名：只留字母数字和 '-'，空格转 '-'
void sanitizeName(const char *model) {
  size_t k = 0;
  for (const char *p = model; *p && k < sizeof(s_id.name) - 1; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (isalnum(c)) s_id.name[k++] = (char)c;
    else if ((c == ' ' || c == '-' || c == '_') && k && s_id.name[k - 1] != '-')
      s_id.name[k++] = '-';
  }
  s_id.name[k] = '\0';
  if (!k) strlcpy(s_id.name, "USBPrinter", sizeof(s_id.name));
}

// 从 "KEY:value;KEY:value;" 中取某个键（大小写不敏感，支持多个别名）
bool getKey(const char *s, size_t len, const char *const *keys, int keyCount,
            const char *&val, size_t &valLen) {
  size_t pos = 0;
  while (pos < len) {
    size_t end = pos;
    while (end < len && s[end] != ';') ++end;
    const char *field = s + pos;
    const size_t fieldLen = end - pos;
    const char *colon = (const char *)memchr(field, ':', fieldLen);
    if (colon) {
      const size_t kLen = (size_t)(colon - field);
      for (int i = 0; i < keyCount; ++i) {
        const size_t want = strlen(keys[i]);
        if (want == kLen && strncasecmp(field, keys[i], kLen) == 0) {
          val = colon + 1;
          valLen = fieldLen - kLen - 1;
          return true;
        }
      }
    }
    pos = end + 1;
  }
  return false;
}

struct CmdMap {
  const char *key;
  const char *mime;
};

// 注意顺序：先长后短，避免 PCL 抢在 PCLm/PCLXL 之前命中
const CmdMap kCmdMap[] = {
    {"POSTSCRIPT", "application/postscript"},
    {"PCLXL", "application/vnd.hp-PCLXL"},
    {"PCL6", "application/vnd.hp-PCLXL"},
    {"PCLM", "application/vnd.hp-PCLm"},
    {"PCL5", "application/vnd.hp-PCL"},
    {"PCL", "application/vnd.hp-PCL"},
    {"PWGRASTER", "image/pwg-raster"},
    {"PWG", "image/pwg-raster"},
    {"URF", "image/urf"},
    // HP 的 1284 Device ID 常用 URP 表示 URF/AirPrint raster（M203-M206 实测即为 URP）。
    // 不认这个别名会把支持 AirPrint 的机器误判成「不支持 URF」，导致 iOS/macOS 不再发现它。
    {"URP", "image/urf"},
    {"PDF", "application/pdf"},
    {"JPEG", "image/jpeg"},
    {"PNG", "image/png"},
    {"PS", "application/postscript"},
    {"PS3", "application/postscript"},
    {"ESCPOS", "application/octet-stream"},
    {"ZPL", "application/octet-stream"},
    {"TSPL", "application/octet-stream"},
};

// 不依赖 strcasestr（newlib 没有），自己做大小写不敏感子串查找
const char *ciFind(const char *hay, const char *needle) {
  if (!hay || !needle || !*needle) return nullptr;
  for (const char *h = hay; *h; ++h) {
    const char *a = h, *b = needle;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
      ++a;
      ++b;
    }
    if (!*b) return h;
  }
  return nullptr;
}

bool hasMime(const char *list, const char *mime) {
  if (!mime || !*mime) return true;
  const size_t n = strlen(mime);
  const char *p = list;
  while ((p = strstr(p, mime)) != nullptr) {
    const bool left = (p == list || p[-1] == ',');
    const bool right = (p[n] == '\0' || p[n] == ',');
    if (left && right) return true;
    p += n;
  }
  return false;
}

void appendMime(char *out, size_t cap, const char *mime) {
  if (!mime || !*mime) return;
  if (strcmp(mime, "application/octet-stream") == 0) return;   // 保底项最后统一加
  if (hasMime(out, mime)) return;
  const size_t used = strlen(out);
  if (used + strlen(mime) + 2 > cap) return;
  if (used) strlcat(out, ",", cap);
  strlcat(out, mime, cap);
}

// 按 CMD 推导出 MIME 列表。PDF 若能确认就放第一（客户端按第一条选默认格式）。
void buildPdl(const char *cmd, size_t cmdLen, bool deviceIdOk) {
  s_id.pdl[0] = '\0';
  s_id.urf = false;
  s_id.color = false;

  if (!deviceIdOk || !cmd || !cmdLen) {
    // 读不到 Device ID：给一个保守集合，绝不谎报 URF / 彩色能力
    strlcpy(s_id.pdl,
            "application/pdf,application/postscript,application/vnd.hp-PCL,"
            "application/octet-stream", sizeof(s_id.pdl));
    s_id.guessed = true;
    return;
  }

  char tmp[256];
  copyField(tmp, sizeof(tmp), cmd, cmdLen);
  // 颜色能力必须在 strtok 破坏 tmp 之前判定
  const bool colorAdvertised = ciFind(tmp, "SRGB") || ciFind(tmp, "COLOR");

  // 先把 PDF 提到首位
  if (ciFind(tmp, "PDF")) appendMime(s_id.pdl, sizeof(s_id.pdl), "application/pdf");

  // 按 CMD 里的顺序逐个映射
  char *save = nullptr;
  for (char *tok = strtok_r(tmp, ",;", &save); tok;
       tok = strtok_r(nullptr, ",;", &save)) {
    while (*tok == ' ') ++tok;
    char up[32];
    size_t k = 0;
    for (const char *p = tok; *p && k < sizeof(up) - 1; ++p) {
      up[k++] = (char)toupper((unsigned char)*p);
    }
    up[k] = '\0';
    if (!k) continue;
    if (strncmp(up, "URF", 3) == 0 || strncmp(up, "URP", 3) == 0) s_id.urf = true;
    for (const auto &m : kCmdMap) {
      if (strncmp(up, m.key, strlen(m.key)) == 0) {
        appendMime(s_id.pdl, sizeof(s_id.pdl), m.mime);
        break;
      }
    }
  }

  // 彩色：Device ID 明确给出颜色能力才报 true，否则一律按单色
  s_id.color = colorAdvertised;

  // HP 用「IPP-E:」字段声明 IPP Everywhere 能力（M203-M206 实测含此字段）。
  // IPP Everywhere 规范要求支持 PWG Raster，这类机器也基本都吃 URF。
  // 即使 CMD 没写 URF/PWG，也不能把 AirPrint 能力弄丢 —— 否则 iOS/macOS
  // 会把原本免驱的打印机从列表里剔除，这是去机型化时最容易发生的回归。
  if (ciFind(s_id.deviceId, "IPP-E:")) {
    appendMime(s_id.pdl, sizeof(s_id.pdl), "image/pwg-raster");
    appendMime(s_id.pdl, sizeof(s_id.pdl), "image/urf");
    s_id.urf = true;
  }

  if (!s_id.pdl[0]) {
    strlcpy(s_id.pdl,
            "application/pdf,application/postscript,application/vnd.hp-PCL,"
            "application/octet-stream", sizeof(s_id.pdl));
    s_id.guessed = true;
  } else {
    strlcat(s_id.pdl, ",application/octet-stream", sizeof(s_id.pdl));
    s_id.guessed = false;
  }
  strlcpy(s_id.defaultFmt, s_id.pdl, sizeof(s_id.defaultFmt));
  if (const char *comma = strchr(s_id.defaultFmt, ',')) *const_cast<char *>(comma) = '\0';
}

// HP / Brother / Kyocera 等厂商用型号后缀 d / dn / dw / dtn / fdw 表示内置双面单元。
// 只取最后一个空格后的型号段，从末尾跳过网络/无线类附加后缀(n/w/t/x/s)，
// 再看是否停在 d，且 d 前面仍是型号编号 —— 避免把普通单词误判成双面。
bool modelSuffixDuplex(const char *s) {
  if (!s || !*s) return false;
  const char *last = strrchr(s, ' ');
  last = last ? last + 1 : s;
  size_t l = strlen(last);
  while (l) {
    const char c = (char)tolower((unsigned char)last[l - 1]);
    if (c != 'n' && c != 'w' && c != 't' && c != 'x' && c != 's') break;
    --l;
  }
  if (l < 2 || tolower((unsigned char)last[l - 1]) != 'd') return false;
  return isalnum((unsigned char)last[l - 2]) != 0;
}

// 双面能力没有统一的 1284 标准字段，所以用多条证据按强度依次判定。
// 型号后缀 d / dw / dn 是 HP、Brother 等厂商表示 duplex 的通行命名（作弱证据）。
void detectDuplexAndPjl() {
  s_id.duplex = false;
  strlcpy(s_id.dupEvidence, "没找到双面相关的信息", sizeof(s_id.dupEvidence));

  if (ciFind(s_id.deviceId, "DUPLEX")) {
    s_id.duplex = true;
    strlcpy(s_id.dupEvidence, "设备自报支持双面", sizeof(s_id.dupEvidence));
  } else {
    static const char *const kCap[] = {"CAP", "CAPABILITIES"};
    const char *v = nullptr;
    size_t vl = 0;
    if (getKey(s_id.deviceId, strlen(s_id.deviceId), kCap, 2, v, vl) &&
        (ciFind(v, "DUP") != nullptr)) {
      s_id.duplex = true;
      strlcpy(s_id.dupEvidence, "设备能力表里写了双面", sizeof(s_id.dupEvidence));
    } else if (ciFind(s_id.commands, "DUPLEX")) {
      s_id.duplex = true;
      strlcpy(s_id.dupEvidence, "命令集里包含双面", sizeof(s_id.dupEvidence));
    } else if (!s_id.guessed) {
      // 后缀启发式必须同时看 MDL 与 DES。HP 的 MDL 常是「系列名」
      // （实测 MDL:HP LaserJet M203-M206，末位是 6），真正的销售型号放在 DES
      // （DES:HP LaserJet M203d，带 d = 自动双面）。只看 MDL 会把双面机漏判成单面，
      // 这正是「固件识别不出自动双面打印机」的根因。
      const char *hit = modelSuffixDuplex(s_id.des) ? s_id.des : nullptr;
      const char *src = "DES";
      if (!hit && modelSuffixDuplex(s_id.model)) {
        hit = s_id.model;
        src = "MDL";
      }
      if (hit) {
        s_id.duplex = true;
        snprintf(s_id.dupEvidence, sizeof(s_id.dupEvidence),
                 "型号以 d 结尾，通常代表双面机型（%s）", src);
      }
    }
  }

  // 只有能理解 PCL / PS / PDF / URF / PWG 的机器才适合收 PJL 头；
  // ESC/POS、ZPL 之类标签语言注入 PJL 会污染作业，必须禁止。
  s_id.pjl = (strstr(s_id.pdl, "PCL") != nullptr ||
              strstr(s_id.pdl, "postscript") != nullptr ||
              strstr(s_id.pdl, "pdf") != nullptr ||
              strstr(s_id.pdl, "urf") != nullptr ||
              strstr(s_id.pdl, "pwg-raster") != nullptr);
}

void publishCommon() {
  // 对外发布的型号优先用 DES：HP 的 MDL 往往是系列名
  // （实测 MDL:HP LaserJet M203-M206），DES 才是具体销售型号
  // （DES:HP LaserJet M203d）。客户端按这个名字认打印机，用系列名会让人看不出机型。
  // DES 缺失、过长或含不可打印字符时回退 MDL，避免把一句描述当成型号发布出去。
  const char *pub = s_id.model;
  const size_t dl = strlen(s_id.des);
  bool usable = dl >= 3 && dl < sizeof(s_id.pubModel);
  if (usable) {
    for (size_t i = 0; i < dl; ++i) {
      const unsigned char c = (unsigned char)s_id.des[i];
      if (c < 0x20 || c > 0x7e) { usable = false; break; }
    }
  }
  if (usable) pub = s_id.des;
  strlcpy(s_id.pubModel, pub, sizeof(s_id.pubModel));
  sanitizeName(s_id.pubModel);
  detectDuplexAndPjl();
  snprintf(s_id.info, sizeof(s_id.info), "%s via ESP32-S3 Print Server", s_id.pubModel);
  // MDL / DES 里常已含厂商名（实测 "HP LaserJet M203d" + MFG:HP），
  // 再拼一次会变成 "HP HP LaserJet ..."，这种重复在 IPP / mDNS 里很难看。
  const bool prefixed = s_id.make[0] && strcmp(s_id.make, "USB") != 0 &&
                        strncasecmp(s_id.pubModel, s_id.make, strlen(s_id.make)) == 0;
  if (s_id.make[0] && strcmp(s_id.make, "USB") != 0 && !prefixed)
    snprintf(s_id.makeModel, sizeof(s_id.makeModel), "%s %s", s_id.make, s_id.pubModel);
  else
    strlcpy(s_id.makeModel, s_id.pubModel, sizeof(s_id.makeModel));
  if (!s_id.deviceId[0]) {
    snprintf(s_id.deviceId, sizeof(s_id.deviceId),
             "MFG:%s;MDL:%s;CLS:PRINTER;DES:%s;", s_id.make, s_id.model, s_id.pubModel);
  }
}

}  // namespace

void printerIdSetFromDeviceId(const uint8_t *buf, size_t len) {
  if (!buf || len < 4) return;
  // 标准格式是「2 字节大端长度（含自身）+ ASCII」。但有些打印机的长度字段
  // 写错或用小端，这时退化为「跳过 2 字节后用全部数据」，不能因此整机不识别。
  const size_t declared = ((size_t)buf[0] << 8) | buf[1];
  size_t n = (declared >= 4 && declared <= len) ? declared - 2 : len - 2;
  if (n > len - 2) n = len - 2;
  const char *s = reinterpret_cast<const char *>(buf + 2);
  if (!n) return;

  static const char *const kMake[] = {"MFG", "MANUFACTURER", "MAKE"};
  static const char *const kModel[] = {"MDL", "MODEL"};
  static const char *const kCmd[] = {"CMD", "COMMAND SET", "COMMANDS"};
  static const char *const kDes[] = {"DES", "DESCRIPTION"};
  static const char *const kSn[] = {"SN", "SERIALNUMBER", "SERNUM"};

  // 先落整条 Device ID：buildPdl 需要从中读 IPP-E / CID 等能力字段
  copyField(s_id.deviceId, sizeof(s_id.deviceId), s, n);

  const char *v = nullptr;
  size_t vl = 0;
  if (getKey(s, n, kMake, 3, v, vl)) copyField(s_id.make, sizeof(s_id.make), v, vl);
  if (getKey(s, n, kModel, 2, v, vl)) copyField(s_id.model, sizeof(s_id.model), v, vl);
  if (getKey(s, n, kDes, 2, v, vl)) copyField(s_id.des, sizeof(s_id.des), v, vl);
  if (getKey(s, n, kCmd, 3, v, vl)) {
    copyField(s_id.commands, sizeof(s_id.commands), v, vl);
    buildPdl(v, vl, true);
  } else {
    buildPdl(nullptr, 0, false);
  }
  if (getKey(s, n, kSn, 3, v, vl)) copyField(s_id.serial, sizeof(s_id.serial), v, vl);

  publishCommon();
  s_ready = true;
  FWLOGI("[id] 自动识别：%s / %s / CMD=%s\n",
                s_id.make, s_id.model, s_id.commands);
  FWLOGD("[id] PDL=%s\n", s_id.pdl);
  if (s_id.des[0]) FWLOGD("[id] DES=%s\n", s_id.des);
  FWLOGI("[id] 发布名称：%s / %s\n", s_id.name, s_id.makeModel);
  FWLOGI("[id] 双面=%s（%s） URF=%d PJL=%d\n",
                s_id.duplex ? "是" : "否", s_id.dupEvidence,
                (int)s_id.urf, (int)s_id.pjl);
}

void printerIdSetFallback(uint16_t vid, uint16_t pid) {
  strlcpy(s_id.make, "USB", sizeof(s_id.make));
  s_id.des[0] = '\0';
  snprintf(s_id.model, sizeof(s_id.model), "Printer %04X:%04X",
           (unsigned)vid, (unsigned)pid);
  snprintf(s_id.deviceId, sizeof(s_id.deviceId),
           "MFG:USB;MDL:Printer %04X:%04X;CLS:PRINTER;", (unsigned)vid, (unsigned)pid);
  buildPdl(nullptr, 0, false);
  publishCommon();
  s_ready = true;
  FWLOGI("[id] Device ID 不可用，回退 VID:PID %04X:%04X\n",
                (unsigned)vid, (unsigned)pid);
}

bool printerIdReady() { return s_ready; }

const char *printerIdMake()        { return s_id.make; }
const char *printerIdModel()       { return s_id.pubModel; }  // 发布型号：DES 优先
const char *printerIdMdl()         { return s_id.model; }     // 1284 MDL 原文（可能是系列名）
const char *printerIdDescription() { return s_id.des; }
const char *printerIdName()        { return s_id.name; }
const char *printerIdInfo()        { return s_id.info; }
const char *printerIdMakeModel()   { return s_id.makeModel; }
const char *printerIdDeviceId()    { return s_id.deviceId; }
const char *printerIdCommands()    { return s_id.commands; }
const char *printerIdSerial()      { return s_id.serial; }
const char *printerIdPdl()         { return s_id.pdl; }
const char *printerIdDefaultFormat(){ return s_id.defaultFmt; }
bool printerIdSupportsUrf()        { return s_id.urf; }
bool printerIdSupportsColor()      { return s_id.color; }
bool printerIdGuessed()            { return s_id.guessed; }
bool printerIdSupportsDuplex()     { return s_id.duplex; }
const char *printerIdDuplexEvidence() { return s_id.dupEvidence; }
bool printerIdSupportsPjl()        { return s_id.pjl; }
