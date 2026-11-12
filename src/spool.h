#pragma once
#include <stdint.h>
#include <stddef.h>

struct SpoolStatus {
  size_t   capacity;
  size_t   used;
  size_t   peak;              // 历史峰值，用来确认有没有被顶满
  uint32_t backpressureEvents;
  bool     paused;            // 高水位暂停收包（spool 自己决定）
  bool     forced;            // 低 PSRAM 强制暂停收包（psram guard 决定）
  bool     psram;             // 是否真的分配在 PSRAM 上
  size_t   psramTotal;        // PSRAM 总容量
  size_t   psramFree;         // 当前剩余 PSRAM
  size_t   psramLargest;      // 当前最大连续可分配块（碎片指标）
  size_t   reserveFloor;      // 开机时给 Wi-Fi/LwIP 留下的空闲 PSRAM（实测值）
};

bool        spoolBegin();      // 自动按可用 PSRAM 分配，失败则回退内部 RAM
void        spoolReset();      // 作业边界清空（由生产者调用）
SpoolStatus spoolStatus();
size_t      spoolUsed();
size_t      spoolFree();
bool        spoolIsPaused();
bool        spoolCanReceive(); // 高/低水位滞回 + 低压强制暂停：false 时必须停止 recv

// PSRAM 低压保护：true = 强制停收（不改变水位滞回状态机，可随时解除）
void        spoolForcePause(bool on);
bool        spoolForcedPaused();

// 单生产者 / 单消费者：先 acquire 拿到连续可写(读)区域，用完再 commit。
// acquire 返回的长度永远不会超过可用空间 —— 这是「永不被顶满」的硬保证。
size_t      spoolWriteAcquire(size_t maxBytes, uint8_t **ptr);
void        spoolWriteCommit(size_t n);
size_t      spoolReadAcquire(size_t maxBytes, uint8_t **ptr);
// 从「读指针 + skip」处取数据，skip = 已经提交给 USB、但还没 commit 的字节数。
// 乒乓流水线会同时持有两块，第二次 acquire 必须跳过第一块，否则会拿到同一段。
size_t      spoolReadAcquireFrom(size_t skip, size_t maxBytes, uint8_t **ptr);
void        spoolReadCommit(size_t n);
