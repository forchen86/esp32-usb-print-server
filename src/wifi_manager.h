#pragma once
#include <stdint.h>
#include <stddef.h>

struct WifiStatus {
  bool     staConnected;
  bool     apOn;              // AP 是否开着（配网成功后会自动关）
  char     ssid[33];
  char     staIp[16];
  char     apIp[16];
  char     apSsid[40];
  int32_t  rssi;
  uint32_t reconnectFails;
  char     message[160];
};

void        wifiManagerBegin();
void        wifiManagerService();              // 由监控任务每 200ms 调用
WifiStatus  wifiManagerStatus();
bool        wifiManagerSetCredentials(const char *ssid, const char *pass);
void        wifiManagerForget();

// Wi-Fi 扫描：所有 WiFi.* 扫描 API 只在 monitorTask 内调用，避免跨任务并发。
// Web handler 只投递请求 / 取结果，不直接触碰 WiFi 库。
void        wifiManagerRequestScan();
bool        wifiManagerScanReady();
bool        wifiManagerScanCopy(char *out, size_t cap);   // 取走结果 JSON 并清就绪标志
