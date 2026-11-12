#include "raw_print.h"
#include "config.h"
#include "net_print.h"
#include "sysmon.h"
#include "usb_printer.h"

#include <Arduino.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// 9100 RAW / PDL-Datastream
//
// 最省资源的一条通路：没有 HTTP、没有 IPP 属性、没有 chunked 解码，收到的
// 字节直接进环形缓冲，由搬运任务送 USB。
//
// 并发控制：accept 与「跑作业」分成两个任务 + 深度 1 的队列。第一个作业在跑
// 时，第二个连接**立刻被关闭**，而不是堆在 listen backlog 里占内存。RAW 没有
// 协议层可以回错误码，关闭连接就是让客户端（打印后台）稍后重试的信号。
// ---------------------------------------------------------------------------
namespace {

int s_listener = -1;

struct RawCtx {
  int fd;
};

struct RawJob {
  int  fd;
  char src[16];           // 来源 IP：乱码页归责的第一手证据
};

RawCtx        s_jobCtx;        // 队列深度 1，同一时刻只有一份作业用它
QueueHandle_t s_jobQ = nullptr;

// 作业泵的数据源：一轮只读一次，读不到就返回 -1 让泵让出 CPU。
// 背压时泵根本不会调用这里 —— 停止 recv 让 TCP 窗口自己收缩，不丢数据。
int rawRead(void *p, uint8_t *buf, size_t max) {
  RawCtx *c = static_cast<RawCtx *>(p);
  const int n = recv(c->fd, buf, max, 0);
  if (n > 0) return n;
  if (n == 0) return 0;                 // 对端关闭：作业正常结束
  if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) return -1;
  return -2;
}

bool ensureListener() {
  if (s_listener >= 0) return true;
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return false;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(PRINT_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) < 0 ||
      listen(fd, 2) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
    FWLOGE("[raw] listen %d failed (errno=%d), retry\n", PRINT_PORT, errno);
    close(fd);
    return false;
  }
  s_listener = fd;
  netPrintSetRawListening(true);
  FWLOGI("[raw] RAW 打印服务已监听 TCP %d（close = 作业结束）\n", PRINT_PORT);
  return true;
}

// 服务探测/端口扫描常会连上却不发数据。先等首字节，等不到就直接关，
// 绝不让一个空连接把作业槽占满。
bool waitFirstByte(int fd, uint32_t ms) {
  const uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    struct timeval tv = {0, 100000};
    const int r = select(fd + 1, &rf, nullptr, nullptr, &tv);
    if (r > 0) return true;
    if (r < 0 && errno != EINTR) return false;
    sysMonFeed();
  }
  return false;
}

void rawJobTask(void *) {
  sysMonTaskRegister();
  for (;;) {
    RawJob job;
    if (xQueueReceive(s_jobQ, &job, pdMS_TO_TICKS(500)) != pdTRUE) {
      sysMonFeed();
      continue;
    }
    s_jobCtx.fd = job.fd;
    netPrintSetJobSource(job.src);   // 归责：这一页是谁打的
    if (!waitFirstByte(job.fd, 10000)) {
      FWLOGD_LN("[raw] 连接 10s 无数据，按探测处理并关闭");
      close(job.fd);
      sysMonFeed();
      continue;
    }
    if (!usbPrinterReady()) {
      FWLOGE_LN("[raw] 打印机未就绪，拒绝本次作业");
      close(job.fd);
      sysMonFeed();
      continue;
    }
    if (usbPrinterPaperEmpty()) {
      // 缺纸时不能收：收进来也只能卡到超时。RAW 没有协议层可回错误码，
      // 关连接让打印后台自己重试。
      FWLOGE_LN("[raw] 打印机缺纸，拒绝本次作业");
      close(job.fd);
      sysMonFeed();
      continue;
    }
    const bool ok = netPrintRunJob(rawRead, &s_jobCtx, "RAW9100");
    close(job.fd);
    FWLOGD("[raw] 作业结束：%s\n", ok ? "ok" : "aborted");
    sysMonFeed();
  }
}

void rawTask(void *) {
  sysMonTaskRegister();
  for (;;) {
    sysMonFeed();
    if (!ensureListener()) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s_listener, &rfds);
    struct timeval tv = {0, 200000};
    const int r = select(s_listener + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0) continue;

    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    const int fd = accept(s_listener, reinterpret_cast<struct sockaddr *>(&sa), &sl);
    if (fd < 0) continue;

    const uint8_t *a = reinterpret_cast<const uint8_t *>(&sa.sin_addr.s_addr);
    char src[16];
    snprintf(src, sizeof(src), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);

    // 单作业并发：忙就立刻关，不排队。来源 IP 一起记录 —— 谁在往 9100 塞
    // 作业（甚至是你自己都没意识到的手机 App），这里一目了然。
    if (netPrintBusy() || uxQueueSpacesAvailable(s_jobQ) < 1) {
      close(fd);
      FWLOGD("[raw] 拒绝 %s 的连接（已有作业在打印，客户端会自动重试）\n", src);
      continue;
    }
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int rcvbuf = 32768;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    fcntl(fd, F_SETFL, O_NONBLOCK);

    FWLOGD("[raw] 作业来源 %s:%u\n", src, (unsigned)ntohs(sa.sin_port));
    RawJob job = {fd, {0}};
    strlcpy(job.src, src, sizeof(job.src));
    if (xQueueSend(s_jobQ, &job, 0) != pdTRUE) {
      close(fd);
      continue;
    }
  }
}

}  // namespace

void rawPrintBegin() {
  s_jobQ = xQueueCreate(1, sizeof(RawJob));
  if (!s_jobQ) {
    sysMonRequestRestart("raw queue failed");
    return;
  }
  // 网络侧固定在 CORE_NET，与 Wi-Fi/LwIP 同核；绝不占用 USB 所在的核 1。
  if (xTaskCreatePinnedToCore(rawJobTask, "raw-job", 6144, nullptr, 3, nullptr,
                              CORE_NET) != pdPASS ||
      xTaskCreatePinnedToCore(rawTask, "raw9100", 6144, nullptr, 3, nullptr,
                              CORE_NET) != pdPASS) {
    sysMonRequestRestart("raw task failed");
  }
}
