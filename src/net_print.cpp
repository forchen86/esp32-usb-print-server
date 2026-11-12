#include "net_print.h"
#include "config.h"
#include "sysmon.h"
#include "usb_printer.h"
#include "spool.h"

#include <Arduino.h>

namespace {
portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
NetPrintStatus s_st = {};
volatile bool  s_busy = false;
TaskHandle_t   s_writerTask = nullptr;   // 空闲时用它做事件唤醒，避免 1ms 空转

// 作业控制：producer = 调用 netPrintRunJob 的任务，consumer = spoolWriterTask。
// 所有跨任务标志一律通过下面两个辅助函数读写，保证双核下的互斥与可见性。
volatile bool  s_jobActive  = false;
volatile bool  s_jobEof     = false;   // 客户端数据已收完
volatile bool  s_jobDrained = false;   // spool 已全部送进 USB
volatile bool  s_jobFailed  = false;
volatile bool  s_resetReq   = false;   // 作业边界：producer 请求 consumer 清 spool
volatile bool  s_resetDone  = false;
volatile bool  s_abortDone  = false;   // 本轮作业是否已经做过 USB abort（防重复清端点）
unsigned       s_submitStreak = 0;     // USB 连续提交失败计数（只在 writer 里用）
size_t         s_inflight = 0;         // 已提交给 USB、尚未 commit 的字节数（只在 writer 里用）
volatile bool  s_abortReq = false;     // Web 侧的中止请求
char           s_curSrc[16] = "";      // 当前作业来源 IP（accept 时由通路设置）

bool flag(volatile bool &f) {
  portENTER_CRITICAL(&s_mux);
  const bool v = f;
  portEXIT_CRITICAL(&s_mux);
  return v;
}
void setFlag(volatile bool &f, bool v) {
  portENTER_CRITICAL(&s_mux);
  f = v;
  portEXIT_CRITICAL(&s_mux);
}

void setMsg(const char *s) {
  portENTER_CRITICAL(&s_mux);
  strlcpy(s_st.message, s, sizeof(s_st.message));
  portEXIT_CRITICAL(&s_mux);
  FWLOGD("[print] %s\n", s);
}

// 唤醒消费者：只在「空闲 → 有活」和作业边界调用，不放在每次 4KB 收包路径上。
void wakeWriter() {
  if (s_writerTask) xTaskNotifyGive(s_writerTask);
}

// ---- 内容完整性校验（Adler-32）---------------------------------------------
// 生产者对「从 socket 收到的字节」算一份，消费者对「送进 USB 的字节」算一份。
// 作业结束时打印两份对比：数量校验抓不住的「内容损坏」（错位/丢失/重复），
// 这里一定抓得住 —— 乱码白纸这类症状必须先排除传输层。
struct Adler32 {
  uint32_t a = 1, b = 0;
  void update(const uint8_t *p, size_t n) {
    while (n) {
      size_t chunk = n > 5550 ? 5550 : n;   // 5550×255 < 2^32，一段一取模
      n -= chunk;
      while (chunk--) { a += *p++; b += a; }
      a %= 65521; b %= 65521;
    }
  }
  uint32_t value() const { return (b << 16) | a; }
};
Adler32 s_crcIn, s_crcOut;
uint32_t s_crcInBytes = 0, s_crcOutBytes = 0;

// spool 只能由消费者在「不在 usbPrinterWrite 内」的安全点清空。
// 生产者通过请求/应答握手等待，彻底消除 reset 与 in-flight 搬运重叠。
void requestSpoolReset() {
  portENTER_CRITICAL(&s_mux);
  s_resetDone = false;
  s_resetReq = true;
  portEXIT_CRITICAL(&s_mux);
  wakeWriter();                      // 消费者可能正在事件等待里睡着

  const uint32_t t0 = millis();
  for (;;) {
    if (flag(s_resetDone)) return;
    sysMonFeed();
    if (millis() - t0 > USB_XFER_TIMEOUT_MS + 5000UL) {
      sysMonRequestRestart("spool reset timeout");
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// 消费者：把 spool 里的字节按 4KiB 搬进 USB，完全不受网络抖动影响
void spoolWriterTask(void *) {
  sysMonTaskRegister();
  for (;;) {
    sysMonFeed();

    if (flag(s_resetReq)) {          // 独占安全点：此处一定不在 USB 搬运中
      usbPrinterFlush();             // 先让在飞块落地，DMA 不再指向环形缓冲
      spoolReset();
      s_inflight = 0;
      portENTER_CRITICAL(&s_mux);
      s_resetReq = false;
      s_resetDone = true;
      portEXIT_CRITICAL(&s_mux);
      continue;
    }
    if (!flag(s_jobActive)) {
      // 没有作业时不再 1ms 空转：睡到被唤醒为止，最长 100ms 兜一次保险。
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
      continue;
    }
    if (flag(s_jobFailed)) {
      // 中止：halt + flush + clear 端点。只 flush 会留下设备侧缓冲里的半页，
      // 下一份作业开头就串页；清端点才是真正的 abort。
      if (!s_abortDone) {
        s_abortDone = true;
        s_inflight = 0;
        usbPrinterAbort();
      }
      setFlag(s_jobDrained, true);   // 只报告停止，清理由 producer 请求
      vTaskDelay(pdMS_TO_TICKS(1));  // 必须让出 CPU：producer 还没清标志前不能热转
      continue;
    }
    if (!usbPrinterReady()) {
      setFlag(s_jobFailed, true);
      continue;
    }

    // 先把管道填满：最多 2 块 16KB 在飞，USB 总线不再「提交→等待」空转。
    // 注意两点：
    //   1) 只 acquire 不 commit —— DMA 还在读这块内存；
    //   2) 第二块必须用 s_inflight 跳过第一块 —— 读指针要等 wait 完才前进，
    //      直接再 acquire 会拿到同一段（曾导致同一块提交两次、s_r 跑到 s_w
    //      前面、used 无符号下溢到 4GB 级）。
    bool submitFailed = false;
    while (usbPrinterRoom() > 0) {
      uint8_t *p = nullptr;
      const size_t n = spoolReadAcquireFrom(s_inflight, USB_XFER_SIZE, &p);
      if (!n) break;
      if (!usbPrinterSubmit(p, n)) { submitFailed = true; break; }
      s_crcOut.update(p, n);                 // 内容校验：送出去什么算什么
      s_crcOutBytes += (uint32_t)n;
      s_inflight += n;
    }
    if (submitFailed) {
      // 提交失败但设备「看起来就绪」：以前这里会每毫秒重试一次，表现为
      // 「数据全在 spool 里、USB 一个字节都不动」，直到 20 分钟作业超时。
      // 连续失败 ~200ms 就判作业失败，走 abort + 让客户端重发。
      if (++s_submitStreak >= 200) {
        s_submitStreak = 0;
        setMsg("USB 连续提交失败，作业中止");
        setFlag(s_jobFailed, true);
      } else {
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      continue;
    }
    s_submitStreak = 0;

    if (usbPrinterPending() == 0) {
      if (spoolUsed() == 0 && flag(s_jobEof)) setFlag(s_jobDrained, true);
      else vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    // 回收最早提交的那块，并且只提交「真正送进 USB」的长度
    size_t submitted = 0;
    const size_t done = usbPrinterWait(&submitted);
    if (done == 0) {
      setFlag(s_jobFailed, true);
      continue;
    }
    if (done < submitted) {
      // 短包：第二块在环里的起点与设备实际收到的字节流已错位，绝不能继续喂。
      // 丢弃在飞块、清端点，整份作业判失败，让客户端重发。
      usbPrinterAbort();
      s_inflight = 0;
      setFlag(s_jobFailed, true);
      continue;
    }
    spoolReadCommit(done);
    s_inflight = (s_inflight >= submitted) ? (s_inflight - submitted) : 0;
    portENTER_CRITICAL(&s_mux);
    s_st.bytesSent += done;
    s_st.bytesTotal += done;   // 对外「累计写入 USB」口径，与 bytesSent 同源
    portEXIT_CRITICAL(&s_mux);
  }
}

}  // namespace

// 通用作业泵：readFn 只负责提供文档字节，其余全部在这里。
bool netPrintRunJob(JobReadFn readFn, void *ctx, const char *tag,
                    JobEofFn onEof, void *eofCtx) {
  // 目前只有 IPP 任务调用，但后续若增加通路，test-and-set 仍必须是原子的
  portENTER_CRITICAL(&s_mux);
  if (s_busy) {
    portEXIT_CRITICAL(&s_mux);
    setMsg("已有作业在打印，拒绝并发请求");
    return false;
  }
  s_busy = true;
  portEXIT_CRITICAL(&s_mux);

  if (!usbPrinterReady()) {
    setFlag(s_busy, false);
    setMsg("打印机未就绪，已拒绝打印作业");
    return false;
  }

  requestSpoolReset();               // 作业开始：清 spool
  s_crcIn = Adler32{};  s_crcOut = Adler32{};
  s_crcInBytes = s_crcOutBytes = 0;
  s_abortDone = false;
  setFlag(s_jobEof, false);
  setFlag(s_jobDrained, false);
  setFlag(s_jobFailed, false);
  setFlag(s_jobActive, true);
  wakeWriter();                      // 让消费者立刻从事件等待里起来（省掉 100ms 空窗）

  const uint32_t start = millis();
  uint32_t last = start, total = 0;
  uint32_t lastDrain = start;        // 最近一次「USB 真的在动」的时刻
  uint64_t sentSeen = 0;
  bool ok = true;
  const char *why = "";
  bool eofNotified = false;
  bool suppressEof = false;    // 门卫拒绝时置位：绝不回「接收成功」
  s_abortReq = false;

  // 文档数据一旦收齐就立刻通知传输层回「成功」，此时 USB 可能还在慢慢吐数据。
  // 对 IPP 尤其关键：URF/PWG raster 动辄几十 MB，USB Full Speed 要传几十秒，
  // 若等排空才回响应，客户端 HTTP 超时会重发整份作业 → 重复打印。
  //
  // 但「一个字节都没收就失败」绝不能回成功：那样客户端会认为已打印，
  // 而纸上一片空白。这种情况保持 replied=false，由上层回错误码（重发是安全的）。
  auto markEof = [&]() {
    setFlag(s_jobEof, true);
    if (!eofNotified && !suppressEof && (ok || total > 0)) {
      eofNotified = true;
      if (onEof) onEof(eofCtx);
    }
  };

  // ---- 打印语言签名门卫 ------------------------------------------------------
  // 前 PDL_PROBE_BYTES 字节必须能认出一种打印语言。认不出 = 别的设备发来的
  // 垃圾数据，拒绝、不上 USB。探针阶段直接从数据源读进本地缓冲（不进 ring，
  // 避免「未提交数据被二次 acquire 覆盖」），认出后一次性写入 ring 再恢复
  // 零拷贝直写。suppressEof：被门卫拒绝的作业绝不回「接收成功」，
  // 否则客户端会以为打完了（IPP 场景尤其重要）。
  uint8_t probeBuf[PDL_PROBE_BYTES];
  size_t  probeLen = 0;
  int     sigDecision = 0;          // 0 = 未决, 1 = 认出, -1 = 拒绝
  auto sigCheck = [&]() -> int {
    const uint8_t *d = probeBuf;
    const size_t n = probeLen;
    if (n >= 1 && d[0] == 0x1b) return 1;                       // UEL/PJL/PCL5/ESC-POS/HP-GL
    if (n >= 2 && d[0] == ')' && d[1] == '}') return 1;         // PCLXL 裸流
    if (n >= 2 && d[0] == '%' && d[1] == '!') return 1;         // PostScript
    if (n >= 1 && d[0] == '^') return 1;                        // ZPL / TSPL
    if (n >= 4 && !memcmp(d, "RaS2", 4)) return 1;              // PWG raster
    if (n >= 7 && !memcmp(d, "UNIRAST", 7)) return 1;           // URF raster
    if (n >= 5 && !memcmp(d, "%PDF-", 5)) return 1;             // PDF / PCLm
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) return 1;       // JPEG
    if (n >= 4 && !memcmp(d, "\x89PNG", 4)) return 1;           // PNG
    return (n >= PDL_PROBE_BYTES) ? -1 : 0;                     // 还有希望，继续探
  };

  setFlag(s_busy, true);
  portENTER_CRITICAL(&s_mux);
  s_st.busy = true;
  s_st.jobBytes = 0;
  portEXIT_CRITICAL(&s_mux);
  setMsg("正在接收打印作业…");

  unsigned burst = 0;
  uint32_t stallStart = 0;      // 背压连续开始时刻（0 = 当前不在背压）
  for (;;) {
    sysMonFeed();

    if (flag(s_jobFailed)) {
      ok = false;
      // 缺纸时 bulk OUT 一直 NAK，超时后被判失败 —— 报出真实原因，
      // 别让用户以为是 USB 线或打印机坏了。
      why = usbPrinterPaperEmpty() ? "打印机缺纸" : "USB 写入失败 / 打印机断开";
      break;
    }
    if (flag(s_jobDrained)) break;

    const bool canRecv = spoolCanReceive();   // 90% 停收 / 70% 复收（滞回）
    // 我们主动停收的时间不能算成客户端空闲，否则长时间背压后恢复会误判断线。
    if (!canRecv) {
      last = millis();
      // 但背压长期不恢复就是消费端（USB）卡住了：不判失败的话，这个作业会
      // 一直挂到 MAX_JOB_MS(20min)，期间 IPP 631 完全不可用。这里 120s 兜底。
      if (!stallStart) {
        stallStart = millis();
      } else if (millis() - stallStart >= BACKPRESSURE_STALL_MS) {
        ok = false;
        why = "背压超时（USB 发送端卡住）";
        break;
      }
    } else {
      stallStart = 0;
    }

    bool gotData = false;
    if (!flag(s_jobEof) && canRecv) {
      if (PDL_SIGNATURE_GATE && sigDecision == 0) {
        // 探针阶段：先攒够签名再放行
        const int n = readFn(ctx, probeBuf + probeLen, PDL_PROBE_BYTES - probeLen);
        if (n > 0) {
          probeLen += (size_t)n;
          total += (uint32_t)n;
          last = millis();
          gotData = true;
          portENTER_CRITICAL(&s_mux);
          s_st.jobBytes = total;
          s_st.bytesReceived += (uint32_t)n;
          portEXIT_CRITICAL(&s_mux);
          sigDecision = sigCheck();
        } else if (n == 0) {
          if (probeLen == 0) {
            sigDecision = 1;              // 空作业：走原有 EOF 路径
          } else {
            sigDecision = -1;             // 对端关了仍认不出 → 垃圾数据
          }
        } else if (n == -2) {
          ok = false; why = "连接异常"; break;
        }  // -1 = EAGAIN，稍后重试

        if (sigDecision < 0) {
          ok = false;
          suppressEof = true;
          why = "打印数据无法识别（开头无已知打印语言签名，疑似其它设备误发）";
          FWLOGE("[gate] 拒绝作业 src=%s bytes=%u：前 %u B 无已知打印语言签名\n",
                 s_curSrc, (unsigned)total, (unsigned)probeLen);
          break;
        }
        if (sigDecision > 0 && probeLen) {
          // 认出语言：探针字节一次性补进 ring（可能跨多个块），再恢复零拷贝
          size_t off = 0;
          while (off < probeLen) {
            uint8_t *p = nullptr;
            const size_t room = spoolWriteAcquire(probeLen - off, &p);
            if (!room) { sysMonFeed(); vTaskDelay(pdMS_TO_TICKS(1)); continue; }
            memcpy(p, probeBuf + off, room);
            s_crcIn.update(p, room);
            s_crcInBytes += (uint32_t)room;
            spoolWriteCommit(room);
            off += room;
          }
        }
      } else {
      uint8_t *p = nullptr;
      const size_t room = spoolWriteAcquire(NET_READ_CHUNK, &p);
      if (room > 0) {
        const int n = readFn(ctx, p, room);  // 零拷贝直写 spool，长度永不超过可用空间
        if (n > 0) {
          s_crcIn.update(p, (size_t)n);          // 内容校验：收到什么算什么
          s_crcInBytes += (uint32_t)n;
          spoolWriteCommit((size_t)n);
          total += (uint32_t)n;
          last = millis();
          gotData = true;
          portENTER_CRITICAL(&s_mux);
          s_st.jobBytes = total;
          s_st.bytesReceived += (uint32_t)n;
          portEXIT_CRITICAL(&s_mux);
        } else if (n == 0) {
          markEof();                        // 数据收完：先回成功，spool 继续后台排空
        } else if (n == -2) {
          ok = false; why = "连接异常"; break;
        }  // -1 = EAGAIN，稍后重试
      }
      }

      // 客户端发完数据却不主动关连接时，按「正常完成」收尾。
      // 绝不能判失败：RST 会让打印队列重发，造成整份文档重复打印。
      if (!flag(s_jobEof) && millis() - last >= CLIENT_IDLE_TIMEOUT_MS) {
        if (total == 0) { ok = false; why = "空作业超时"; break; }
        if (PDL_SIGNATURE_GATE && sigDecision == 0) {
          // 探针期内客户端停止发送：签名永远悬而未决。这些字节既不能送 USB
          //（没认出语言），也不能回「接收成功」（客户端会以为打完了）。
          ok = false;
          suppressEof = true;
          why = "数据流不完整（探针期内客户端停止发送）";
          FWLOGE("[gate] 拒绝作业 src=%s bytes=%u：探针期内对端停止发送\n",
                 s_curSrc, (unsigned)total);
          break;
        }
        markEof();
        setMsg("客户端长时间无新数据，按已发送完成收尾（不会重发作业）");
      }
    }

    if (millis() - start >= MAX_JOB_MS) { ok = false; why = "作业超时"; break; }
    if (total >= MAX_JOB_BYTES) { ok = false; why = "作业超出大小上限"; break; }
    if (s_abortReq) { ok = false; why = "用户中止"; break; }

    // 数据已收完，但 USB 一直不动 → 不能傻等 20 分钟把 busy 焊死。
    // 60s 内 bytesSent 没有任何增长就判失败，走 abort 释放服务。
    if (flag(s_jobEof)) {
      uint64_t sent = 0;
      portENTER_CRITICAL(&s_mux);
      sent = s_st.bytesSent;
      portEXIT_CRITICAL(&s_mux);
      if (sent != sentSeen) {
        sentSeen = sent;
        lastDrain = millis();
      } else if (millis() - lastDrain >= 60000) {
        ok = false;
        why = "USB 排空超时（60s 无进展）";
        break;
      }
    }

    // 边收边送：producer 不等 USB 完成，consumer 在另一任务里并行搬运。
    // 连续收包时每 8 轮让出一次 CPU，避免饿死低优先级任务。
    if (gotData) ++burst;
    else burst = 0;
    if (!gotData || burst >= 8) {
      burst = 0;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  if (!ok) {
    setFlag(s_jobFailed, true);
    markEof();                     // 已送出的字节无法回滚，绝不回错误码让客户端重发
    const uint32_t t0 = millis();
    while (!flag(s_jobDrained)) {          // 等消费者退出当前搬运（最长 35s，期间喂狗）
      sysMonFeed();
      if (millis() - t0 > USB_XFER_TIMEOUT_MS + 5000UL) {
        sysMonRequestRestart("job drain timeout");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  } else {
    // 正常收尾：只补 UEL（ESC%-12345X），不补 FF。
    // UEL 让打印机退出当前 PDL / 回到 PJL 作业边界，足以防止下一份作业的第一页
    // 与残留格式状态粘在一起；客户端驱动生成的流末尾本来也多带 UEL，重复无害。
    // 绝不能追加 FF(0x0C)：UEL 之后页缓冲已空，FF 会强制走纸吐出一张空白页
    // —— 这正是「每次打印都多一张白纸」的根因（v1.4.4 修）。
    static const uint8_t kTail[] = {0x1B, '%', '-', '1', '2', '3', '4', '5', 'X'};
    if (JOB_TAIL_UEL) {
      setFlag(s_jobDrained, false);            // 还要再送这几字节，先清掉完成标志
      size_t wrote = 0;
      const uint32_t t0 = millis();
      while (wrote < sizeof(kTail) && millis() - t0 < 5000) {
        uint8_t *p = nullptr;
        const size_t room = spoolWriteAcquire(sizeof(kTail) - wrote, &p);
        if (room) {
          memcpy(p, kTail + wrote, room);
          s_crcIn.update(p, room);           // 尾部也算进「收到」一侧，口径一致
          s_crcInBytes += (uint32_t)room;
          spoolWriteCommit(room);
          wrote += room;
        } else {
          sysMonFeed();
          vTaskDelay(pdMS_TO_TICKS(1));
        }
      }
      // 等它真的送完：不能用 s_jobDrained 判断 —— 消费者可能在「写入之前」
      // 就已经把 drained 置位（它看到的是空缓冲），那样会提前收尾、UEL 丢失。
      // 直接盯「环形缓冲空 + 没有在飞块」这个物理条件才可靠。
      const uint32_t t1 = millis();
      while ((spoolUsed() > 0 || usbPrinterPending() > 0) &&
             millis() - t1 < 10000) {
        sysMonFeed();
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }
  }
  setFlag(s_jobActive, false);
  requestSpoolReset();               // 作业结束：消费者在安全点清空

  // 内容完整性：收到的 vs 送进 USB 的。不一致 = 传输层改了字节（乱码白纸的
  // 直接证据）；一致 = 固件忠实透传，问题在驱动流或打印机侧。
  FWLOGI("[crc] %s in=%08lx(%u B) out=%08lx(%u B) %s\n",
         tag, (unsigned long)s_crcIn.value(), (unsigned)s_crcInBytes,
         (unsigned long)s_crcOut.value(), (unsigned)s_crcOutBytes,
         (s_crcIn.value() == s_crcOut.value() && s_crcInBytes == s_crcOutBytes)
             ? "MATCH" : "MISMATCH !!");

  setFlag(s_busy, false);
  portENTER_CRITICAL(&s_mux);
  s_st.busy = false;
  // 这里不累加 bytesTotal：它表示「真正写进 USB 的字节数」，只能由消费者
  // 在 spoolReadCommit 成功时累加。早先在这里加 total（收到的字节）会让
  // 「一个字节都没送出去」也显示 bytes 增长，把故障诊断带偏。
  if (ok) ++s_st.jobsOk;
  else ++s_st.jobsFailed;
  if (!strcmp(tag, "RAW9100")) ++s_st.jobsRaw;
  else ++s_st.jobsIpp;
  strlcpy(s_st.lastTag, tag, sizeof(s_st.lastTag));
  strlcpy(s_st.lastSrc, s_curSrc, sizeof(s_st.lastSrc));
  s_st.lastBytes = total;
  s_st.lastMs = millis() - start;
  s_st.lastOk = ok;
  s_st.lastAborted = !ok;
  strlcpy(s_st.lastWhy, why, sizeof(s_st.lastWhy));
  s_st.lastAtSec = millis() / 1000;
  portEXIT_CRITICAL(&s_mux);

  char m[160], num[24];
  snprintf(num, sizeof(num), "%lu", (unsigned long)total);
  if (ok) snprintf(m, sizeof(m), "%s 作业完成，共 %s 字节（已全部送入 USB）", tag, num);
  else snprintf(m, sizeof(m), "%s 作业%s：%s（已收 %s 字节）", tag,
                "已中止", why, num);
  setMsg(m);
  return ok;
}

void netPrintBegin() {
  setMsg("打印服务启动中…");
  // 搬运任务固定在 CORE_USB：USB 的忙等只会占住核 1，绝不和 Wi-Fi/LwIP 抢核 0
  if (xTaskCreatePinnedToCore(spoolWriterTask, "spool-writer", 6144, nullptr, 4,
                              &s_writerTask, CORE_USB) != pdPASS) {
    sysMonRequestRestart("spool writer task failed");
    return;
  }
}

void netPrintSetIppListening(bool on) {
  portENTER_CRITICAL(&s_mux);
  s_st.ippListening = on;
  s_st.listening = on || s_st.rawListening;
  portEXIT_CRITICAL(&s_mux);
}

void netPrintSetRawListening(bool on) {
  portENTER_CRITICAL(&s_mux);
  s_st.rawListening = on;
  s_st.listening = on || s_st.ippListening;
  portEXIT_CRITICAL(&s_mux);
}

LastJobInfo netPrintLastJob() {
  LastJobInfo j = {};
  portENTER_CRITICAL(&s_mux);
  strlcpy(j.tag, s_st.lastTag, sizeof(j.tag));
  strlcpy(j.src, s_st.lastSrc, sizeof(j.src));
  j.bytes = s_st.lastBytes;
  j.ms = s_st.lastMs;
  j.ok = s_st.lastOk;
  j.aborted = s_st.lastAborted;
  strlcpy(j.why, s_st.lastWhy, sizeof(j.why));
  j.atSec = s_st.lastAtSec;
  portEXIT_CRITICAL(&s_mux);
  return j;
}

NetPrintStatus netPrintStatus() {
  NetPrintStatus copy;
  portENTER_CRITICAL(&s_mux);
  copy = s_st;
  portEXIT_CRITICAL(&s_mux);
  const SpoolStatus sp = spoolStatus();
  copy.spoolCapacity = sp.capacity;
  copy.spoolUsed = sp.used;
  copy.spoolPeak = sp.peak;
  copy.backpressure = sp.backpressureEvents;
  copy.spoolPaused = sp.paused;
  copy.spoolForced = sp.forced;
  copy.spoolPsram = sp.psram;
  copy.psramTotal = sp.psramTotal;
  copy.psramFree = sp.psramFree;
  copy.psramLargest = sp.psramLargest;
  copy.psramReserve = sp.reserveFloor;
  return copy;
}

bool netPrintBusy() { return flag(s_busy); }

void netPrintSetJobSource(const char *ip) {
  portENTER_CRITICAL(&s_mux);
  strlcpy(s_curSrc, ip ? ip : "", sizeof(s_curSrc));
  portEXIT_CRITICAL(&s_mux);
}

// 卡死时的逃生口：不需要断电重启。作业泵会在下一轮循环看到标志，
// 走 abort（halt + flush + clear）并复位 busy。
void netPrintAbort() {
  s_abortReq = true;
  setMsg("收到中止请求，正在中止当前作业…");
}
