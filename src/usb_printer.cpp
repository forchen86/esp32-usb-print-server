#include "usb_printer.h"
#include "config.h"
#include "printer_identity.h"
#include "sysmon.h"

#include <Arduino.h>
#include "usb/usb_host.h"

namespace {
struct PrinterIface {
  uint8_t number, protocol, outEp, inEp;
};

// 单把锁同时保护 s_st 与全部跨任务标志；任何临界区内都不得调用 setMsg()
portMUX_TYPE            s_mux = portMUX_INITIALIZER_UNLOCKED;
UsbPrinterStatus        s_st = {};
usb_host_client_handle_t s_client = nullptr;
usb_device_handle_t     s_dev = nullptr;

// 打印身份识别（GET_DEVICE_ID）用的 transfer 与状态。
// 放在最前面：openDevice() 在文件上半部分就要重置这些标志。
usb_transfer_t *s_idXfer = nullptr;
volatile bool   s_idInFlight = false;
bool            s_idDone = false;
uint32_t        s_idSubmitAt = 0;
uint8_t         s_idRetries = 0;
uint16_t        s_idVid = 0, s_idPid = 0;

// 16KB 双缓冲乒乓：两个 transfer 轮转提交，一个在飞时另一个已经排好队，
// USB 总线不再「提交 → 等完成 → 再提交」地空转。
struct XferSlot {
  usb_transfer_t *t = nullptr;
  volatile bool   inFlight = false;
  volatile bool   done = false;
  volatile int    status = 0;
  volatile size_t actual = 0;
  size_t          len = 0;      // 本次提交长度（判断短包用）
  uint32_t        crcBefore = 0;  // 提交前缓冲区校验（抓在飞期间被改写的竞态）
};
XferSlot s_slot[USB_XFER_SLOTS];
uint8_t  s_submitIdx = 0;       // 下一个要提交的槽
uint8_t  s_waitIdx = 0;         // 下一个要回收的槽（严格 FIFO）
uint8_t  s_pending = 0;         // 在飞数量

volatile bool   s_inFlight = false;      // 有 transfer 未回收时不能释放设备
volatile bool   s_devGone = false;
volatile bool   s_pendingClose = false;
volatile bool   s_frozen = false;        // 超时回收失败：冻结所有 USB Host 操作等重启
volatile uint8_t s_pendingAddr = 0;
// 正在等 transfer 完成的任务。完成回调里直接 notify 它，把「1ms 轮询」
// 换成事件唤醒。
volatile TaskHandle_t s_xferWaiter = nullptr;

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
  FWLOGI("[usb] %s\n", s);
}

// 只认标准 USB Printer Class (07/01)，协议 1(单向) 或 2(双向)
bool parseConfig(const usb_config_desc_t *cd, PrinterIface &out) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(cd);
  const size_t   total = cd->wTotalLength;
  if (total < 9 || p[1] != 2 /* CONFIGURATION */) return false;

  PrinterIface cur = {};
  uint8_t expected = 0, seen = 0;
  bool scanning = false, bad = false, found = false;

  for (size_t pos = 0; pos + 2 <= total;) {
    const uint8_t len = p[pos], type = p[pos + 1];
    if (len < 2 || pos + len > total) break;

    if (type == 4 /* INTERFACE */ && len >= 9) {
      if (scanning && !bad && cur.outEp && seen == expected && !found) {
        out = cur;
        found = true;
      }
      const uint8_t alt = p[pos + 3];
      scanning = (alt == 0 && p[pos + 5] == 0x07 && p[pos + 6] == 0x01 &&
                  (p[pos + 7] == 1 || p[pos + 7] == 2));
      if (scanning) {
        cur = {};
        cur.number = p[pos + 2];
        cur.protocol = p[pos + 7];
        expected = p[pos + 4];
        seen = 0;
        bad = false;
      }
    } else if (type == 5 /* ENDPOINT */ && len >= 7 && scanning) {
      ++seen;
      const uint8_t addr = p[pos + 2];
      const uint8_t attr = p[pos + 3];
      if ((attr & 0x03) != 0x02 /* BULK */ || (addr & 0x0F) == 0 || (addr & 0x70)) {
        bad = true;
      } else if (addr & 0x80) {
        if (cur.inEp) bad = true;
        cur.inEp = addr;
      } else {
        if (cur.outEp) bad = true;
        cur.outEp = addr;
      }
    }
    pos += len;
  }
  // 只要求存在 Bulk OUT：单向(协议1)与双向(协议2，即使没有 IN 端点)都能透传
  if (scanning && !bad && cur.outEp && seen == expected && !found) {
    out = cur;
    found = true;
  }
  return found;
}

void rejectDevice(usb_device_handle_t dev, const char *why) {
  usb_host_device_close(s_client, dev);
  usb_host_device_free_all();       // 否则被拒设备留在 Host 库设备表里，重插不再上报
  portENTER_CRITICAL(&s_mux);
  s_pendingAddr = 0;               // 允许后续插入重新触发枚举
  portEXIT_CRITICAL(&s_mux);
  setMsg(why);
}

void openDevice(uint8_t addr) {
  usb_device_handle_t dev = nullptr;
  if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) return;

  const usb_device_desc_t  *dd = nullptr;
  const usb_config_desc_t  *cd = nullptr;
  PrinterIface pi = {};
  if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK ||
      usb_host_get_active_config_descriptor(dev, &cd) != ESP_OK ||
      !parseConfig(cd, pi)) {
    rejectDevice(dev, "发现 USB 设备，但不是标准打印机接口");
    return;
  }
  if (usb_host_interface_claim(s_client, dev, pi.number, 0) != ESP_OK) {
    rejectDevice(dev, "无法占用打印机 USB 接口");
    return;
  }

  portENTER_CRITICAL(&s_mux);
  s_dev = dev;
  s_st.connected = true;
  s_st.fault = false;
  s_st.vid = dd->idVendor;
  s_st.pid = dd->idProduct;
  // 新设备插入：重新识别机型
  s_idVid = dd->idVendor;
  s_idPid = dd->idProduct;
  s_idDone = false;
  s_idRetries = 0;
  s_idSubmitAt = 0;
  s_idInFlight = false;
  s_st.iface = pi.number;
  s_st.protocol = pi.protocol;
  s_st.outEp = pi.outEp;
  s_st.inEp = pi.inEp;
  s_devGone = false;
  s_pendingClose = false;
  s_pendingAddr = 0;
  portEXIT_CRITICAL(&s_mux);

  char m[120];
  snprintf(m, sizeof(m), "打印机就绪 VID:%04X PID:%04X 接口%d OUT:0x%02X IN:0x%02X",
           (unsigned)dd->idVendor, (unsigned)dd->idProduct, pi.number, pi.outEp, pi.inEp);
  setMsg(m);
}

void closeDevice() {
  usb_device_handle_t dev;
  uint8_t iface;
  portENTER_CRITICAL(&s_mux);
  // 二次确认：canClose 判断与本次调用之间可能已有新的 transfer 提交
  if (s_inFlight) {
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  dev = s_dev;
  iface = s_st.iface;
  s_dev = nullptr;
  s_st.connected = false;
  s_st.fault = false;
  s_devGone = false;
  s_pendingClose = false;
  s_pendingAddr = 0;
  portEXIT_CRITICAL(&s_mux);

  if (dev) {
    usb_host_interface_release(s_client, dev, iface);
    usb_host_device_close(s_client, dev);
    // 只 close 不 free，设备仍留在 Host 的设备表里，后续 openDevice 会失败，
    // 表现为「打印机休眠再唤醒后永远连不回来」。
    usb_host_device_free_all();
  }
  setMsg("打印机已断开，等待重新枚举");
}

// 主动重新枚举：打印机休眠时 USB 会掉（DEV_GONE），唤醒后 Host 不一定再发
// NEW_DEV 事件。只靠事件等待就永远回不来，所以无设备时定期扫地址表。
void rescanDevices() {
  static uint32_t lastTry = 0;
  if (millis() - lastTry < 2000) return;
  lastTry = millis();

  uint8_t addrs[8] = {0};
  int count = 0;
  // 该 API 只给「当前设备表里的地址列表」，参数是元素个数（不是字节数）
  if (usb_host_device_addr_list_fill((int)(sizeof(addrs) / sizeof(addrs[0])), addrs,
                                     &count) != ESP_OK) return;
  if (count <= 0) return;

  portENTER_CRITICAL(&s_mux);
  if (!s_dev && !s_pendingAddr && !s_pendingClose) s_pendingAddr = addrs[0];
  portEXIT_CRITICAL(&s_mux);
}

// 置 fault 后如果设备句柄还在，说明设备没拔、只是写不动了（休眠/挂起）。
// 强制走一次 close + free，让 rescan 重新打开，而不是一直卡在 fault。
void serviceFaultRecovery() {
  static uint32_t faultSince = 0;
  bool faulted = false;
  portENTER_CRITICAL(&s_mux);
  faulted = s_st.fault && s_dev != nullptr;
  portEXIT_CRITICAL(&s_mux);

  if (!faulted) { faultSince = 0; return; }
  if (!faultSince) { faultSince = millis(); return; }
  if (millis() - faultSince < 5000) return;

  faultSince = 0;
  setMsg("USB 故障 5s 未恢复，重新枚举设备");
  portENTER_CRITICAL(&s_mux);
  s_pendingClose = true;
  portEXIT_CRITICAL(&s_mux);
  usb_host_device_free_all();
}

// 回调只置标志，不做任何 RTOS 调用 —— 不依赖回调所在上下文（任务/ISR 都安全）
// ---- DMA 缓冲「脏数据」校验 -------------------------------------------------
// 提交前对 DMA 缓冲算一份 Adler-32，传输完成后再算一份。正常情况两份必然
// 相同（缓冲只在 CPU 写、DMA 只读）；不一致 = 在飞期间缓冲被别的东西改写了
// （竞态 / 越界写），这正是「偶发乱码」这类难以复现的问题最需要的一手证据。
static uint32_t bufAdler(const uint8_t *p, size_t n) {
  uint32_t a = 1, b = 0;
  while (n) {
    size_t chunk = n > 5550 ? 5550 : n;
    n -= chunk;
    while (chunk--) { a += *p++; b += a; }
    a %= 65521; b %= 65521;
  }
  return (b << 16) | a;
}
static uint32_t s_bufCorrupt = 0;     // 缓冲校验不一致计数（writer 任务内递增）

void xferCallback(usb_transfer_t *t) {
  XferSlot *s = static_cast<XferSlot *>(t->context);
  if (s) {
    s->status = (int)t->status;
    s->actual = t->actual_num_bytes;
    s->done = true;
  }
  // 回调在 usb-client 任务上下文里跑（非中断），可以直接 notify 等待者
  TaskHandle_t w = s_xferWaiter;
  if (w) xTaskNotifyGive(w);
}

void clientEventCallback(const usb_host_client_event_msg_t *msg, void *) {
  portENTER_CRITICAL(&s_mux);
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (!s_dev && !s_pendingAddr) s_pendingAddr = msg->new_dev.address;
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    if (s_dev && msg->dev_gone.dev_hdl == s_dev) {
      s_devGone = true;
      s_pendingClose = true;
      s_pendingAddr = 0;
    }
  }
  portEXIT_CRITICAL(&s_mux);
}

// 打印身份识别（IEEE 1284 Device ID）：枚举成功后异步读一次，读不到回退 VID:PID
void deviceIdCb(usb_transfer_t *t) {
  if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 2) {
    // 控制传输的数据阶段紧跟在 8 字节 setup packet 后面。
    // actual_num_bytes 在不同 IDF 版本里可能含/不含 setup，所以取一个宽松上限，
    // 真正的字符串长度由 1284 的长度字段决定。
    const size_t off = sizeof(usb_setup_packet_t);
    size_t n = (size_t)t->actual_num_bytes;
    if (n > off) n -= off;
    if (n > 1026) n = 1026;
    if (n > 2) {
      printerIdSetFromDeviceId(t->data_buffer + off, n);
      s_idDone = true;
    }
  }
  s_idInFlight = false;
}

void serviceDeviceId() {
  if (s_idDone || !s_idXfer) return;

  if (s_idInFlight) {
    // 控制请求 3s 没回来 → 设备不支持 GET_DEVICE_ID，直接用 VID:PID 兜底
    if (millis() - s_idSubmitAt > 3000) {
      s_idInFlight = false;
      printerIdSetFallback(s_idVid, s_idPid);
      s_idDone = true;
    }
    return;
  }
  if (s_idSubmitAt && millis() - s_idSubmitAt < 800) return;   // 重试间隔

  usb_device_handle_t dev;
  uint8_t iface;
  portENTER_CRITICAL(&s_mux);
  dev = s_dev;
  iface = s_st.iface;
  portEXIT_CRITICAL(&s_mux);
  if (!dev) return;

  usb_setup_packet_t *setup =
      reinterpret_cast<usb_setup_packet_t *>(s_idXfer->data_buffer);
  setup->bmRequestType = 0xA1;    // device-to-host, class, interface
  setup->bRequest = 0x00;         // GET_DEVICE_ID
  setup->wValue = 0;              // configuration index
  setup->wIndex = iface;          // (alt<<8)|iface，本固件 alt=0
  setup->wLength = 1024;
  s_idXfer->device_handle = dev;
  s_idXfer->bEndpointAddress = 0;
  s_idXfer->callback = deviceIdCb;
  s_idXfer->context = nullptr;
  s_idXfer->num_bytes = (int)(sizeof(usb_setup_packet_t) + 1024);
  s_idXfer->flags = 0;

  s_idInFlight = true;
  s_idSubmitAt = millis();
  if (usb_host_transfer_submit_control(s_client, s_idXfer) != ESP_OK) {
    s_idInFlight = false;
    if (++s_idRetries >= 3) {
      printerIdSetFallback(s_idVid, s_idPid);
      s_idDone = true;
    }
  }
}

// ---- 端口状态轮询（缺纸 / 选中）--------------------------------------------
// 每 2s 发一次 GET_PORT_STATUS（控制传输，不占 bulk OUT）。缺纸时引擎停，
// bulk OUT 会一直 NAK —— 有了这个状态就能提前告诉客户端，而不是干等超时。
usb_transfer_t *s_portXfer = nullptr;
volatile bool   s_portInFlight = false;
uint32_t        s_portLast = 0;

void portStatusCb(usb_transfer_t *t) {
  if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes >= 1) {
    const uint8_t st = t->data_buffer[sizeof(usb_setup_packet_t)];
    portENTER_CRITICAL(&s_mux);
    s_st.portStatus = st;
    s_st.paperEmpty = ((st >> 5) & 1) != 0;
    s_st.selected = ((st >> 4) & 1) != 0;
    portEXIT_CRITICAL(&s_mux);
  }
  s_portInFlight = false;
}

void servicePortStatus() {
  if (s_portInFlight || !s_portXfer) return;
  if (millis() - s_portLast < 2000) return;
  s_portLast = millis();

  usb_device_handle_t dev;
  uint8_t iface;
  portENTER_CRITICAL(&s_mux);
  dev = s_dev;
  iface = s_st.iface;
  portEXIT_CRITICAL(&s_mux);
  if (!dev) return;

  usb_setup_packet_t *setup = reinterpret_cast<usb_setup_packet_t *>(s_portXfer->data_buffer);
  setup->bmRequestType = 0xA1;
  setup->bRequest = 0x01;          // GET_PORT_STATUS
  setup->wValue = 0;
  setup->wIndex = iface;
  setup->wLength = 1;
  s_portXfer->device_handle = dev;
  s_portXfer->bEndpointAddress = 0;
  s_portXfer->callback = portStatusCb;
  s_portXfer->context = nullptr;
  s_portXfer->num_bytes = (int)(sizeof(usb_setup_packet_t) + 1);
  s_portXfer->flags = 0;

  s_portInFlight = true;
  if (usb_host_transfer_submit_control(s_client, s_portXfer) != ESP_OK) {
    s_portInFlight = false;
  }
}

// ---- IN 端点探测（诊断用）--------------------------------------------------
// bulk OUT 超时后读一次 IN，用来区分两种情况：
//   · IN 也超时  → 设备整体 USB 停摆（休眠/掉电/线问题）
//   · IN 能完成  → 设备活着，只是 OUT 方向不收数据（引擎未就绪或流控）
volatile bool   s_probeDone = false;
volatile int    s_probeStatus = -1;
volatile size_t s_probeActual = 0;

void probeCb(usb_transfer_t *t) {
  s_probeStatus = (int)t->status;
  s_probeActual = t->actual_num_bytes;
  s_probeDone = true;
  TaskHandle_t w = s_xferWaiter;
  if (w) xTaskNotifyGive(w);
}

void probeInEndpoint() {
  usb_device_handle_t dev;
  uint8_t ep;
  portENTER_CRITICAL(&s_mux);
  dev = s_dev;
  ep = s_st.inEp;
  portEXIT_CRITICAL(&s_mux);
  if (!dev || !ep) {
    FWLOGD_LN("[usb] 无 IN 端点，跳过探测");
    return;
  }

  usb_transfer_t *t = nullptr;
  if (usb_host_transfer_alloc(64, 0, &t) != ESP_OK || !t) return;
  t->device_handle = dev;
  t->bEndpointAddress = ep;
  t->callback = probeCb;
  t->context = nullptr;
  t->num_bytes = 64;
  t->flags = 0;
  s_probeDone = false;
  s_probeStatus = -1;
  s_probeActual = 0;
  s_xferWaiter = xTaskGetCurrentTaskHandle();

  if (usb_host_transfer_submit(t) != ESP_OK) {
    s_xferWaiter = nullptr;
    usb_host_transfer_free(t);
    FWLOGE_LN("[usb] IN 探测提交失败");
    return;
  }
  const uint32_t t0 = millis();
  while (!s_probeDone && millis() - t0 < 2500) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
  }
  s_xferWaiter = nullptr;
  FWLOGD("[usb] IN 探测：%s status=%d actual=%u\n",
                s_probeDone ? "完成" : "超时", (int)s_probeStatus,
                (unsigned)s_probeActual);
  usb_host_transfer_free(t);
}

// USB Printer Class GET_PORT_STATUS (bRequest=0x01)：
// 不经过 bulk OUT 就能读到「缺纸 / 错误 / 是否选中」，正好用来判断
// 「OUT 一直 NAK」到底是打印机引擎停了还是接口坏了。
// 返回字节：bit3=NOT_ERROR, bit4=SELECTED, bit5=PAPER_EMPTY, bit6=NOT_ATTENTION
void probePortStatus() {
  usb_device_handle_t dev;
  uint8_t iface;
  portENTER_CRITICAL(&s_mux);
  dev = s_dev;
  iface = s_st.iface;
  portEXIT_CRITICAL(&s_mux);
  if (!dev) return;

  usb_transfer_t *t = nullptr;
  const size_t dataLen = 1;
  if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + dataLen, 0, &t) != ESP_OK || !t) {
    return;
  }
  usb_setup_packet_t *setup = reinterpret_cast<usb_setup_packet_t *>(t->data_buffer);
  setup->bmRequestType = 0xA1;    // device-to-host, class, interface
  setup->bRequest = 0x01;         // GET_PORT_STATUS
  setup->wValue = 0;
  setup->wIndex = iface;
  setup->wLength = dataLen;

  t->device_handle = dev;
  t->bEndpointAddress = 0;
  t->callback = probeCb;
  t->context = nullptr;
  t->num_bytes = (int)(sizeof(usb_setup_packet_t) + dataLen);
  t->flags = 0;

  s_probeDone = false;
  s_probeStatus = -1;
  s_probeActual = 0;
  s_xferWaiter = xTaskGetCurrentTaskHandle();
  if (usb_host_transfer_submit_control(s_client, t) != ESP_OK) {
    s_xferWaiter = nullptr;
    usb_host_transfer_free(t);
    FWLOGE_LN("[usb] GET_PORT_STATUS 提交失败");
    return;
  }
  const uint32_t t0 = millis();
  while (!s_probeDone && millis() - t0 < 2500) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
  }
  s_xferWaiter = nullptr;

  if (s_probeDone && s_probeStatus == (int)USB_TRANSFER_STATUS_COMPLETED) {
    const uint8_t st = t->data_buffer[sizeof(usb_setup_packet_t)];
    FWLOGD("[usb] PORT_STATUS=0x%02X not_error=%d selected=%d paper_empty=%d\n",
                  st, (st >> 3) & 1, (st >> 4) & 1, (st >> 5) & 1);
  } else {
    FWLOGE("[usb] PORT_STATUS 读取失败 status=%d\n", (int)s_probeStatus);
  }
  usb_host_transfer_free(t);
}

void recoverTimeout(usb_device_handle_t dev, uint8_t ep) {
  // 打印机的 bulk OUT 在深度休眠时会长时间 NAK：transfer 提交成功但永不完成。
  // 这不是「设备坏了」，更不能因为一次超时就把整机重启（重启后打印机还在睡，
  // 只会陷入 重启→再超时 的循环）。正确做法是复位端点、丢掉这一块，
  // 让作业失败并让客户端重试 —— 打印机被 USB 活动唤醒后自然就通了。
  setMsg("USB 传输超时，复位端点（不重启）");
  if (dev) {
    usb_host_endpoint_halt(dev, ep);
    usb_host_endpoint_flush(dev, ep);
  }
  // 等 transfer 真正回收（DMA 缓冲不能提前复用），最多 2s
  const uint32_t t0 = millis();
  for (;;) {
    bool busy = false;
    for (int i = 0; i < USB_XFER_SLOTS; ++i) {
      if (s_slot[i].inFlight && !s_slot[i].done) busy = true;
    }
    if (!busy || millis() - t0 >= 2000) break;
    sysMonFeed();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (dev) usb_host_endpoint_clear(dev, ep);
  // 这一块已经作废：槽与计数清干净，后续作业可以重新提交
  for (int i = 0; i < USB_XFER_SLOTS; ++i) {
    s_slot[i].inFlight = false;
    s_slot[i].done = false;
  }
  portENTER_CRITICAL(&s_mux);
  s_pending = 0;
  s_inFlight = false;
  portEXIT_CRITICAL(&s_mux);

  // 诊断：同时读一次 IN 端点 + 一次 GET_PORT_STATUS，判断设备是整体停摆、
  // 只有 OUT 不收数据，还是打印机引擎报错（缺纸/门开）。
  probeInEndpoint();
  probePortStatus();
}

void hostDaemon(void *) {
  sysMonTaskRegister();
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(pdMS_TO_TICKS(1000), &flags);
    if ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) && !flag(s_frozen)) {
      usb_host_device_free_all();
    }
    sysMonFeed();
  }
}

void clientTask(void *) {
  sysMonTaskRegister();
  uint32_t hb = 0;
  for (;;) {
    usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    // 心跳：只要它在涨，就说明 client 任务在正常泵事件，
    // transfer 不完成就只能是「设备侧不应答」（打印机深度休眠时 bulk OUT 会一直 NAK）。
    if ((++hb % 2500) == 0) {
      FWLOGD("[usb] client hb=%lu pending=%u ready=%d\n",
                    (unsigned long)hb, (unsigned)usbPrinterPending(),
                    (int)usbPrinterReady());
    }
    servicePortStatus();          // 每 2s 刷新缺纸/选中状态
    serviceDeviceId();            // 枚举后读一次 1284 Device ID
    if (!flag(s_frozen)) {
      uint8_t addr = 0;
      portENTER_CRITICAL(&s_mux);
      if (s_pendingAddr && !s_dev && !s_pendingClose) {
        addr = s_pendingAddr;
        s_pendingAddr = 0;
      }
      portEXIT_CRITICAL(&s_mux);
      if (addr) openDevice(addr);

      portENTER_CRITICAL(&s_mux);
      const bool canClose = s_pendingClose && !s_inFlight;
      portEXIT_CRITICAL(&s_mux);
      if (canClose) closeDevice();

      // 打印机休眠会掉 USB，唤醒后 Host 不一定再发 NEW_DEV；只等事件就永远
      // 连不回来。这里在没有设备时主动重扫地址表，设备还在但写不动时强制重开。
      portENTER_CRITICAL(&s_mux);
      const bool noDev = (s_dev == nullptr) && !s_pendingAddr && !s_pendingClose;
      portEXIT_CRITICAL(&s_mux);
      if (noDev) rescanDevices();
      serviceFaultRecovery();
    }
    sysMonFeed();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
}  // namespace

void usbPrinterBegin() {
  memset(&s_st, 0, sizeof(s_st));
  setMsg("等待 USB 打印机插入…");

  usb_host_config_t hc = {};
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  esp_err_t e = usb_host_install(&hc);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    // 此时 monitorTask 尚未创建，sysMonRequestRestart 不会被执行
    FWLOGE("[usb] usb_host_install failed: %d, restarting\n", (int)e);
    delay(500);
    ESP.restart();
    return;
  }

  usb_host_client_config_t cc = {};
  cc.is_synchronous = false;
  cc.max_num_event_msg = 8;
  cc.async.client_event_callback = clientEventCallback;
  cc.async.callback_arg = nullptr;
  if (usb_host_client_register(&cc, &s_client) != ESP_OK) {
    FWLOGE_LN("[usb] client register failed");
    usb_host_uninstall();
    ESP.restart();
    return;
  }
  // 双缓冲乒乓：两块 16KB 一起分好，运行期不再申请/释放 DMA 缓冲
  for (int i = 0; i < USB_XFER_SLOTS; ++i) {
    if (usb_host_transfer_alloc(USB_XFER_SIZE, 0, &s_slot[i].t) != ESP_OK) {
      FWLOGE("[usb] transfer alloc %d failed\n", i);
      for (int k = 0; k < i; ++k) {
        usb_host_transfer_free(s_slot[k].t);
        s_slot[k].t = nullptr;
      }
      usb_host_client_deregister(s_client);
      s_client = nullptr;
      usb_host_uninstall();
      ESP.restart();
      return;
    }
    s_slot[i].t->context = &s_slot[i];
  }

  // 缺纸检测用的控制传输块（1 字节数据 + setup packet）
  if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 1, 0, &s_portXfer) != ESP_OK) {
    s_portXfer = nullptr;
    FWLOGE_LN("[usb] port-status transfer alloc failed（缺纸检测不可用）");
  }
  // 1284 Device ID 读取块（setup + 2 字节长度 + 最长 1024 字节 ASCII）
  if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 1026, 0, &s_idXfer) != ESP_OK) {
    s_idXfer = nullptr;
    FWLOGE_LN("[usb] device-id transfer alloc failed（机型自动识别不可用）");
    printerIdSetFallback(0, 0);
    s_idDone = true;
  }

  // USB 侧全部固定在 CORE_USB：绝不去和 Wi-Fi/LwIP 抢核 0
  if (xTaskCreatePinnedToCore(hostDaemon, "usb-daemon", 4096, nullptr, 5, nullptr, CORE_USB) != pdPASS ||
      xTaskCreatePinnedToCore(clientTask, "usb-client", 4096, nullptr, 4, nullptr, CORE_USB) != pdPASS) {
    FWLOGE_LN("[usb] task create failed");
    ESP.restart();
  }
}

UsbPrinterStatus usbPrinterStatus() {
  UsbPrinterStatus copy;
  portENTER_CRITICAL(&s_mux);
  copy = s_st;
  portEXIT_CRITICAL(&s_mux);
  return copy;
}

bool usbPrinterReady() {
  portENTER_CRITICAL(&s_mux);
  const bool ok = s_dev && s_st.connected && !s_devGone && !s_pendingClose &&
                  !s_st.fault && !s_frozen;
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

bool usbPrinterPaperEmpty() {
  portENTER_CRITICAL(&s_mux);
  const bool v = s_st.paperEmpty;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

uint32_t usbPrinterBufCorrupt() { return s_bufCorrupt; }

// ---- 双缓冲乒乓流水线 ------------------------------------------------------
// 只允许一个生产者（spool 搬运任务），因此下标推进不需要额外加锁：
// submitIdx 单调前进，waitIdx 单调追赶，天然构成 FIFO。
uint8_t usbPrinterPending() { return s_pending; }
uint8_t usbPrinterRoom() { return (uint8_t)(USB_XFER_SLOTS - s_pending); }

bool usbPrinterSubmit(const uint8_t *data, size_t len) {
  if (!data || !len || len > USB_XFER_SIZE) return false;
  if (s_pending >= USB_XFER_SLOTS) return false;
  XferSlot &s = s_slot[s_submitIdx];
  if (!s.t || s.inFlight) return false;

  // 「检查 + 取设备快照 + 置 inFlight」必须在同一临界区，
  // 否则 DEV_GONE 可能抢在 inFlight 置位前关闭设备。
  usb_device_handle_t dev;
  uint8_t ep;
  portENTER_CRITICAL(&s_mux);
  if (s_frozen || !s_dev || !s_st.connected || s_devGone || s_pendingClose || s_st.fault) {
    portEXIT_CRITICAL(&s_mux);
    return false;
  }
  dev = s_dev;
  ep = s_st.outEp;
  s_inFlight = true;
  portEXIT_CRITICAL(&s_mux);

  // 先填缓冲/字段，再置 done=false —— 顺序反了会让 wait 看到上一轮的 done
  s.t->device_handle = dev;
  s.t->bEndpointAddress = ep;
  s.t->callback = xferCallback;
  s.t->context = &s;
  s.t->num_bytes = (int)len;
  s.t->flags = 0;
  memcpy(s.t->data_buffer, data, len);
  s.crcBefore = bufAdler(s.t->data_buffer, len);   // 提交前基线
  s.status = -1;
  s.actual = 0;
  s.len = len;
  s.done = false;
  s.inFlight = true;

  s_xferWaiter = xTaskGetCurrentTaskHandle();   // 先登记再提交，避免完成回调落空
  const esp_err_t err = usb_host_transfer_submit(s.t);
  if (err != ESP_OK) {
    s_xferWaiter = nullptr;          // 不留下指向本任务的悬空唤醒目标
    s.inFlight = false;
    s.done = false;
    if (s_pending == 0) setFlag(s_inFlight, false);
    // 打印具体错误码：超时/未配置/参数非法对应的处理完全不同，
    // 只写「提交失败」等于没写。
    static uint32_t fails = 0;
    if ((++fails % 10) == 1) {
      FWLOGE("[usb] submit 失败 err=%d (%s) len=%u ep=0x%02X\n",
                    (int)err, esp_err_to_name(err), (unsigned)len, ep);
    }
    setMsg("USB 提交失败");
    return false;
  }
  s_submitIdx = (uint8_t)((s_submitIdx + 1) % USB_XFER_SLOTS);
  ++s_pending;
  return true;
}

size_t usbPrinterWait(size_t *submitted) {
  if (submitted) *submitted = 0;
  if (!s_pending) return 0;

  XferSlot &s = s_slot[s_waitIdx];
  const size_t want = s.len;
  const uint32_t start = millis();
  uint32_t lastLog = start;
  bool timedOut = false;
  for (;;) {
    if (s.done) break;
    if (flag(s_devGone)) break;
    if (millis() - start >= USB_XFER_TIMEOUT_MS) {
      recoverTimeout(s.t->device_handle, s.t->bEndpointAddress);
      timedOut = true;
      break;
    }
    // 每 5s 打一次现场：done/status 能区分「设备不应答」和「回调没跑」
    if (millis() - lastLog >= 5000) {
      lastLog = millis();
      FWLOGD("[usb] wait %us done=%d status=%d actual=%u want=%u ep=0x%02X num=%d\n",
                    (unsigned)((millis() - start) / 1000), (int)s.done, (int)s.status,
                    (unsigned)s.actual, (unsigned)want, s.t->bEndpointAddress,
                    s.t->num_bytes);
    }
    sysMonFeed();
    // 完成回调会立刻 notify；5ms 只是兜底（回调在别的任务里，极端情况下
    // 可能在两次检查之间刚好错过一次 notify）。
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
  }
  s_xferWaiter = nullptr;

  const bool gone = flag(s_devGone);
  size_t written = 0;
  // IDF: COMPLETED 允许短包，按实际字节数推进，剩余字节交给上层重发
  if (!timedOut && !gone && s.done &&
      s.status == (int)USB_TRANSFER_STATUS_COMPLETED && s.actual <= want) {
    written = s.actual;
    // DMA 在飞期间缓冲必须原封不动。校验整块 want（短包时未发送的尾部也
    // 不应该被改），不一致立即报告并计数 —— 这是脏数据竞态的直接证据。
    const uint32_t crcAfter = bufAdler(s.t->data_buffer, want);
    if (crcAfter != s.crcBefore) {
      ++s_bufCorrupt;
      FWLOGE("[usb] 缓冲校验不一致! slot=%d want=%u before=%08lx after=%08lx (第%lu次)\n",
             (int)s_waitIdx, (unsigned)want,
             (unsigned long)s.crcBefore, (unsigned long)crcAfter,
             (unsigned long)s_bufCorrupt);
    }
  }

  s.inFlight = false;
  s_waitIdx = (uint8_t)((s_waitIdx + 1) % USB_XFER_SLOTS);
  if (s_pending) --s_pending;

  portENTER_CRITICAL(&s_mux);
  if (s_pending == 0) s_inFlight = false;   // 还有在飞就保持 true，阻止 closeDevice
  if (written > 0) s_st.bytesTotal += written;
  portEXIT_CRITICAL(&s_mux);

  if (submitted) *submitted = want;
  if (written == 0 && !gone && !timedOut) {
    portENTER_CRITICAL(&s_mux);
    s_st.fault = true;
    ++s_st.usbErrors;
    portEXIT_CRITICAL(&s_mux);
    setMsg("USB 写入失败，准备自动重启恢复");
    sysMonRequestRestart("usb write failed");
  }
  return written;
}

// 作业收尾：把所有在飞块落地（字节数不回收，交给上层按作业整体统计）。
// 必须在 spoolReset() 之前调用，否则 DMA 可能还在读已被复用的环形块。
void usbPrinterFlush() {
  uint8_t guard = 0;
  while (s_pending && guard++ < USB_XFER_SLOTS * 2) {
    usbPrinterWait(nullptr);
  }
}

// 短包保护：一旦某块只送出一部分，第二块在环里的起始位置就和设备实际收到的
// 字节流错位了（USB bulk 严格按提交顺序落地），只能把在飞块全部丢弃并清端点，
// 由上层判作业失败、让客户端重发整份文档 —— 绝不把错位的数据继续喂给打印机。
void usbPrinterAbort() {
  usb_device_handle_t dev;
  uint8_t ep;
  portENTER_CRITICAL(&s_mux);
  dev = s_dev;
  ep = s_st.outEp;
  portEXIT_CRITICAL(&s_mux);

  if (dev) {
    usb_host_endpoint_halt(dev, ep);
    usb_host_endpoint_flush(dev, ep);
  }
  for (int i = 0; i < USB_XFER_SLOTS; ++i) {
    s_slot[i].inFlight = false;
    s_slot[i].done = false;
    s_slot[i].len = 0;
  }
  s_pending = 0;
  s_submitIdx = s_waitIdx = 0;
  portENTER_CRITICAL(&s_mux);
  s_inFlight = false;
  portEXIT_CRITICAL(&s_mux);
  if (dev) usb_host_endpoint_clear(dev, ep);
  setMsg("USB 短包：已丢弃在飞数据并清端点，作业按失败处理");
}

size_t usbPrinterWrite(const uint8_t *data, size_t len) {
  if (!usbPrinterSubmit(data, len)) return 0;
  return usbPrinterWait(nullptr);
}
