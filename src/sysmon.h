#pragma once
#include <stdint.h>

// 硬件任务看门狗 (TWDT) + 健康监控 + 受控重启
void        sysMonInit();                       // setup() 最开始调用
void        sysMonBegin();                      // 创建监控任务
void        sysMonTaskRegister();               // 每个自建任务开头调用
void        sysMonFeed();                       // 每个任务循环里调用
void        sysMonRequestRestart(const char *reason);
uint32_t    sysMonBootCount();
uint32_t    sysMonAllocFails();                 // 本次开机以来的分配失败次数
const char *sysMonRestartReason();
const char *sysMonResetReason();
// 最近一次「异常复位」的原因（看门狗/panic），正常重启不会被它覆盖。
// 用于回答「这台机器到底为什么自己重启过」，空字符串 = 从没异常复位。
const char *sysMonLastFault();
