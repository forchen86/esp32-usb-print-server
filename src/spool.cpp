#include "spool.h"
#include "config.h"

#include <Arduino.h>
#include <esp32-hal-psram.h>
#include "esp_heap_caps.h"

// ============================================================================
// 小环形缓冲 + 双水位背压（替换原来的整包大缓存）
//
// 结构：SPOOL_BLOCK_COUNT 个固定 64KB 块组成环形池，开机一次性分配，运行期
// 绝不 malloc/free。环形「回绕」在这里等于「换下一块」，因此一次 acquire 永远
// 不会跨到未提交的区域，也就不需要任何跨块拼接逻辑。
//
// 背压：水位到 75% 就把 canReceive() 置 false，上层**停止 recv**（不是丢数据）；
// lwIP 的接收窗口随之收缩，客户端由 TCP 自己降速。降到 25% 才恢复。滞回区间
// 有 50 个百分点，绝不在阈值附近抖动。
// ============================================================================

namespace {
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

uint8_t *s_blk[SPOOL_BLOCK_COUNT] = {nullptr};
uint8_t  s_blocks = 0;
size_t   s_cap = 0;
bool     s_psram = false;

// 单调递增的读写计数：used = w - r，天然无「满/空」歧义。
// 单作业上限 32MB，size_t 不会回绕。
size_t   s_w = 0;
size_t   s_r = 0;
size_t   s_peak = 0;
bool     s_paused = false;      // 高水位滞回
bool     s_forced = false;      // 低 PSRAM 强制暂停
size_t   s_floor = 0;           // 分配完实测剩下的 PSRAM（给 Wi-Fi/LwIP 的余量）
uint32_t s_backpressure = 0;    // 进入「停收」的次数
bool     s_warned     = false;   // 一致性告警只打一次，避免刷屏

// s_r 绝不允许超过 s_w。一旦出现（说明配对错位），立刻把 r 拉回 w：
// 否则 used 会下溢成 4GB 级数字，free 也变成巨大值，读写全部越界。
size_t usedLocked() {
  if (s_r > s_w) s_r = s_w;
  return s_w - s_r;
}
size_t psramFreeNow() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

inline void locate(size_t pos, size_t &blk, size_t &off) {
  const size_t p = pos % s_cap;
  blk = p / SPOOL_BLOCK_BYTES;
  off = p % SPOOL_BLOCK_BYTES;
}
}  // namespace

bool spoolBegin() {
  const size_t total = ESP.getPsramSize();
  const size_t freeP = psramFreeNow();

  if (total >= (4UL * 1024UL * 1024UL) && freeP > SPOOL_PSRAM_RESERVE) {
    // 逐块申请：4 → 3 → 2 → 1。某一块失败就停在这一档，而不是整池放弃。
    for (uint8_t i = 0; i < SPOOL_BLOCK_COUNT; ++i) {
      // 每块分配前先确认「分完仍留得住 PSRAM 红线」，绝不越线。
      if (psramFreeNow() < SPOOL_BLOCK_BYTES + SPOOL_PSRAM_RESERVE) {
        FWLOGD("[ring] stop at %u block(s): psram free %u < %u\n",
                      (unsigned)i, (unsigned)psramFreeNow(),
                      (unsigned)(SPOOL_BLOCK_BYTES + SPOOL_PSRAM_RESERVE));
        break;
      }
      uint8_t *p = (uint8_t *)ps_malloc(SPOOL_BLOCK_BYTES);
      if (!p) {
        FWLOGE("[ring] block %u alloc failed, keep %u\n",
                      (unsigned)(i + 1), (unsigned)i);
        break;
      }
      s_blk[i] = p;
      ++s_blocks;
    }
    if (s_blocks) {
      s_psram = true;
      s_floor = psramFreeNow();
    }
  }

  if (!s_blocks) {
    // PSRAM 一块都放不下：内部 RAM 只兜一块 64KB，且必须留够 keep。
    // 绝不做「PSRAM 失败 → 把内部 RAM 吃满」这种退化。
    static const size_t kKeep = 64UL * 1024UL;
    const size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (freeInt >= SPOOL_BLOCK_BYTES + kKeep) {
      void *p = heap_caps_malloc(SPOOL_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (p) {
        s_blk[0] = (uint8_t *)p;
        s_blocks = 1;
        s_psram = false;
        s_floor = 0;
      }
    }
    if (!s_blocks) {
      FWLOGE("[ring] FATAL: no buffer (psram total %u free %u, internal free %u)\n",
                    (unsigned)total, (unsigned)freeP,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      return false;
    }
    FWLOGD("[ring] internal RAM 1 x %u B (psram total %u / free %u)\n",
                  (unsigned)SPOOL_BLOCK_BYTES, (unsigned)total, (unsigned)freeP);
  } else {
    FWLOGI("[ring] %u x %u B = %u B in PSRAM, left %u B for Wi-Fi/LwIP\n",
                  (unsigned)s_blocks, (unsigned)SPOOL_BLOCK_BYTES,
                  (unsigned)(s_blocks * SPOOL_BLOCK_BYTES), (unsigned)s_floor);
  }

  s_cap = (size_t)s_blocks * SPOOL_BLOCK_BYTES;
  s_w = s_r = 0;
  s_peak = 0;
  s_paused = false;
  s_forced = false;
  s_backpressure = 0;
  return true;
}

void spoolReset() {
  size_t w = 0, r = 0;
  portENTER_CRITICAL(&s_mux);
  w = s_w;
  r = s_r;
  s_w = s_r = 0;
  s_paused = false;
  portEXIT_CRITICAL(&s_mux);
  // 每个作业边界留一行，用来核对「生产者/消费者」的配对是否对称
  if (w || r) {
    FWLOGD("[spool] reset (w=%u r=%u)\n", (unsigned)w, (unsigned)r);
  }
}

size_t spoolUsed() {
  portENTER_CRITICAL(&s_mux);
  const size_t u = usedLocked();
  portEXIT_CRITICAL(&s_mux);
  return u;
}

size_t spoolFree() {
  portENTER_CRITICAL(&s_mux);
  const size_t f = s_cap - usedLocked();
  portEXIT_CRITICAL(&s_mux);
  return f;
}

bool spoolIsPaused() {
  portENTER_CRITICAL(&s_mux);
  const bool p = s_paused;
  portEXIT_CRITICAL(&s_mux);
  return p;
}

// 高水位停收 / 低水位复收（75% / 25%）。返回 false 时上层必须停止 recv，
// 让 TCP 窗口自己收缩 —— 不丢数据、不自己限速。
bool spoolCanReceive() {
  portENTER_CRITICAL(&s_mux);
  const size_t u = usedLocked();
  if (!s_paused && u * 100 >= s_cap * SPOOL_HIGH_WATER_PCT) {
    s_paused = true;
    ++s_backpressure;
  } else if (s_paused && u * 100 <= s_cap * SPOOL_LOW_WATER_PCT) {
    s_paused = false;
  }
  const bool allow = !s_paused && !s_forced;
  portEXIT_CRITICAL(&s_mux);
  return allow;
}

// 低 PSRAM 强制停收：与水位滞回解耦，PSRAM 回升后由 guard 解除
void spoolForcePause(bool on) {
  portENTER_CRITICAL(&s_mux);
  s_forced = on;
  portEXIT_CRITICAL(&s_mux);
}

bool spoolForcedPaused() {
  portENTER_CRITICAL(&s_mux);
  const bool f = s_forced;
  portEXIT_CRITICAL(&s_mux);
  return f;
}

size_t spoolWriteAcquire(size_t maxBytes, uint8_t **ptr) {
  *ptr = nullptr;
  constexpr size_t kBlk = (size_t)SPOOL_BLOCK_BYTES;   // 统一成 size_t，避免 min() 模板推导冲突
  portENTER_CRITICAL(&s_mux);
  const size_t freeBytes = s_cap - usedLocked();
  if (freeBytes) {
    size_t blk, off;
    locate(s_w, blk, off);
    size_t n = freeBytes < kBlk - off ? freeBytes : kBlk - off;
    if (n > maxBytes) n = maxBytes;
    if (n) *ptr = s_blk[blk] + off;
    portEXIT_CRITICAL(&s_mux);
    return n;
  }
  portEXIT_CRITICAL(&s_mux);
  return 0;
}

void spoolWriteCommit(size_t n) {
  bool   bad = false;
  size_t w = 0, r = 0, cap = 0;
  portENTER_CRITICAL(&s_mux);
  cap = s_cap;
  const size_t used = usedLocked();
  if (n > cap - used) {
    // 调用方 commit 的长度超过了它 acquire 到的空间：环形状态会永久错位
    // （used 下溢后 free 变成巨大值，读出来的全是垃圾）。这里钳住并记录现场。
    bad = true;
    w = s_w;
    r = s_r;
    n = cap - used;
  }
  s_w += n;
  const size_t u = usedLocked();
  if (u > s_peak) s_peak = u;
  portEXIT_CRITICAL(&s_mux);

  if (bad && !s_warned) {
    s_warned = true;
    FWLOGE("[spool] BAD writeCommit > cap (w=%u r=%u cap=%u), clamped\n",
                  (unsigned)w, (unsigned)r, (unsigned)cap);
  }
}

size_t spoolReadAcquire(size_t maxBytes, uint8_t **ptr) {
  return spoolReadAcquireFrom(0, maxBytes, ptr);
}

// skip 是「已提交给 USB、尚未 commit」的字节数。乒乓流水线在等待第一块完成时，
// 第二块必须从 s_r + skip 开始取，否则会和第一块拿到同一段数据（同一段被提交
// 两次、commit 两次，s_r 直接跑到 s_w 前面 → used 无符号下溢）。
size_t spoolReadAcquireFrom(size_t skip, size_t maxBytes, uint8_t **ptr) {
  *ptr = nullptr;
  constexpr size_t kBlk = (size_t)SPOOL_BLOCK_BYTES;
  portENTER_CRITICAL(&s_mux);
  const size_t used = usedLocked();
  if (skip < used) {
    size_t blk, off;
    locate(s_r + skip, blk, off);
    size_t avail = used - skip;
    size_t n = avail < kBlk - off ? avail : kBlk - off;
    if (n > maxBytes) n = maxBytes;
    if (n) *ptr = s_blk[blk] + off;
    portEXIT_CRITICAL(&s_mux);
    return n;
  }
  portEXIT_CRITICAL(&s_mux);
  return 0;
}

void spoolReadCommit(size_t n) {
  bool   bad = false;
  size_t w = 0, r = 0;
  portENTER_CRITICAL(&s_mux);
  w = s_w;
  r = s_r;
  if (r > w) {                                  // 已经错位：先记现场再自愈
    bad = true;
    s_r = w;
    n = 0;
  } else {
    const size_t used = w - r;
    if (n > used) {
      // 读提交超过了「已写未读」的量 = 生产者/消费者配对错位。
      // 这是 used 下溢（s_r 跑到 s_w 前面）的唯一入口，必须抓住。
      bad = true;
      n = used;
    }
    s_r += n;                                   // 提交后生产者才能复用这段空间
  }
  portEXIT_CRITICAL(&s_mux);

  if (bad && !s_warned) {
    s_warned = true;
    FWLOGE("[spool] BAD readCommit (n=%u w=%u r=%u), clamped\n",
                  (unsigned)n, (unsigned)w, (unsigned)r);
  }
}

SpoolStatus spoolStatus() {
  SpoolStatus s = {};
  portENTER_CRITICAL(&s_mux);
  s.capacity = s_cap;
  s.used = usedLocked();
  s.peak = s_peak;
  s.backpressureEvents = s_backpressure;
  s.paused = s_paused;
  s.forced = s_forced;
  s.reserveFloor = s_floor;
  portEXIT_CRITICAL(&s_mux);
  s.psram = s_psram;
  s.psramTotal = ESP.getPsramSize();
  s.psramFree  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s.psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  return s;
}
