#pragma once
// ============================================================================
// 日志分级：同一份源码产出 debug / release 两套固件
//
//   FW_DEBUG=1（pio run -e debug）   → FWLOGD 照常输出，串口可见逐请求/逐作业细节
//   FW_DEBUG=0（pio run -e release） → FWLOGD 编译为空，参数不求值、不占 Flash
//
//   FWLOGI / FWLOGE 两版都保留：正式版也需要最基本的可运维性
//   （启动版本号、Wi-Fi/USB 设备事件、作业拒绝与失败原因、重启原因）。
//
// 划分原则：
//   FWLOGD = 观测点。逐请求（IPP op）、逐作业（字节数/耗时）、高频轮询
//            （PORT_STATUS、USB 心跳）、环形缓冲指针、内存快照、自检。
//   FWLOGI = 状态事件。启动横幅、服务监听、Wi-Fi/USB 连断、配置变更、统计命令。
//   FWLOGE = 异常。初始化失败、缓冲溢出、传输失败、作业被拒绝、重启原因。
// ============================================================================
#include <Arduino.h>

#ifndef FW_DEBUG
#define FW_DEBUG 0
#endif

#if FW_DEBUG
  #define FWLOGD(...)    Serial.printf(__VA_ARGS__)
  #define FWLOGD_LN(s)   Serial.println(s)
#else
  // 正式版：用 if(false) 而不是 ((void)0)。
  // 参数仍然参与编译期检查、变量不会报 unused，而 GCC 会把整个不可达分支
  // （连同字符串常量）优化掉，运行时零开销、Flash 也不留日志字符串。
  #define FWLOGD(...)    do { if (false) Serial.printf(__VA_ARGS__); } while (0)
  #define FWLOGD_LN(s)   do { if (false) Serial.println(s); } while (0)
#endif

#define FWLOGI(...)      Serial.printf(__VA_ARGS__)
#define FWLOGI_LN(s)     Serial.println(s)
#define FWLOGE(...)      Serial.printf(__VA_ARGS__)
#define FWLOGE_LN(s)     Serial.println(s)
