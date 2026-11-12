#pragma once
#include <stddef.h>

// 运行期可调、掉电保存的打印选项。
//
// 双面能力有两层：
//   1) 自动识别（printer_identity 从 1284 Device ID 判断打印机有没有双面单元）；
//   2) 用户策略：auto = 跟随识别结果，on/off = 手动强制覆盖。
// IPP sides、Bonjour TXT、Web 一律读 cfgDuplexEnabled()（有效值），
// 保证三处口径一致。

enum DuplexMode {
  DUPLEX_AUTO = 0,   // 默认：由打印机识别结果决定
  DUPLEX_ON   = 1,
  DUPLEX_OFF  = 2,
};

int         cfgDuplexMode();
void        cfgSetDuplexMode(int mode);
bool        cfgDuplexEnabled();        // 有效值：IPP sides / mDNS duplex 都用它
bool        cfgDuplexDetected();       // 设备侧自动识别结果
const char *cfgDuplexEvidence();       // 识别依据（Web 显示）
void        cfgSetDuplexEnabled(bool on);  // 兼容旧接口 → DUPLEX_ON / DUPLEX_OFF

// IPP 的 sides 属性会被拆封装丢掉。开启后，收到 two-sided 作业时在文档流前
// 注入一段 PJL 作业控制头（SET DUPLEX=ON / SET BINDING=...），让打印机真的
// 双面出纸。这是作业控制指令注入，不是页面格式转换；只对懂 PJL 的机器生效。
bool        cfgPjlDuplexEnabled();
void        cfgSetPjlDuplexEnabled(bool on);

// AirPrint 的 URF 能力串。DM3 = 支持自动双面，DM1 = 仅单面；其余键固定。
void cfgUrfString(bool duplex, char *out, size_t cap);

void cfgBegin();      // 开机从 NVS 读取（必须在 wifiManagerBegin 之前）
void cfgService();    // loop 里调用
