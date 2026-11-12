#include "printer_cfg.h"
#include "config.h"
#include "printer_identity.h"

#include <Arduino.h>
#include <Preferences.h>
#include <stdio.h>

namespace {

constexpr const char *NVS_NS      = NVS_NS_CFG;
constexpr const char *NVS_MODE    = "duplex_mode";
constexpr const char *NVS_PJL     = "pjl_duplex";
constexpr const char *NVS_OLD     = "duplex";   // v1.3.x 的布尔开关，迁移用

// 由 Web（loopTask）写、由 IPP / mDNS（其它任务）读，用 volatile 保证回内存取值。
volatile int  s_mode = DUPLEX_DEFAULT_MODE;
volatile bool s_pjl  = (PJL_DUPLEX_DEFAULT != 0);
volatile bool s_txtDirty = false;

bool store(int key, int value) {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return false;
  const bool ok = (key == 0) ? p.putInt(NVS_MODE, value) : p.putBool(NVS_PJL, value != 0);
  p.end();
  return ok;
}

}  // namespace

int cfgDuplexMode() { return s_mode; }

bool cfgDuplexEnabled() {
  const int m = s_mode;
  if (m == DUPLEX_ON) return true;
  if (m == DUPLEX_OFF) return false;
  return printerIdSupportsDuplex();
}

bool cfgDuplexDetected()        { return printerIdSupportsDuplex(); }
const char *cfgDuplexEvidence() { return printerIdDuplexEvidence(); }

bool cfgPjlDuplexEnabled() {
  // 对不懂 PJL 的机器（ESC/POS、ZPL 等）一律禁用，避免污染作业数据
  return s_pjl && printerIdSupportsPjl();
}

void cfgUrfString(bool duplex, char *out, size_t cap) {
  if (!out || cap < 8) return;
  snprintf(out, cap,
           "CP1,FN3,IS1-4-8-13,MT1-3-4-5-8-11,OB10,PQ0-4,RS300-600,SRGB24,V1.4,W8,%s",
           duplex ? "DM3" : "DM1");
}

void cfgSetDuplexMode(int mode) {
  if (mode < DUPLEX_AUTO || mode > DUPLEX_OFF) return;
  if (mode == s_mode) return;
  s_mode = mode;
  if (!store(0, mode)) {
    FWLOGE_LN("[cfg] 双面模式写 NVS 失败，本次运行有效、重启后回退");
  }
  s_txtDirty = true;
  FWLOGI("[cfg] 双面模式 -> %s（有效值 %s）\n",
                mode == DUPLEX_AUTO ? "自动" : (mode == DUPLEX_ON ? "强制开" : "强制关"),
                cfgDuplexEnabled() ? "双面" : "单面");
}

// 兼容旧调用
void cfgSetDuplexEnabled(bool on) {
  cfgSetDuplexMode(on ? DUPLEX_ON : DUPLEX_OFF);
}

void cfgSetPjlDuplexEnabled(bool on) {
  if (on == (bool)s_pjl) return;
  s_pjl = on;
  if (!store(1, on ? 1 : 0)) {
    FWLOGE_LN("[cfg] PJL 双面注入写 NVS 失败");
  }
  FWLOGI("[cfg] IPP sides → PJL 双面注入 %s\n", on ? "开启" : "关闭");
}

void cfgBegin() {
  Preferences p;
  bool haveMode = false;
  if (p.begin(NVS_NS, false)) {
    if (p.isKey(NVS_MODE)) {
      const int m = p.getInt(NVS_MODE, DUPLEX_AUTO);
      s_mode = (m >= DUPLEX_AUTO && m <= DUPLEX_OFF) ? m : DUPLEX_AUTO;
      haveMode = true;
    } else if (p.isKey(NVS_OLD)) {
      // v1.3.x 存的是布尔开关：迁移成强制开 / 强制关，不让老配置失效
      s_mode = p.getBool(NVS_OLD, false) ? DUPLEX_ON : DUPLEX_OFF;
      p.putInt(NVS_MODE, (int)s_mode);
      haveMode = true;
      FWLOGI("[cfg] 迁移旧双面开关 -> %s\n",
                    s_mode == DUPLEX_ON ? "强制开" : "强制关");
    }
    if (p.isKey(NVS_PJL)) s_pjl = p.getBool(NVS_PJL, true);
    p.end();
  }
  if (!haveMode) {
    // 旧命名空间也可能存过（v1.4.0 之前的命名）
    Preferences old;
    if (old.begin(NVS_NS_CFG_OLD, true) && old.isKey(NVS_OLD)) {
      s_mode = old.getBool(NVS_OLD, false) ? DUPLEX_ON : DUPLEX_OFF;
      old.end();
      store(0, (int)s_mode);
      FWLOGI("[cfg] 从旧命名空间迁移双面开关 -> %s\n",
                    s_mode == DUPLEX_ON ? "强制开" : "强制关");
    }
  }
  FWLOGI("[cfg] 双面模式=%s PJL注入=%s（自动识别要等 USB 枚举完成后生效）\n",
                s_mode == DUPLEX_AUTO ? "自动" : (s_mode == DUPLEX_ON ? "强制开" : "强制关"),
                s_pjl ? "开" : "关");
}

void cfgService() {
  // 被动 mDNS 每次应答都实时读 cfgDuplexEnabled()，不需要刷新缓存
  if (s_txtDirty) s_txtDirty = false;
}
