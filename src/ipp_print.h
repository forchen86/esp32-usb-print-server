#pragma once
#include <stdint.h>

// 精简 IPP 1.1 服务端（TCP 631）：只做「取下 IPP 封装，把文档字节交给作业引擎」，
// 不解析打印语言、不渲染、不做队列。文档字节直接进环形缓冲 → USB 通路。
void ippPrintBegin();

// 全固件唯一的打印机状态口径（RFC 8011）：3=idle 4=processing 5=stopped。
// SNMP 代理读的就是这个值，不允许另建一套状态机。
uint8_t ippPrinterState();
