#pragma once
#include <stdint.h>
#include <stddef.h>

struct UsbPrinterStatus {
  bool     connected;
  bool     fault;
  uint16_t vid;
  uint16_t pid;
  uint8_t  iface;
  uint8_t  protocol;      // 1 = 单向, 2 = 双向
  uint8_t  outEp;
  uint8_t  inEp;
  // USB Printer Class GET_PORT_STATUS 轮询结果（每 2s 刷新）
  bool     paperEmpty;    // 缺纸 → 引擎停，bulk OUT 会一直 NAK
  bool     selected;      // 打印机被选中（缺纸/报错时为 false）
  uint8_t  portStatus;    // 原始字节，0xFF = 还没读到
  char     message[160];
  uint64_t bytesTotal;
  uint32_t usbErrors;
};

void             usbPrinterBegin();
UsbPrinterStatus usbPrinterStatus();
bool             usbPrinterReady();
// 缺纸时引擎会停，bulk OUT 永远 NAK —— 提前判断，别让客户端干等超时
bool             usbPrinterPaperEmpty();

// ---- 双缓冲乒乓流水线 ------------------------------------------------------
// submit 只把数据拷进 DMA 缓冲并排队，立刻返回；wait 等「最早提交的那块」完成。
// 同一时刻最多 USB_XFER_SLOTS 块在飞，USB 总线不会在「提交→等待」之间空转。
// 约束：调用方在 wait 返回前，绝不能覆盖/回收已 submit 的那段内存。
uint8_t          usbPrinterPending();                 // 在飞块数
uint8_t          usbPrinterRoom();                    // 还能排几块
bool             usbPrinterSubmit(const uint8_t *data, size_t len);
// 返回实际送入 USB 的字节数；*submitted 返回该块的提交长度（短包时两者不等，
// 调用方只提交 actual，剩余字节下次重发）。0 = 失败。
size_t           usbPrinterWait(size_t *submitted = nullptr);
void             usbPrinterFlush();                   // 作业收尾：等所有在飞块落地
// 短包导致后续在飞块位置错位时用：丢弃在飞块并清端点，作业按失败处理。
void             usbPrinterAbort();

// 单块同步写（内部 = submit + wait），保留给非流水线调用点
size_t           usbPrinterWrite(const uint8_t *data, size_t len);
