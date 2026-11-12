#pragma once
#include <stdint.h>
#include <stddef.h>

// 作业数据源：IPP 631 通过它把「文档字节」喂进同一个 spool。
// 返回 >0 = 实际读到的字节数；0 = EOF；-1 = 暂时无数据(EAGAIN，稍后重试)；-2 = 致命错误。
// 注意：readFn 由打印任务调用，绝不能长时间阻塞。
typedef int (*JobReadFn)(void *ctx, uint8_t *buf, size_t max);
// 文档字节全部收完时立即回调一次（不等 USB 排空）。
// IPP 用它先回 successful-ok，避免大 raster 作业让客户端 HTTP 超时后重发。
typedef void (*JobEofFn)(void *ctx);

struct NetPrintStatus {
  bool     listening;      // 任一条通路已监听
  bool     ippListening;   // 631
  bool     rawListening;   // 9100
  bool     busy;
  uint32_t jobsOk;
  uint32_t jobsFailed;
  uint32_t jobsRaw;        // 其中走 9100 的作业数
  uint32_t jobsIpp;        // 其中走 631 的作业数
  uint64_t bytesTotal;
  uint64_t bytesReceived;   // 生产者累计收包（用于验证边收边送）
  uint64_t bytesSent;       // 消费者累计送入 USB
  uint32_t jobBytes;
  size_t   spoolCapacity;
  size_t   spoolUsed;
  size_t   spoolPeak;
  uint32_t backpressure;
  bool     spoolPaused;
  bool     spoolForced;     // 低 PSRAM 强制停收
  bool     spoolPsram;
  size_t   psramTotal;
  size_t   psramFree;
  size_t   psramLargest;
  size_t   psramReserve;    // 开机实测留给 Wi-Fi/LwIP 的空闲 PSRAM
  char     message[160];
  // 最近一次作业（Web 与 IPP 回执共用，避免两处各记一套）
  char     lastTag[12];
  char     lastSrc[16];    // 最近一次作业的来源 IP
  uint32_t lastBytes;
  uint32_t lastMs;
  bool     lastOk;
  bool     lastAborted;
  char     lastWhy[40];
  uint32_t lastAtSec;
};

// 最近一次作业的结果（Web 与 IPP 回执共用）
struct LastJobInfo {
  char     tag[12];        // "RAW9100" / "IPP"
  char     src[16];        // 作业来源 IP（局域网里谁打的，一眼可见）
  uint32_t bytes;
  uint32_t ms;             // 本次作业耗时
  bool     ok;
  bool     aborted;        // 失败/被中止（USB 出错或客户端断开）
  char     why[40];
  uint32_t atSec;          // 结束时刻（millis/1000）
};

void           netPrintBegin();
NetPrintStatus netPrintStatus();
LastJobInfo    netPrintLastJob();
bool           netPrintBusy();
// 记录当前作业来源 IP（accept 时取，RunJob 前设置）——乱码页归责的第一手证据
void           netPrintSetJobSource(const char *ip);
// 中止当前作业（不需要断电重启）：作业泵下一轮走 abort 并复位 busy。
void           netPrintAbort();
void           netPrintSetIppListening(bool on);
void           netPrintSetRawListening(bool on);

// 运行一次完整的打印作业：readFn 提供文档字节，内部负责 spool 背压、USB 搬运、
// 作业级超时、失败收尾与统计。成功返回 true。不负责关闭 socket。
bool           netPrintRunJob(JobReadFn readFn, void *ctx, const char *tag,
                              JobEofFn onEof = nullptr, void *eofCtx = nullptr);
