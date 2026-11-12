#include "sysmon.h"
#include "config.h"
#include "usb_printer.h"
#include "wifi_manager.h"
#include "spool.h"

#include <Arduino.h>
#include <Preferences.h>
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

namespace {
portMUX_TYPE   s_restartMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool  g_restart = false;
char           g_restartReason[64] = "";
char           g_resetReason[32] = "";
char           g_lastFault[32] = "";   // 最近一次 WDT/panic 复位（NVS 持久化）
uint32_t       g_bootCount = 0;
volatile uint32_t g_allocFails = 0;

// 分配失败回调：可能运行在 Wi-Fi / LwIP / 任意任务里，且此刻内存已耗尽，
// 所以这里绝对不 malloc、不 String、不 Serial（Serial 写失败回调可能死在锁上），
// 只用 ROM 级 printf 打一行硬信息，方便定位是谁把 PSRAM/内部 RAM 吃爆的。
void onAllocFailed(size_t size, uint32_t caps, const char *function)
{
  g_allocFails = g_allocFails + 1;   // 不用 ++：C++20 下 volatile 自增已弃用
  esp_rom_printf("[heap] ALLOC FAILED size=%u caps=0x%08x from=%s "
                 "int_free=%u psram_free=%u\n",
                 (unsigned)size, (unsigned)caps, function ? function : "?",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// PSRAM 红线守护：spool 是固定大块分配，运行时唯一的 PSRAM 波动来自
// Wi-Fi 收发缓冲 / LwIP pbuf。掉到临界值就收紧 TCP 窗口（强制停收），
// 让 Wi-Fi 不再为新数据分配 pbuf，把 1MB 红线抢回来。
void psramGuardService() {
  static bool     crit = false;
  static uint32_t critSince = 0;
  static uint32_t lastLog = 0;

  const size_t freePs  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const uint32_t now = millis();

  // Wi-Fi / LwIP / mDNS 全部起来后的第一次快照：这才是「红线是否守住」的真凭据
  static bool reported = false;
  if (!reported && now > 2000) {
    reported = true;
    FWLOGD("[mem] internal heap total %u, free %u, largest %u\n",
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    FWLOGD("[mem] DMA-capable free %u, largest %u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    FWLOGD("[mem] psram total %u, free %u, largest %u, alloc fails %lu\n",
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                  (unsigned)freePs, (unsigned)largest,
                  (unsigned long)g_allocFails);
  }

  if (freePs < PSRAM_CRIT_FREE_BYTES) {
    if (!crit) {
      crit = true;
      critSince = now;
      FWLOGI("[guard] PSRAM critical: free %u (largest %u) -> pause RX\n",
                    (unsigned)freePs, (unsigned)largest);
      spoolForcePause(true);
    } else if (!spoolForcedPaused()) {
      spoolForcePause(true);              // 作业重新开始后仍要守住
    }
    if (now - critSince >= PSRAM_CRIT_HOLD_MS) {
      sysMonRequestRestart("psram exhausted");
    }
    return;
  }

  if (crit) {
    if (freePs >= PSRAM_RECOVER_BYTES) {  // 滞回，避免在临界线上反复开关
      crit = false;
      spoolForcePause(false);
      FWLOGI("[guard] PSRAM recovered: free %u (largest %u)\n",
                    (unsigned)freePs, (unsigned)largest);
    }
    return;
  }

  if (freePs < PSRAM_WARN_FREE_BYTES || largest < PSRAM_MIN_LARGEST_BYTES) {
    if (now - lastLog >= 10000) {
      lastLog = now;
      FWLOGI("[guard] PSRAM low: free %u, largest %u, alloc failures %lu\n",
                    (unsigned)freePs, (unsigned)largest,
                    (unsigned long)g_allocFails);
    }
  }
}

const char *reasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

void ledService() {
#if STATUS_LED_PIN >= 0
  static uint32_t last = 0;
  static bool on = false;
  uint32_t period = 1000;
  if (!usbPrinterReady()) period = 200;
  else if (!wifiManagerStatus().staConnected) period = 1000;
  else {
    digitalWrite(STATUS_LED_PIN, HIGH);   // 一切正常: 常亮
    return;
  }
  if (millis() - last >= period) {
    last = millis();
    on = !on;
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
  }
#endif
}

#if ENABLE_SERIAL_DEBUG_CMDS
// 故意卡死：订阅 TWDT 后彻底停止喂狗，用来现场验证「15s 后是不是真的会
// 自动重启」。这是唯一能证明看门狗生效的实验手段 —— 否则只能靠推断。
void hangTestTask(void *) {
  esp_task_wdt_add(NULL);
  FWLOGD_LN("[selftest] 已订阅 TWDT 并停止喂狗，预计 15s 内 panic 重启");
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));     // 只睡不喂：真正的卡死形态
  }
}

// 串口单行命令：wdt / reboot / stat
void serialCmdService() {
  static char   line[32];
  static size_t n = 0;
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch < 0) break;
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (n + 1 < sizeof(line)) line[n++] = (char)ch;
      else n = 0;                        // 超长丢弃，不留在缓冲里
      continue;
    }
    line[n] = '\0';
    n = 0;
    if (!line[0]) continue;

    if (!strcmp(line, "wdt")) {
      FWLOGD_LN("[selftest] 创建不喂狗的任务，15s 内应触发 TWDT panic");
      if (xTaskCreate(hangTestTask, "hangs", 2048, nullptr, 1, nullptr) != pdPASS) {
        FWLOGE_LN("[selftest] 任务创建失败");
      }
    } else if (!strcmp(line, "reboot")) {
      FWLOGD_LN("[selftest] 受控重启");
      sysMonRequestRestart("serial cmd");
    } else if (!strcmp(line, "stat")) {
      const SpoolStatus sp = spoolStatus();
      FWLOGI("[stat] tasks %u | int free %u largest %u | psram free %u | "
                    "spool %u/%u peak %u | alloc fails %lu\n",
                    (unsigned)uxTaskGetNumberOfTasks(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    (unsigned)sp.used, (unsigned)sp.capacity, (unsigned)sp.peak,
                    (unsigned long)g_allocFails);
    } else {
      FWLOGI_LN("[cmd] wdt | reboot | stat");
    }
  }
}
#endif  // ENABLE_SERIAL_DEBUG_CMDS

void monitorTask(void *) {
  sysMonTaskRegister();
  for (;;) {
    sysMonFeed();
    wifiManagerService();
    ledService();
    psramGuardService();          // 守住给 Wi-Fi/LwIP 的 PSRAM 红线
#if ENABLE_SERIAL_DEBUG_CMDS
    serialCmdService();           // 串口诊断命令（含看门狗自测）
#endif

    // 注意：ESP.getFreeHeap() 是「含 PSRAM 的总堆」，PSRAM 还剩 1MB 时它永远
    // 不会低于这个阈值 —— 用它判断内部内存会把自恢复彻底废掉。这里必须显式
    // 只看 internal（Wi-Fi/LwIP/USB 的 DMA 描述符都要从这里拿）。
    const size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largeInt = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (freeInt < LOW_HEAP_RESTART_BYTES || largeInt < 4096) {
      FWLOGE("[sysmon] low internal heap: free %u largest %u\n",
                    (unsigned)freeInt, (unsigned)largeInt);
      sysMonRequestRestart("low heap");
    }
    if (g_restart) {
      FWLOGI("[sysmon] restart requested: %s\n", sysMonRestartReason());
      delay(WDT_RESTART_DELAY_MS);   // 留出时间让串口/网页把原因发出来
      esp_restart();
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
}  // namespace

void sysMonInit() {
  const esp_reset_reason_t rr = esp_reset_reason();
  strlcpy(g_resetReason, reasonName(rr), sizeof(g_resetReason));

  Preferences p;
  if (p.begin("sysmon", false)) {
    g_bootCount = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", g_bootCount);
    if (rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_PANIC) {
      p.putString("lastwdt", String(g_resetReason));
    }
    // 不论本次是不是异常复位，都把「上一次异常」读出来：正常重启不该抹掉它，
    // 否则查「它到底为什么自己重启过」时永远只看得到最近一次。
    // 先判键是否存在：直接 getString 不存在的键会让 Preferences 打一条
    // [E] nvs_get_str len fail: NOT_FOUND，每次启动都是噪音。
    String lw = p.isKey("lastwdt") ? p.getString("lastwdt", "") : String("");
    strlcpy(g_lastFault, lw.c_str(), sizeof(g_lastFault));
    p.end();
  }

  // SDK 已默认启用 TWDT(5s)。这里把它重配成 15s + panic 重启，
  // 不 deinit，避免影响 Arduino 已经订阅的任务。
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms      = WDT_TIMEOUT_S * 1000;    // 此 IDF 版本用毫秒
  cfg.idle_core_mask  = (1 << portNUM_PROCESSORS) - 1;
  cfg.trigger_panic   = true;
  esp_err_t e = esp_task_wdt_reconfigure(&cfg);   // 已在跑就直接改参数
  if (e == ESP_ERR_INVALID_STATE) e = esp_task_wdt_init(&cfg);  // 还没初始化才 init
  if (e != ESP_OK) {
    // 走到这里说明 SDK 的状态和预期不一致：干脆推倒重建，
    // 因为「看门狗没配上」等于整机失去自恢复能力，必须确保真的武装上。
    esp_task_wdt_deinit();
    e = esp_task_wdt_init(&cfg);
  }
  if (e != ESP_OK) FWLOGE("[sysmon] TWDT configure failed: %d（自恢复不可用）\n", (int)e);
  esp_task_wdt_add(NULL);                      // 注册 Arduino loopTask

  // 分配失败诊断钩子：真的 OOM 时能在串口看到是谁、要多少、要哪类内存
  if (heap_caps_register_failed_alloc_callback(onAllocFailed) != ESP_OK) {
    FWLOGE_LN("[sysmon] alloc-failed hook unavailable");
  }

  FWLOGI("[sysmon] TWDT %ds, boot #%lu, reset=%s\n",
                WDT_TIMEOUT_S, (unsigned long)g_bootCount, g_resetReason);
}

void sysMonBegin() {
#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
#endif
  // 监控任务固定在 CORE_USB：它是「守夜人」，不该和 Wi-Fi/LwIP 抢核 0
  if (xTaskCreatePinnedToCore(monitorTask, "sysmon", 4096, nullptr, 2, nullptr,
                              CORE_USB) != pdPASS) {
    FWLOGE_LN("[sysmon] FATAL: cannot create monitor task");
    delay(1000);
    esp_restart();
  }
}

void sysMonTaskRegister() {
  esp_err_t e = esp_task_wdt_add(NULL);
  if (e != ESP_OK && e != ESP_ERR_INVALID_ARG) {
    FWLOGE("[sysmon] cannot register task: %d\n", (int)e);
  }
}

void sysMonFeed() {
  esp_task_wdt_reset();
}

void sysMonRequestRestart(const char *reason) {
  portENTER_CRITICAL(&s_restartMux);
  if (!g_restart) {
    strlcpy(g_restartReason, reason ? reason : "unknown", sizeof(g_restartReason));
    g_restart = true;
  }
  portEXIT_CRITICAL(&s_restartMux);
}

uint32_t sysMonBootCount()      { return g_bootCount; }
uint32_t sysMonAllocFails()     { return g_allocFails; }
const char *sysMonResetReason() { return g_resetReason; }
const char *sysMonLastFault() { return g_lastFault; }

const char *sysMonRestartReason() {
  static char copy[64];
  portENTER_CRITICAL(&s_restartMux);
  strlcpy(copy, g_restartReason[0] ? g_restartReason : "-", sizeof(copy));
  portEXIT_CRITICAL(&s_restartMux);
  return copy;
}
