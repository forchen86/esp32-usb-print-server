#include <Arduino.h>
#include "config.h"
#include "sysmon.h"
#include "wifi_manager.h"
#include "web_ui.h"
#include "usb_printer.h"
#include "net_print.h"
#include "ipp_print.h"
#include "printer_cfg.h"
#include "raw_print.h"
#include "snmp_agent.h"
#include "spool.h"

#include "esp_arduino_version.h"

// native USB-OTG 必须留给 USB Host；任何一个 device 功能开机自启都会抢占 PHY
#if ARDUINO_USB_CDC_ON_BOOT || ARDUINO_USB_MSC_ON_BOOT || ARDUINO_USB_DFU_ON_BOOT
#error "请在 platformio.ini 中关闭 USB CDC / MSC / DFU on boot"
#endif

void setup() {
  Serial.begin(115200);
  delay(400);
  FWLOGI("\n==============================================\n");
  FWLOGI(" USB Print Server v%s\n", FW_VERSION);
  FWLOGI(" Arduino-ESP32 %s / ESP32-S3 USB Host\n", ESP_ARDUINO_VERSION_STR);
  FWLOGI("==============================================\n");

  sysMonInit();          // 任务看门狗（必须最先初始化）
  if (!spoolBegin()) {   // PSRAM spool：没有它就无法收发解耦
    FWLOGE_LN("[main] spool allocation failed, restarting");
    delay(1000);
    ESP.restart();
  }
  cfgBegin();            // 先读 NVS 里的打印选项（mDNS TXT 要用到双面状态）
  wifiManagerBegin();    // AP 常开 + STA 凭据 + 断线重连
  webUiBegin();          // Web 界面 / API
  usbPrinterBegin();     // USB Host 打印机类
  netPrintBegin();       // spool(环形) → USB 消费任务
  rawPrintBegin();       // 9100 RAW / PDL-Datastream（默认打印通路）
  ippPrintBegin();       // 631 IPP（AirPrint / Windows IPP）
  snmpAgentBegin();      // 161 SNMP 状态（数据源 = IPP printer-state）
  sysMonBegin();         // 健康监控任务（含 Wi-Fi 状态机与扫描）
}

void loop() {
  webUiLoop();
  cfgService();          // 双面开关改动后，把 mDNS TXT 补刷一次
  sysMonFeed();          // loopTask 也在 TWDT 监控之下
  delay(10);
}
