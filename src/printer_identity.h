#pragma once
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// 打印机身份自动识别（去机型化的核心）
//
// 数据来源优先级：
//   1) USB Printer Class 的 GET_DEVICE_ID 控制请求 → IEEE 1284 Device ID
//      （MFG / MDL / CMD / DES / SN），Windows 也是靠它认机型的；
//   2) 读不到时回退 USB 设备描述符的 VID:PID。
//
// 所有 IPP / mDNS / SNMP / Web 的机型相关字段都必须从这里取，
// 不允许再 anywhere 写死某个品牌或型号。
// ============================================================================

// 由 USB 层在 GET_DEVICE_ID 完成时调用；buf 指向「2 字节长度 + ASCII」的数据段
void printerIdSetFromDeviceId(const uint8_t *buf, size_t len);
// 读不到 Device ID 时的兜底
void printerIdSetFallback(uint16_t vid, uint16_t pid);
bool printerIdReady();

const char *printerIdMake();           // MFG
const char *printerIdModel();          // 对外发布的型号：DES 优先，回退 MDL
const char *printerIdMdl();            // 1284 MDL 原文（HP 常是系列名）
const char *printerIdDescription();    // DES（销售型号，可能比 MDL 更具体、带双面后缀）
const char *printerIdName();           // 短名：DNS / IPP 安全，截断去空格
const char *printerIdInfo();           // printer-info
const char *printerIdMakeModel();      // printer-make-and-model
const char *printerIdDeviceId();       // 原始/构造的 1284 Device ID
const char *printerIdCommands();       // CMD 原文
const char *printerIdSerial();         // SN
const char *printerIdPdl();            // 逗号分隔的 MIME 列表（按 CMD 推导）
const char *printerIdDefaultFormat();  // pdl 的第一项
bool        printerIdSupportsUrf();
bool        printerIdSupportsColor();
bool        printerIdGuessed();     // true = 没读到 Device ID，能力是推定值
// 自动双面能力检测：1284 Device ID 的 CAP/CMD/DUPLEX 字段 + 型号后缀启发式
bool        printerIdSupportsDuplex();
const char *printerIdDuplexEvidence();  // 判定依据，给 Web 显示
// 是否能接受 PJL 作业控制指令（只有这类机器才能把 IPP sides 翻成 PJL 双面命令）
bool        printerIdSupportsPjl();
