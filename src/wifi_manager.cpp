#include "wifi_manager.h"
#include "config.h"
#include "mdns_responder.h"
#include "printer_cfg.h"
#include "sysmon.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_mac.h>

namespace {
// 所有共享状态由 s_mux 保护；WiFi.* 一律只在 monitorTask（或启动阶段）调用，
// Web handler 只写 NVS + 内存副本 + pending 标志，避免跨任务重入 WiFi 库。
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
char     g_ssid[33] = "";
char     g_pass[64] = "";
char     g_apSsid[40] = "";
char     g_apIp[16] = "192.168.4.1";
char     g_staIp[16] = "";
char     g_msg[160] = "init";
bool     g_connected = false;
bool     g_everConnected = false;
volatile bool g_apOn = true;       // AP 是否开着（配网成功后自动关）
uint32_t g_staUpSince = 0;         // STA 连续在线的起点（0 = 当前不在线）
uint32_t g_staDownSince = 0;       // STA 掉线的起点（0 = 当前在线）
int32_t  g_rssi = 0;
uint32_t g_lastAttempt = 0;
uint32_t g_retry = WIFI_RETRY_MIN_MS;
uint32_t g_fails = 0;
volatile bool g_pendingApply = false;
volatile bool g_pendingDisconnect = false;

// --- 扫描状态机（只在 monitorTask 里跑）-------------------------------------
volatile bool g_scanReq = false;
volatile bool g_scanDone = false;
volatile bool g_scanInFlight = false;
uint8_t  g_scanRetry = 0;
uint32_t g_scanStart = 0;
char     g_scanJson[2048] = "";

void setMsg(const char *s) {
  portENTER_CRITICAL(&s_mux);
  strlcpy(g_msg, s, sizeof(g_msg));
  portEXIT_CRITICAL(&s_mux);
  FWLOGD("[wifi] %s\n", s);
}

void attempt() {
  char ssid[33], pass[64];
  uint32_t fails, retry;
  portENTER_CRITICAL(&s_mux);
  strlcpy(ssid, g_ssid, sizeof(ssid));
  strlcpy(pass, g_pass, sizeof(pass));
  g_lastAttempt = millis();
  g_retry = min(g_retry * 2, (uint32_t)WIFI_RETRY_MAX_MS);
  ++g_fails;
  fails = g_fails;
  retry = g_retry;
  portEXIT_CRITICAL(&s_mux);

  WiFi.disconnect(false, false);       // 只断 STA，不影响常开的 AP
  WiFi.begin(ssid, pass);
  FWLOGD("[wifi] connect #%lu -> %s (backoff %lu ms)\n",
                (unsigned long)fails, ssid, (unsigned long)retry);
}


void jsonEscapeAppend(String &out, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    if (*p == '"' || *p == '\\') { out += '\\'; out += (char)*p; }
    else if (*p < 0x20) { char t[8]; snprintf(t, sizeof(t), "\\u%04x", *p); out += t; }
    else out += (char)*p;          // UTF-8 原样透传，浏览器按 utf-8 解码
  }
}

void setScanError(const char *why) {
  snprintf(g_scanJson, sizeof(g_scanJson), "{\"nets\":[],\"error\":\"%s\"}", why);
  g_scanDone = true;
}

// 每次 monitorTask 循环调用；异步扫描 + 失败重试，绝不阻塞，也不碰 loopTask。
void serviceScan() {
  if (g_scanInFlight) {
    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      if (millis() - g_scanStart > 15000) {
        WiFi.scanDelete();
        g_scanInFlight = false;
        setScanError("scan timeout，请手动填写 SSID");
      }
      return;
    }
    if (n < 0) {
      WiFi.scanDelete();
      g_scanInFlight = false;
      setScanError("scan failed，请手动填写 SSID");
      return;
    }
    String out = "{\"nets\":[";
    const int count = min(n, 20);
    for (int i = 0; i < count; ++i) {
      if (i) out += ",";
      out += "{\"s\":\"";
      jsonEscapeAppend(out, WiFi.SSID(i).c_str());
      out += "\",\"r\":" + String(WiFi.RSSI(i));
      out += ",\"e\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? 1 : 0) + "}";
      if (out.length() > (int)sizeof(g_scanJson) - 64) break;
    }
    out += "]}";
    strlcpy(g_scanJson, out.c_str(), sizeof(g_scanJson));
    WiFi.scanDelete();
    g_scanInFlight = false;
    g_scanDone = true;
    FWLOGD("[wifi] scan done, %d APs\n", n);
    return;
  }

  if (g_scanReq) {
    g_scanReq = false;
    WiFi.scanDelete();
    const int r = WiFi.scanNetworks(true);   // 异步：成功返回 WIFI_SCAN_RUNNING(-1)
    if (r == WIFI_SCAN_FAILED) {
      ++g_scanRetry;
      FWLOGE("[wifi] scan start FAILED r=%d mode=%d status=%d (try %u)\n",
                    r, (int)WiFi.getMode(), (int)WiFi.status(), (unsigned)g_scanRetry);
      if (g_scanRetry < 4) {
        g_scanReq = true;                    // 下一轮 200ms 后重试，避开驱动瞬时忙
      } else {
        g_scanRetry = 0;
        setScanError("Wi-Fi 扫描不可用，请手动填写 SSID");
      }
      return;
    }
    g_scanRetry = 0;
    g_scanInFlight = true;
    g_scanStart = millis();
  }
}
}  // namespace

void wifiManagerBegin() {
  WiFi.persistent(false);          // 凭据由我们自己在 NVS 管理
  WiFi.setAutoReconnect(false);    // 重连完全由本模块掌控（带退避）
  WiFi.setSleep(false);
  WiFi.setHostname(FW_HOSTNAME);
  WiFi.mode(WIFI_AP_STA);

  // 直接读 eFuse：softAP 刚启动时 netif 未就绪，WiFi.macAddress() 会返回全 0
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(g_apSsid, sizeof(g_apSsid), "%s%02X%02X%02X",
           AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

  const char *apPass = AP_PASSWORD[0] ? AP_PASSWORD : nullptr;
  if (!WiFi.softAP(g_apSsid, apPass, AP_CHANNEL, 0, AP_MAX_CLIENTS)) {
    setMsg("AP 启动失败");
  } else {
    IPAddress ip = WiFi.softAPIP();
    snprintf(g_apIp, sizeof(g_apIp), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    FWLOGI("[wifi] AP %s / %s  (password: %s)\n",
                  g_apSsid, g_apIp, apPass ? apPass : "<open>");
  }

  // 被动 mDNS responder 常驻：只应答查询，不主动广播（不占空口、不惊动省电）
  mdnsBegin();

  Preferences p;                    // 局部对象，不与其它任务共享
  if (p.begin(NVS_NS_WIFI, false)) {
    String s = p.getString("ssid", "");
    String q = p.getString("pass", "");
    if (!s.length()) {
      // v1.4.0 起的通用命名空间；若为空，从旧命名空间迁移一次，升级不丢配置
      p.end();
      Preferences old;
      if (old.begin(NVS_NS_WIFI_OLD, true)) {
        s = old.getString("ssid", "");
        q = old.getString("pass", "");
        old.end();
      }
      if (s.length() && p.begin(NVS_NS_WIFI, false)) {
        p.putString("ssid", s);
        p.putString("pass", q);
        p.end();
      }
    } else {
      p.end();
    }
    strlcpy(g_ssid, s.c_str(), sizeof(g_ssid));
    strlcpy(g_pass, q.c_str(), sizeof(g_pass));
  }

  if (g_ssid[0]) {
    setMsg("正在连接已保存的 Wi-Fi…");
    attempt();                      // 启动阶段 monitorTask 尚未创建，无并发
  } else {
    setMsg("未配置 Wi-Fi：可直接连 AP 使用打印服务");
  }
}

void wifiManagerService() {
  serviceScan();                   // 扫描与下面所有 WiFi.* 都在同一任务，无并发

  bool apply = false, disconnect = false;
  portENTER_CRITICAL(&s_mux);
  if (g_pendingApply) { g_pendingApply = false; apply = true; }
  if (g_pendingDisconnect) { g_pendingDisconnect = false; disconnect = true; }
  portEXIT_CRITICAL(&s_mux);

  if (disconnect) WiFi.disconnect(false, false);
  if (apply) attempt();

  const bool now = (WiFi.status() == WL_CONNECTED);
  const int32_t rssi = now ? WiFi.RSSI() : 0;
  const IPAddress ip = now ? WiFi.localIP() : IPAddress();

  bool wasConnected, ever, justConnected = false;
  uint32_t fails, retry, lastAttempt;
  char ssid[33];
  portENTER_CRITICAL(&s_mux);
  wasConnected = g_connected;
  if (now && !wasConnected) {
    g_connected = true;
    g_everConnected = true;
    g_fails = 0;
    g_retry = WIFI_RETRY_MIN_MS;
    g_staUpSince = millis();
    g_staDownSince = 0;
    snprintf(g_staIp, sizeof(g_staIp), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    strlcpy(g_msg, "Wi-Fi 已连接", sizeof(g_msg));
    justConnected = true;
  } else if (!now && wasConnected) {
    g_connected = false;
    g_staIp[0] = '\0';
    g_staUpSince = 0;
    g_staDownSince = millis();
    strlcpy(g_msg, "Wi-Fi 断开，正在自动重连…", sizeof(g_msg));
  }
  if (now) g_rssi = rssi;
  ever = g_everConnected;
  fails = g_fails;
  retry = g_retry;
  lastAttempt = g_lastAttempt;
  strlcpy(ssid, g_ssid, sizeof(ssid));
  portEXIT_CRITICAL(&s_mux);

  if (justConnected) {
    FWLOGI("[wifi] connected, IP=%s, RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), (int)rssi);
  }
  if (!now && wasConnected) FWLOGI_LN("[wifi] disconnected -> reconnect");

  // AP 自动开关：
  //   · STA 连续在线满 60s 才关 AP —— 避免「连上就掉」时反复开关；
  //   · 凭据被清除 / 从未配置 —— 立刻开回来，否则设备直接失联；
  //   · STA 掉线满 3 分钟才重开 —— 短暂掉线由指数退避重连处理，不惊动用户。
  auto openAp = [&]() {
    const char *apPass = AP_PASSWORD[0] ? AP_PASSWORD : nullptr;
    if (WiFi.softAP(g_apSsid, apPass, AP_CHANNEL, 0, AP_MAX_CLIENTS)) {
      g_apOn = true;
      return true;
    }
    return false;
  };

  if (g_apOn) {
    if (now && g_staUpSince && millis() - g_staUpSince >= AP_CLOSE_AFTER_STA_MS) {
      WiFi.softAPdisconnect(true);
      g_apOn = false;
      setMsg("STA 已稳定 60s，AP 已关闭（省电 / 降干扰）");
    }
  } else if (!now && ssid[0] == '\0') {
    // 用户在网页上「清除 Wi-Fi 配置」后必须马上能连回 AP，不能等 3 分钟
    if (openAp()) setMsg("Wi-Fi 配置已清除，AP 已重开");
  } else if (!now && g_staDownSince &&
             millis() - g_staDownSince >= AP_REOPEN_AFTER_DOWN_MS) {
    if (openAp()) setMsg("STA 掉线超过 3 分钟，AP 已重开供配网");
  }

  if (!now && ssid[0] && !g_scanInFlight) {   // 扫描期间不发起连接，避免驱动状态冲突
    if (millis() - lastAttempt >= retry) {
      attempt();
      if (fails % 5 == 0) setMsg("Wi-Fi 重连中（AP 仍可打印）");
    }
    // 只有「曾经连上过、之后长期掉线」才重启自救；从未配好密码时保持 AP 可用
    if (ever && fails >= WIFI_MAX_FAILS) sysMonRequestRestart("wifi reconnect failed");
  }
}

WifiStatus wifiManagerStatus() {
  WifiStatus s = {};
  portENTER_CRITICAL(&s_mux);
  s.staConnected = g_connected;
  s.apOn = g_apOn;
  strlcpy(s.ssid, g_ssid, sizeof(s.ssid));
  strlcpy(s.staIp, g_staIp, sizeof(s.staIp));
  strlcpy(s.apIp, g_apIp, sizeof(s.apIp));
  strlcpy(s.apSsid, g_apSsid, sizeof(s.apSsid));
  s.rssi = g_rssi;
  s.reconnectFails = g_fails;
  strlcpy(s.message, g_msg, sizeof(s.message));
  portEXIT_CRITICAL(&s_mux);
  return s;
}

void wifiManagerRequestScan() {
  portENTER_CRITICAL(&s_mux);
  if (!g_scanDone && !g_scanInFlight) g_scanReq = true;
  portEXIT_CRITICAL(&s_mux);
}

bool wifiManagerScanReady() {
  portENTER_CRITICAL(&s_mux);
  const bool v = g_scanDone;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

bool wifiManagerScanCopy(char *out, size_t cap) {
  if (!out || !cap) return false;
  portENTER_CRITICAL(&s_mux);
  if (!g_scanDone) { portEXIT_CRITICAL(&s_mux); return false; }
  strlcpy(out, g_scanJson, cap);
  g_scanDone = false;
  portEXIT_CRITICAL(&s_mux);
  return true;
}

bool wifiManagerSetCredentials(const char *ssid, const char *pass) {
  if (!ssid || !ssid[0] || strlen(ssid) > 32) return false;
  if (pass && strlen(pass) > 63) return false;

  Preferences p;
  if (!p.begin(NVS_NS_WIFI, false)) return false;
  p.putString("ssid", ssid);
  p.putString("pass", pass ? pass : "");
  p.end();

  portENTER_CRITICAL(&s_mux);
  strlcpy(g_ssid, ssid, sizeof(g_ssid));
  strlcpy(g_pass, pass ? pass : "", sizeof(g_pass));
  g_fails = 0;
  g_retry = WIFI_RETRY_MIN_MS;
  g_everConnected = false;
  g_pendingApply = true;           // 由 monitorTask 统一执行 WiFi.begin
  strlcpy(g_msg, "已保存新 Wi-Fi，正在连接…", sizeof(g_msg));
  portEXIT_CRITICAL(&s_mux);
  return true;
}

void wifiManagerForget() {
  Preferences p;
  if (p.begin(NVS_NS_WIFI, false)) {
    p.remove("ssid");
    p.remove("pass");
    p.end();
  }
  portENTER_CRITICAL(&s_mux);
  g_ssid[0] = '\0';
  g_pass[0] = '\0';
  g_connected = false;
  g_everConnected = false;
  g_fails = 0;
  g_staIp[0] = '\0';
  g_pendingDisconnect = true;      // 由 monitorTask 统一执行 WiFi.disconnect
  strlcpy(g_msg, "已清除 Wi-Fi 配置，现在仅 AP 直连可用", sizeof(g_msg));
  portEXIT_CRITICAL(&s_mux);
}
