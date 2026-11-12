# 固件复核报告

复核对象：HP M203d / ESP32-S3 N16R8 打印服务器固件（PlatformIO + Arduino-ESP32 3.3.0 / IDF 5.5）。
复核方式：逐文件静态审查 + 独立代码审查交叉验证 + 编译/ELF 符号校验 + 前端 JS 语法检查。

结论：共确认并修复 **15 项缺陷**（2 致命 / 2 高 / 5 中 / 6 低），其中两项致命问题会直接导致大作业数据错乱或整份文档重复打印。修正后重新编译通过，镜像与校验值已更新。

---

## 一、发现并修复的缺陷

### 致命

| # | 位置 | 问题 | 修法 |
|---|---|---|---|
| F1 | `net_print.cpp` 作业边界 | `s_writerBusy` 在 `spoolReadCommit()` 之前清零，且生产者只等 3 秒；`usbPrinterWrite()` 最长可阻塞 30 秒。`spoolReset()` 会与遗留的读提交重叠，导致 `s_w - s_r` 下溢或新作业数据错位 | 改为**请求/应答握手**：`spoolReset()` 只允许消费者在自己循环顶部（保证不在 USB 搬运中）执行；生产者 `requestSpoolReset()` 等待 `s_resetDone`，超时则请求重启 |
| F2 | `net_print.cpp` idle 超时 | 背压暂停期间 `last` 冻结，长时间背压后恢复收包的瞬间会命中 60s 空闲超时，判失败并发 RST → 打印队列重发整份文档 | 暂停期间持续刷新 `last`；idle 超时改为**按正常完成收尾**（`s_jobEof=true`）而非失败，只有 0 字节空连接才判失败 |

### 高

| # | 位置 | 问题 | 修法 |
|---|---|---|---|
| H1 | `wifi_manager.cpp` / `web_ui.cpp` | Web handler 在 loopTask 直接调用 `WiFi.begin/disconnect/SSID/RSSI`，与 monitorTask 的重连逻辑并发；且打印中改 Wi-Fi 会打断作业 | 所有 WiFi API 收敛到 monitorTask；Web 只写 NVS + 内存副本 + `pendingApply` 标志；状态查询只读带锁缓存；`/api/wifi`、`/api/forget`、`/api/reboot` 在打印中返回 409 |
| H2 | `usb_printer.cpp` | 「检查设备 + 取 `dev` 快照 + 置 `s_inFlight`」不是原子的，`DEV_GONE` 可能抢在置位前关闭设备，导致 transfer 用了已释放句柄 | 三者合并进同一临界区；`closeDevice()` 内二次确认 `!s_inFlight`；统一用 `s_mux` 保护 `s_st` 与全部标志 |

### 中

| # | 位置 | 问题 | 修法 |
|---|---|---|---|
| M1 | `usb_printer.cpp` | IDF 允许 `COMPLETED` 短包，原实现要求 `actual == len`，短包会判失败并重启 | `usbPrinterWrite()` 返回**实际送出字节数**，消费者只提交该长度，剩余字节下一轮重发；`actual == 0` 才视为异常 |
| M2 | `usb_printer.cpp` | 传输超时只等 2 秒就无条件清 `s_inFlight`，若回调晚到会 use-after-free | 等待时间延长到与传输超时同量级；新增 `s_frozen` 冻结标志，重启前禁止 `device_free_all` / `closeDevice` / 新枚举 |
| M3 | `web_ui.cpp` | `WiFi.scanNetworks()` 同步阻塞 loopTask 数秒，且 `n < 0` 时死循环 | 改异步扫描：首次请求 `scanNetworks(true)` 返回 `pending`，前端轮询 `scanComplete()`；15 秒超时兜底；负值显式处理 |
| M4 | `usb_printer.cpp` | 拒绝非打印机设备后未 `device_free_all()`，设备表残留导致重插不再枚举 | `rejectDevice()` 统一 close + `device_free_all()` + 清 `pendingAddr`；`closeDevice()` 同样清 `pendingAddr` |
| M5 | `net_print.cpp` | 客户端发完数据不关连接、spool 仍有数据时，idle 超时判失败 → 重复打印 | 与 F2 合并修复：一律按正常完成收尾，等 spool 排空再关闭连接 |

### 低

| # | 位置 | 问题 | 修法 |
|---|---|---|---|
| L1 | `spool.cpp` | PSRAM 大块分配失败会直接退化到 32KB 内部 RAM | 改为连续阶梯：PSRAM 内 -25% → -12.5% → 减半 → -64KB → -32KB 探到 64KB，再按 128/96/80/64/48/32/24/16KB 分级落到内部 RAM |
| L2 | `*_status.bytesTotal` | 32 位累计在大作业下会回绕 | 改 `uint64_t`，JSON 用 `%llu` 输出 |
| L3 | 各 `message[96]` | 中文 UTF-8 在末尾被 `strlcpy` 截断可能出现半个字符 | 扩到 160 字节，长消息先用 `snprintf` 到局部缓冲 |
| L4 | `web_ui.cpp` | 扫描返回值负值处理 | 随 M3 修复 |
| L5 | `sysmon.cpp` | 多个任务并发写 `g_restartReason` 竞态 | 用 `s_restartMux` 临界区，getter 返回加锁拷贝 |
| L6 | `usb_printer.cpp` | `usb_host_install/client_register/transfer_alloc` 失败时只请求重启，但 monitorTask 尚未创建，重启不会执行 | 逆序释放已初始化资源并直接 `ESP.restart()` |

---

## 二、数据通路并发性（边 recv 边送 USB）

这是本次复核重点确认的部分，结论：**收发确实是并行的，大作业不会等整份收完才开始送 USB**。

| 环节 | 任务 / 优先级 | 是否阻塞对方 |
|---|---|---|
| TCP 9100 `recv()` 直写 PSRAM spool | `print9100` / 3 | 不等 USB。唯一暂停条件是水位 ≥90%（此时消费者仍在搬运） |
| spool → USB Bulk OUT | `spool-writer` / 4 | 不等网络。只等当前 4 KiB USB transfer 完成回调 |
| USB Host 事件 / transfer 回调 | `usb-client` / 4、`usb-daemon` / 5 | 独立任务，回调只置标志不改缓冲区 |
| Wi-Fi / Web / 看门狗 | `sysmon` / 2、Arduino `loopTask` / 1 | 与收发不共享锁路径 |

- **环形缓冲 SPSC**：`spoolWriteAcquire()` 返回的写入长度 = `min(空闲, 回绕前连续段, 4 KiB)`，写提交后消费者才可见、读提交后空间才可复用；索引操作全部在临界区内，双核安全。
- **背压不丢数据**：水位 90% 停收 → TCP 接收窗口自然收窄，客户端被流控减速；降到 70% 恢复收包。生产者不 `recv` 的这段时间，消费者持续把数据搬进 USB，所以 spool 水位会下降，不会死锁。
- **本次新增的吞吐优化**：生产者连续收包时不再每轮强制 `vTaskDelay(1)`（改为每 8 轮让出一次 CPU，避免饿死低优先级任务），这样 spool 能更快填充，消费者始终有数据可搬。
- **可验证指标**：Web 界面「PSRAM spool」卡片新增 **累计收 / 送** 两个数字。打印一个大作业时，两者应同时增长；spool 水位在 70%~90% 之间波动、背压次数增加，都属正常，说明流式通路在工作。
- **作业完成语义**：客户端 FIN 后不立即关闭连接，等 spool 完全排空才 `close()`，CUPS `waiteof=true` 才能正确等待；失败才用 RST 通知队列。

> 性能上限仍是硬件：ESP32-S3 的 USB-OTG 只有 Full Speed 12 Mbps（约 1.2 MB/s），大作业耗时由打印机端决定，不是 spool 或网络造成。

---

## 三、经查证「未发现问题」的项

1. `ARDUINO_USB_MODE=1` + `CDC/MSC/DFU_ON_BOOT=0`：core 3.3.0 的 `app_main()` 中所有 USB device 初始化都带 `&& !ARDUINO_USB_MODE` 条件；ELF 中只有 `usb_host_install`、无 `tusb_init`/`tud_task`，USB-OTG 完整留给 Host。
2. TWDT 配置：`esp_task_wdt_reconfigure()` 设 15s + `trigger_panic=true`，全部自建任务注册并周期喂狗；`CONFIG_FREERTOS_HZ=1000`，`vTaskDelay(1)` 是真阻塞，不会忙等饿死 Idle 任务。
3. USB 描述符解析：按 alt=0、class 0x07 / subclass 0x01、Bulk OUT 端点筛选，边界检查 `len < 2 || pos + len > total` 正确。
4. `SO_LINGER` RST、非阻塞 `recv`/`accept`、listener 绑定 `INADDR_ANY`（AP 与 STA 同时可打印）用法正确。
5. 分区表 / flash 尺寸：N16R8 官方板定义 `default_16MB.csv`，bootloader 头 `0x4f` = 16MB / 80MHz。
6. PSRAM 确实启用：ELF 中可查到 `esp_psram_init` / `ps_malloc` / `psramInit` / `spoolBegin` 符号。
7. HTML 内嵌资源无外网依赖；JSON 字符串转义处理了引号、反斜杠与控制字符。
8. `MAX_JOB_MS` / `MAX_JOB_BYTES` / 空闲超时构成兜底，异常客户端不会永久占用端口。
9. `platformio.ini` 的 `build_unflags` 正确移除了会抢占 native USB 的宏。

---

## 四、仍需注意的已知限制

- **未做实机打印验证**（无 M203d + N16R8 硬件）。编译、符号、镜像头与前端语法均已校验，首次上电请先看串口 `[spool] PSRAM spool ...` 与 `[usb] 打印机就绪 VID:03F0`，再打一张测试页。
- Wi-Fi 扫描保留在 loopTask（已改异步、且打印中禁止），与 monitorTask 的 `WiFi.status()/begin()` 仍存在轻量 API 级并发；`WiFi.begin()` 已收敛为单任务调用，风险可控。
- spool 是易失内存，掉电/重启会丢未送完的字节，需要客户端重发；这是 RAW 透传的固有语义。
- 单作业串行、无 IPP/AirPrint/SNMP/双向状态回传；添加 Windows 端口时务必关闭 SNMP 状态。
- `vTaskDelay(1)` 轮询 USB 完成回调每 4 KiB 最多引入 1 tick 延迟；这是为兼容回调上下文而做的稳健取舍，不是数据通路阻塞。

---

## 五、复核后的验证结果

| 项目 | 结果 |
|---|---|
| 编译 | PlatformIO 6.2.0 / pioarduino `espressif32 55.03.30` / Arduino-ESP32 3.3.0 / Xtensa GCC 14.2.0 → **SUCCESS**，Flash 1,060,707 B（16.2%）、RAM 48,416 B（14.8%） |
| 镜像 | `firmware/M203D_merged.bin` **1,126,640 B**，从 `0x0` 单文件烧录；bootloader 头 `0x4f` = 16MB / 80MHz |
| 完整性 | 四件 `.bin` 的 SHA256 均通过 `sha256sum -c firmware/SHA256SUMS.txt` |
| ELF 符号 | 存在 `esp_psram_init` / `ps_malloc` / `spoolBegin` / `spoolWriteAcquire`；**不存在** `tusb_init` / `tud_task` |
| 前端 | 内嵌 JavaScript 经 `node --check` 通过（3,454 字符） |
| 实机 | **未做**：无 M203d + N16R8 硬件，仍需测试页验证 |

---

## 六、v1.1.0 后续迭代（本节内容优先于上文）

上文是 **RAW 9100 版** 的复核结论，以下为后续实机部署与 IPP 改造后的更新：

### 实机发现并修复的缺陷

| 严重度 | 问题 | 触发条件 | 修复 |
|---|---|---|---|
| 中 | AP 名变成 `M203D-Print-000000` | `WiFi.mode(WIFI_AP_STA)` 后立即调 `WiFi.macAddress(mac)`，netif 未就绪返回全 0 | 改用 `esp_read_mac(mac, ESP_MAC_WIFI_STA)` 直接读 eFuse |
| 中 | 「扫描附近网络」恒报 `scan start failed` | `WIFI_SCAN_RUNNING=-1` / `WIFI_SCAN_FAILED=-2` 判断写反，且 Web 在 loopTask 里直接调 WiFi 扫描 API，与 monitorTask 并发 | 状态码改用宏；**扫描全部收敛到 monitorTask**，异步两段式 + 4 次重试 + 串口诊断；Web 只投递请求/取结果 |
| 高 | IPP 大作业会被客户端重发 | URF/PWG raster 经 USB Full Speed 传几十秒，若等 spool 排空才回 IPP 响应，客户端 HTTP 超时会重发整份文档 → 重复打印 | `netPrintRunJob` 增加 `JobEofFn`：文档收齐立即回 `successful-ok`（`job-state=processing`），spool 后台继续排空；已送出字节后绝不回错误码 |
| 中 | IPP 能力响应缓冲可能不够 | 声明 PWG/URF/1284 ID 等约 60 个属性，原 4KB 缓冲偏紧，溢出时会只发 HTTP 头让客户端挂住 | 缓冲扩到 8KB 且改为静态（不占任务栈）；溢出时重建最小 `server-error-internal-error` 响应 |

### 架构变化

- 主通路改为 **IPP over HTTP / TCP 631**，RAW 9100 默认关闭（`ENABLE_RAW_9100`）。
- `net_print.cpp` 抽象出 `netPrintRunJob(JobReadFn, ...)`，IPP 与 RAW 共用同一套 spool 背压 / USB / 看门狗 / 状态机；`s_busy` 改为临界区内原子 test-and-set。
- 新增 `ipp_print.cpp`：HTTP 头解析（Content-Length / chunked / Expect 100-continue）、IPP 属性区解析到 `0x03`、IPP 二进制响应构造、mDNS `_ipp._tcp` 能力声明。
- **免驱判定更正**：HP 官方规格列出 M203d 支持 `PCL5c; PCL6; PS; PCLm; PDF; URF; PWG`。因此 AirPrint 的 URF、IPP Everywhere 的 PWG Raster 都由打印机自己解释，固件只需拆封装透传，**不需要渲染**。上文「无 IPP/AirPrint」的结论已不成立。
- PSRAM spool 由写死 6MB 改为按实测可用量分配 `min(8MB, free-1MB)`，COM10 实测 **7,331,840 B（87.4%）**。

### 当前验证状态

| 项目 | 结果 |
|---|---|
| 编译 | v1.1.0 **SUCCESS**，Flash 1,074,907 B（16.4%）、RAM 59,240 B（18.1%） |
| 实机烧录 | COM10 已烧录，Hash 校验通过 |
| 启动日志 | spool 7,331,840 B / `_ipp._tcp:631` 发布 / IPP 631 监听 / 9100 关闭 / TWDT 15s，全部正常 |
| 扫描 | 逻辑已重构，**尚未在实机点击验证** |
| IPP 能力协商 / 真实打印 | **未验证**（打印机未接入 OTG 口）；请用 `python scripts/ipp_probe.py <IP>` 先测 |

---

## 七、1MB 给 LwIP/Wi-Fi 的硬保留（v1.1.0 追加）

### 为什么要「硬」保留

本工程编进的是 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` + `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`（已从工具链 `sdkconfig` 核对）。含义是：**内部 RAM 紧张时，Wi-Fi 的收发缓冲、LwIP 的 TCP pbuf（>4 KB 的分配）会直接落到 PSRAM**。spool 是开机一次性大块分配、永不释放，如果把它吃到贴着 8 MB，运行期 Wi-Fi 任务里一次 `malloc` 失败就是 abort/panic——用户看到的就是「打印到一半随机重启」。所以 1 MB 不能只是「算一算剩下的」，必须是可验证、可强制、可诊断的红线。

### 实现（三层）

| 层 | 位置 | 做什么 |
|---|---|---|
| 开机强校验 | `spool.cpp: spoolBegin()` | 分配前留 64 KB 边际（紧张时 8 KB）；分配后**实测** `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` 是否 ≥ 1 MB；不达标先按实测缺额精修（最多 3 次），精修不动才降一档，绝无腰斩 |
| 运行期守护 | `sysmon.cpp: psramGuardService()`（monitorTask 每 200 ms） | free < **288 KB** → `spoolForcePause(true)` 强制停收，收紧 TCP 窗口让 Wi-Fi 不再为新数据分配 pbuf；回升到 **384 KB** 才解除（滞回）；持续临界 **20 s** → 受控重启并记录 `psram exhausted` |
| OOM 诊断 | `sysmon.cpp: onAllocFailed()` | `heap_caps_register_failed_alloc_callback()` 注册钩子，用 ROM 级 printf 打出 `size / caps / 调用函数 / internal & psram 余量`；回调内**不分配任何内存**，避免在 OOM 路径上二次崩溃 |

### 修复的一个实现缺陷

第一版用「腰斩」策略处理红线不达标：实测 `reserve violated: left 1044024 < 1048576`（只差 **4,552 B**），结果 spool 被减半到 **3,665,920 B**，白丢 3.6 MB。根因是大块分配有对齐/簿记开销，`avail - reserve` 不能当成精确等式。改为「先留 64 KB 边际 + 按实测缺额精确缩容」后，一次重试即收敛：**7,192,576 B spool + 1,175,096 B 预留**。

### 实测证据（COM10，v1.1.0 阶梯版）

```
[spool] rung 1: 7266304 B breaks reserve by 4552 -> refine 7192576 B
[spool] PSRAM spool 7192576 bytes (85.7% of 8388608), Wi-Fi/LwIP reserve 1175096 bytes, ladder 2
[mem] internal heap total 341604, free 211692, largest 167924
[mem] DMA-capable free 203964, largest 167924
[mem] psram total 8388608, free 1162360, largest 1146868, alloc fails 0
```

Wi-Fi（AP+STA）+ mDNS + Web + IPP + USB Host 全部就绪后，PSRAM 仍余 **1,162,360 B**、最大连续块 **1,146,868 B**（几乎无碎片）、OOM 0 次 —— 红线真的守住了。

### 已知边界

- 打印机未接入 OTG 口，**「打印中」场景下的 PSRAM 曲线尚未实测**；若出现 `[guard] PSRAM critical` 日志，说明 Wi-Fi 突发占用超出预期，可把 `SPOOL_PSRAM_RESERVE` 提到 1.5 MB / 2 MB（spool 会相应变小）。
- `PSRAM_CRIT_HOLD_MS` 到达后会受控重启，**正在打印的作业会丢失**；这是「已经要 OOM」时的取舍，宁可有记录地重启，也不要 panic 无痕。

---

## 八、分配阶梯：绝不一步退回最小档（v1.1.0 追加）

### 问题

原实现在 PSRAM 分配失败时用 `want >>= 1`（腰斩），7.2MB → 3.6MB 一步就掉一半；一旦 PSRAM 全档失败，直接 `malloc(32KB)` 落进内部 RAM——中间没有任何过渡档，日志里也看不出到底退了几级。用户要求：**必须逐级减档，不能一下全部退回内部 RAM**。

### 现在的阶梯

`spool.cpp: nextRung()` 按当前档位算出「下一档」，只降一档，永不跳档：

| 当前容量 | 每次降幅 | 举例 |
|---|---|---|
| > 4 MB | −25% | 7,266,304 → 5,449,728 → 4,087,296 |
| 1 ~ 4 MB | −12.5% | 3,043,328 → 2,662,912 → 2,330,048 |
| 256 KB ~ 1 MB | 减半 | 917,504 → 458,752 → 229,376 |
| 128 ~ 256 KB | −64 KB | 229,376 → 163,840 → 98,304 |
| < 128 KB | −32 KB | 98,304 → 65,536（=`SPOOL_PSRAM_FLOOR_BYTES`，到此才放弃 PSRAM） |

只有 PSRAM 探到 64 KB 仍失败，才进入内部 RAM，且内部 RAM 自己也分 8 档：
**128 → 96 → 80 → 64 → 48 → 32 → 24 → 16 KB**，每档都要求「分配后仍留 64 KB」（`SPOOL_INTERNAL_KEEP_BYTES`）给 Wi-Fi / 任务栈，并用 `MALLOC_CAP_INTERNAL` 显式指定介质（默认 `malloc` 会把大块往 PSRAM 引）。

另外，红线不足时优先**精修**（只削差额 + 边际，最多 3 次），精修削不动才降一档——这样缺 4.5 KB 不会白丢几百 KB。

### 顺带修正一个事实错误

之前 config.h 注释写「ESP32-S3 内部 RAM 总共只有 320KB」，这是错的。实查 `ld/memory.ld`：`dram0_0_seg: org = 0x3FC88000, len = 0x53700 = 341,760 B`。也就是说 **片内 SRAM 总计 512 KB**，其中链接给应用当 DRAM 用的是 ≈334 KB，其余被 ROM 的 bss/data 与 cache 占掉。实机日志 `[mem] internal heap total 341604` 与这个数字吻合（PlatformIO 报表里的 `327680` 是板级 JSON 的估值，不是芯片总量）。全栈起来后实测空闲内部堆 211,692 B，所以内部 RAM 兜底只能给 KB 级档位。

### 日志怎么读

```
[spool] rung 1: 7266304 B breaks reserve by 4552 -> refine 7192576 B
[spool] PSRAM spool 7192576 bytes (85.7% of 8388608), Wi-Fi/LwIP reserve 1175096 bytes, ladder 2
```
`rung N` 是第几次尝试，`ladder` 是总共走了几档。若看到 `rung 3: ... alloc failed -> next ...`，说明前两档遇到碎片或空间不足，已经一级一级退下来了，而不是一脚踩到底。

---

## 九、IPP 服务与看门狗复核（v1.1.0，2026-11-11）

### 结论

- **没有发现无条件死循环**：所有 `for(;;)` 都有数据进展、超时或上限条件。
- 但有 **4 处可被客户端拖住的循环**（都喂狗，所以看门狗抓不到），已逐一加上限。
- 发现 **2 个真实逻辑缺陷**（一个让自恢复失效、一个会让客户端误以为打印成功），已修。
- 看门狗覆盖 6 个任务，**已实测** panic 后自动重启并正确记录 `reset=TASK_WDT`。

### 问题与修复

| # | 位置 | 问题 | 修法 |
|---|---|---|---|
| 1 | `sysmon.cpp` monitorTask | `ESP.getFreeHeap()` 是**含 PSRAM 的总堆**，PSRAM 还剩 1MB 时永远不低于 15KB 阈值 → 「低内存自动重启」实际从未生效 | 改用 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`，并额外检查最大连续块 < 4KB |
| 2 | `net_print.cpp` markEof | 失败路径也会调 `onEof`，于是 `total==0`（空作业超时 / 连接异常）也回 `successful-ok`：客户端以为印了，纸上是空的 | `markEof` 仅在 `ok || total>0` 时回成功；0 字节失败保持 replied=false，由上层回错误码（重发安全） |
| 3 | `net_print.cpp` 主循环 | 背压永不恢复（USB 消费端卡死）时，`last` 被不断重置 → 空闲超时永不触发 → 只能等 `MAX_JOB_MS` 20 分钟，期间 631 完全不可用 | 新增 `BACKPRESSURE_STALL_MS=120s`：背压连续 120s 直接判失败并释放 |
| 4 | `ipp_print.cpp` `rawLine` | 无整行总字节上限：客户端持续发不含 `\n` 的数据时，`rawFill` 每次都有数据、永不超时 → `for(;;)` 长期不退出且每轮喂狗 | 加 8 KiB 行上限，超限返回 -2 断开 |
| 5 | `ipp_print.cpp` `parseIppAttributes` | 每字节一次 `bodyRead(wait=true)`，每次都重开 30s 计时器 → 慢速滴灌最坏可占用数千秒（且喂狗） | `attrDeadline` 30s **整体**预算，所有读取收敛到 `rd8()` 统一检查 |
| 6 | `ipp_print.cpp` | `listen(fd, 2)` 太小；打印期间不 accept（上界 = 排空 spool ≈ 7 MB / 1 MB·s⁻¹ ≈ 7~8 s） | backlog 提到 8；`listen` 失败打印 errno |
| 7 | `ipp_print.cpp` | 打印机未就绪和「设备忙」都用 `0x0507`，语义混 | 未就绪改 `device-error(0x0504)`，忙保留 `busy(0x0507)` |
| 8 | `sysmon.cpp` `sysMonInit` | `reconfigure` 与 `init` 都失败时只打印一句就继续 → 等于没有看门狗 | 失败时 `deinit()` 后重建；仍失败则明确告警「自恢复不可用」 |

### 实测证据（COM10，v1.1.0）

启动自检（新增，确认 IPP 属性响应不溢出 8 KiB）：

```
[ipp] Get-Printer-Attributes 响应 2519 / 8192 B
[ipp] IPP 打印服务已监听 TCP 631 /ipp/print
```

看门狗自测（串口发 `wdt`，注入一个订阅 TWDT 但停止喂狗的任务）：

```
[selftest] 已订阅 TWDT 并停止喂狗，预计 15s 内 panic 重启
E (27391) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (27391) task_wdt:  - hangs (CPU 0/1)
E (27391) task_wdt: Aborting.
Rebooting...
rst:0xc (RTC_SW_CPU_RST),boot:0x28 (SPI_FAST_FLASH_BOOT)
[sysmon] TWDT 15s, boot #3, reset=TASK_WDT
```

受控重启（串口发 `reboot`）：

```
[sysmon] restart requested: serial cmd
rst:0xc (RTC_SW_CPU_RST),boot:0x28 (SPI_FAST_FLASH_BOOT)
[sysmon] TWDT 15s, boot #3, reset=SW
```

重启后 spool / AP / mDNS / IPP 631 / Web 80 全部正常恢复，`alloc fails 0`。

### 仍然是「设计取舍」而非缺陷的边界

- **打印期间不 accept**：IPP 任务在跑作业时串行处理，客户端此时发起的新连接会排在 backlog（8）里，作业结束后才被处理。上界是排空 spool 的时间（≈7~8s），而且响应已在文档收齐时提前发过，不会触发 HTTP 超时重发。真要并发查状态，需要把作业移到独立任务并给响应缓冲加锁——收益不抵复杂度，暂不做。
- **单作业串行**：第二个 Print-Job 在作业期间会收到 `busy`，由客户端重试，这是刻意的（两个作业交错进 USB 会打出乱页）。
- **`markEof` 提前回成功**：文档收齐即回 `successful-ok`，若之后 USB 阶段才失败，客户端不会重发（这是防「重复打印」的取舍），失败只能从串口/网页看到。

---

## 十、v1.2.0：移除 RAW 9100 + 负载优化

### 一、RAW 通路的完整拆除

RAW 9100 的能力为 0（没有作业边界、没有格式协商、没有错误回执），M203d 的免驱能力全部来自 IPP 631。它是纯粹的负担：一个监听 socket、一个 6 KB 栈的任务、一条与 IPP 共享作业泵的并发入口。拆除清单：

| 删除项 | 位置 |
|---|---|
| `ENABLE_RAW_9100` / `PRINT_PORT` 宏 | `config.h` |
| 监听 socket、`closeFd()`、`ensureListener()`、`rawRead()`、`handleRawJob()`、`printTask()` | `net_print.cpp`（约 100 行） |
| `print9100` 任务（6 KB 栈、优先级 3） | `net_printBegin()` |
| `NetPrintStatus::rawListening` / `ippListening` / `protocol` | `net_print.h` |
| mDNS `_pdl-datastream._tcp` | `wifi_manager.cpp` |
| `print.raw` JSON 字段、网页「协议 / RAW」标签 | `web_ui.cpp` |

`netPrintRunJob()` 保留（它是 IPP 的作业泵），`netPrintSetIppListening()` 简化为只置 `listening`。

**验证靠二进制而不是靠读代码**：`firmware.bin` 里已无 `pdl-datastream`、`rawListening`；`RAW` / `9100` 仅剩网页里那句给用户看的「本固件只开 631，已彻底移除 9100 RAW 端口」；`_ipp._tcp` / `/ipp/print` / `image/urf` 仍在。

### 二、四项负载优化

| # | 位置 | 原来 | 现在 | 收益 |
|---|---|---|---|---|
| 1 | `net_print.cpp` spoolWriterTask | 空闲也 `vTaskDelay(1)` = 1000 次/秒唤醒，一天 8640 万次 | 空闲 `ulTaskNotifyTake(pdTRUE, 100ms)`；生产者开工/清 spool 时 `xTaskNotifyGive` | 空闲唤醒降到 10 次/秒；作业启动还省掉最多 100 ms 空窗 |
| 2 | `ipp_print.cpp` ippTask | `accept` 失败 `delay(10ms)` = 100 次/秒空转 | `select()` 等 200 ms，有 SYN 立即返回 | 唤醒降到 5 次/秒，且**连接建立延迟更低**（不再最多等 10 ms） |
| 3 | `usb_printer.cpp` usbPrinterWrite | 等 transfer 完成用 `vTaskDelay(1)` 轮询，4 KB 传输约 4 次空转 | 完成回调 `xTaskNotifyGive(s_xferWaiter)`，5 ms 兜底 | 作业期间空转大幅减少，完成即刻进下一包 |
| 4 | `web_ui.cpp` handleStatus | `String` 反复 `+=`，一次请求几十次 `malloc/free` | 静态 `json[2048]` + `vsnprintf` 追加，溢出打告警 | 运行期零堆分配，消除内部 RAM 碎片主因 |

第 3 项的实现细节：`s_xferWaiter` 在 `usb_host_transfer_submit()` **之前**登记，避免完成回调早于登记而漏唤醒；提交失败与函数返回时都清空它。回调运行在 `usb-client` 任务上下文（非中断），因此可以直接调用 `xTaskNotifyGive`。spool-writer 自己的通知槽与这个是同一个——两者都只是「再看一眼」的信号，被吞掉最多多等一个轮询周期，不会丢状态。

### 三、代价

- 静态 JSON 缓冲占 2 KB `.bss`：RAM 59,272 → **61,312 B**（18.7%）。
- 删掉 `print9100` 任务省 6 KB **内部堆**，净 +4 KB，并少一个被调度器周期性选中的任务。
- Flash 1,079,159 → **1,075,727 B**（-3.4 KB）。

### 四、尚待实机复验

本轮改动编译通过（SUCCESS），但**板子当时不在线**（`USB-SERIAL CH340 (COM10)` 在设备管理器里为 `Unknown`，端口打不开），所以以下两项仍是待办：

1. 启动日志确认不再出现 `RAW 兼容端口已监听 TCP 9100`；
2. 串口 `stat` 确认任务数比 v1.1.0 少 1、`int free` 从 211,676 B 上升到约 218 KB。

---

## 十一、v1.3.0：双通路 + 环形缓冲 + 被动 mDNS（2026-11-11，已在 COM11 实机验证）

v1.2.0 的「只剩 IPP 一条通路 + 7.2 MB 整包缓存」被整体推翻，改为：

**9100 RAW + 631 IPP 双通路 / 256 KB 环形缓冲 / 16 KB USB 乒乓 / 被动 mDNS / SNMP 161 / AP 用完即关。**

### 一、改造中自己发现并修掉的缺陷

| # | 缺陷 | 后果 | 处理 |
|---|---|---|---|
| 1 | 乒乓 + 短包的数据错位 | 第一块只送出一部分时，第二块的起始位置仍按「第一块全送出」计算，设备收到的字节流中间缺一段 → 整页错乱 | `usbPrinterWait()` 发现 `actual < num_bytes` 立刻 `usbPrinterAbort()`（halt + flush + clear），整份作业判失败让客户端重发 |
| 2 | DMA 还在读就 commit | 原来的 `spoolReadCommit` 在 `usbPrinterWrite` 返回后调用，乒乓下第二块已经在飞，若此时 commit 让生产者覆盖同一块，DMA 会读到新数据 | 改成 acquire 时不 commit，`wait()` 回收后才按实际字节 commit |
| 3 | accept 与「跑作业」耦合 | 第一份作业在跑时，第二个连接只能堵在 listen backlog（占内存，客户端还以为连上了） | IPP / RAW 各自拆成「accept 任务 + 作业任务 + 深度 1 队列」；忙则 IPP 回 busy+Retry-After、RAW 直接关连接 |
| 4 | ESPmDNS 的周期广播关不掉 | TTL 硬编码 120s，每 60s 一轮 announce，Wi-Fi 空口与省电被无谓唤醒 | 自写 5353 responder，只应答查询 |
| 5 | AP 关闭时机过急会来回抖 | 刚连上就掉时，AP 反复开关，配网体验差 | STA 稳定 60s 才关；掉线满 3 分钟才重开 |
| 6 | 休眠唤醒后 USB 连不回来 | 只等 `NEW_DEV` 事件，而 Host 不一定重发；`close` 不 `free_all` 时设备还留在表里 | 无设备时每 2s 扫地址表主动重开；fault 持续 5s 强制 close + free_all 后重枚举 |

### 二、参数与实测

- 环形缓冲：4 × 65536 B = **262,144 B**（PSRAM），启动日志 `left 8,113,708 B for Wi-Fi/LwIP`；
- 水位：75% 停收 / 25% 复收；背压只停 `recv`，不丢数据；
- 启动后：`int free 147,968 / largest 104,436`、`psram free 8,098,756`、`alloc fails 0`、`tasks 21`；
- Get-Printer-Attributes 响应：双面关 2435 B、双面开 2528 B（8 KB 缓冲，余量充足）；
- 三条监听全部起来：`RAW 9100` / `IPP 631` / `SNMP UDP 161`。

内部堆从 v1.2.0 的约 218 KB 降到 148 KB，主要开销是：2 × 16 KB USB DMA 缓冲（32 KB）
与 5 个新任务栈（raw-job / raw9100 / ipp-job / snmp / mdns，合计约 29 KB 预留）。
这是「双通路 + SNMP + 自写 mDNS」的实价，仍在安全线以上（TWDT 的低堆阈值为 15 KB）。

### 三、lwIP 接收窗口：能改，但本机缺工具链（2026-11-11 20:4x 更正）

先更正本节早前的错误结论：**`custom_sdkconfig` 在 pioarduino 的 Arduino 框架下是生效的**。
实测加 `custom_sdkconfig = sdkconfig.defaults` 后，`arduino.py:890` 直接调用
`call_compile_libs()`，会把 Arduino 的 IDF 组件（含 `liblwip.a`）重新编译，因此窗口可以改。
之前写成「不生效」是因为键名用错了（`board_build.custom_sdkconfig` 是 board 级，
此处要用 env 级 `custom_sdkconfig`），且没有真正跑一次编译验证。

实际卡点：这条路径需要 **cmake + ninja**。本机 `packages/tool-cmake/bin` 与 `tool-ninja`
只有 `package.json`、无可执行文件，系统也未安装；`pio pkg install -t platformio/tool-cmake`
联网 12 分钟无任何输出（GitHub 下载被阻断），已中止并回退配置。

当前仍按默认 **5760 B** 运行。运行时能做的都做了：`TCP_NODELAY`、
`SO_RCVBUF=32768`（受 `TCP_WND` 上限约束）、8 KB 读块。5760 B 在 RTT 5 ms 下理论吞吐
≈1.15 MB/s，已高于 USB Full Speed 的 0.7–1.0 MB/s，局域网内不构成瓶颈。

以后若要做：补 cmake/ninja → 放回 `sdkconfig.defaults`（`CONFIG_LWIP_TCP_WND_DEFAULT=32768`）
→ 加 env 级 `custom_sdkconfig` → 全量重建 20–40 分钟。代码零改动，删两项即回退。

### 四、待实机复验（需要接上网络）

串口侧已验证启动与三路监听；以下需要连到设备的 AP 或局域网后执行：

1. `printf '%%!PS-Adobe-3.0\nshowpage\n' | nc <IP> 9100` —— RAW 作业能否出纸；
2. 作业进行中再发一个 IPP Print-Job —— 应立刻收到 `0x0507` + `Retry-After`；
3. `snmpget -v2c -c public <IP> 1.3.6.1.2.1.25.3.5.1.1.1` —— 空闲 3、打印中 4；
4. `dns-sd -B _ipp._tcp` —— 只在查询时有应答，空口无周期广播；
5. 打印机休眠 5 分钟再唤醒 —— 应自动重新枚举，无需重启固件。

### 五、复查（2026-11-11 20:0x）：又抓到 6 个

逐条核对需求时做的第二轮代码审查，发现并修复：

| # | 问题 | 影响 | 修复 |
|---|---|---|---|
| 1 | `s_resp`（8KB 响应缓冲）被 ippTask 与 ippJobTask 同时使用 | **打印期间 Windows 会轮询属性**，与作业早期回执同时写同一块缓冲 → 响应字节互相踩踏、属性乱码/客户端解析失败 | `sendIpp()` 全程持 `s_respMux`（互斥量，IPP 启动时创建） |
| 2 | `s_jobReply` 在 `handleConnection` 开头被重置 | 与 ippJobTask 的作业回执并发写，可能把 `sides`/`state` 覆盖成 `one-sided`/`processing`，回执内容错误 | 删除该重置；所有写入统一走 `setJobReply()`（`s_jobMux` 临界区），`fillJobAttrs` 读快照 |
| 3 | 作业尾部 UEL 依赖 `s_jobDrained` | 消费者可能在生产者写入**之前**就已置位（它看到缓冲是空的），导致提前收尾、UEL 丢失 | 改为盯物理条件：`spoolUsed()==0 && usbPrinterPending()==0` |
| 4 | 作业结束后 `job-state` 不更新 | 后续 `Get-Job-Attributes` 永远报 `processing(5)`，失败作业也不报 aborted | 回执后按结果写最终态：成功 9 / 失败 8 + `aborted-by-system` |
| 5 | 「清除 Wi-Fi 配置」后 AP 不立刻回来 | 若 AP 已被自动关闭，设备会失联整整 3 分钟 | 凭据为空（`ssid[0]=='\0'`）时立即重开 AP |
| 6 | mDNS 无可用 IP 时仍应答 A=0.0.0.0 | 客户端会记住一个无效地址 | `haveIpv4()` 为假时直接不应答 |

另外三处小的：SNMP 丢弃 v3 报文（结构不同，硬解会错）；USB submit 失败时清空 `s_xferWaiter`（避免唤醒悬空句柄）；`printer-state` 与 `printer-state-reasons` 改为一次取值（避免「idle + media-empty」自相矛盾）。

**一次自己制造的险情**（记录在案）：复查时误判 `jAppend(o, ",\"cfg\":{\"duplex\":%s}")` 缺少 `}`，改成 `%s}}` —— 实际原串末尾的 `}` 就是 JSON 闭合，`printf` 不转义 `}`，改完反而多一个闭合括号，会让 `/api/status` 变成非法 JSON。已改回，并用脚本把 `handleStatus()` 里所有 `jAppend/jStr` 按顺序拼出样例串做 `json.loads()` 校验（braces 7/7，结构 OK）。

编译结果：Flash 1,060,991 B（16.2%）、RAM 63,152 B（19.3%），无警告；已烧录 COM11 并联调启动日志（三路监听、ring 262144 B、`alloc fails 0`）。

### 六、RAW 卡死的真因（2026-11-11 21:xx，实机定位）

用户报告「RAW 收到 8382 B 但 usb.bytes=0、busy 永不复位、IPP 报设置无效」。实测复现并定位到**两个独立原因**：

**原因 1（固件缺陷，我引入的）：乒乓填充重复 acquire**
`spoolReadAcquire()` 不推进读指针，双缓冲填充循环第二次调用时拿到的是**第一段的同一块**。
同一块被 submit 两次、`spoolReadCommit` 也执行两次 → `s_r` 跑到 `s_w` 前面 → `used` 无符号下溢。
报告里 `used = -10` 正好是 UEL 的 10 字节，与这个机制完全吻合。
修复：新增 `spoolReadAcquireFrom(skip)`，按「已提交未 commit」的字节数偏移；commit 增加越界钳制。

**原因 2（环境）：打印机缺纸**
用 USB Printer Class 的 `GET_PORT_STATUS`（控制传输，不经过 bulk OUT）读到：

```
[usb] PORT_STATUS=0x28  not_error=1 selected=0 paper_empty=1
[usb] IN 探测：完成 status=0 actual=0
```

IN 端点能完成、控制请求能通，唯独 bulk OUT 永久 NAK —— 缺纸时引擎停摆，打印机不收数据。
这不是固件能修的，但固件**必须能识别并如实上报**，否则表现就是「卡 8 秒然后报 USB 断开」。

**连带修复**
| 项 | 处理 |
|---|---|
| 超时后重启整机 | 改为复位端点（halt+flush+clear）、作业失败、不重启 —— 否则打印机还在睡时会陷入重启循环 |
| 数据收完但 USB 不动 | 60s 无进展判失败，`busy` 复位 |
| 缺纸不可见 | 每 2s 轮询 GET_PORT_STATUS；`ippPrinterState()` 缺纸报 5/stopped，reasons=media-empty，accepting=0；SNMP hrDeviceStatus=3 + noPaper 位；Web 显示「缺纸」 |
| 缺纸时仍收作业 | IPP 回 device-error「打印机缺纸」；RAW 直接关连接，不再等 8s 超时 |
| `print.bytes` 虚增 | 原来把「收到的字节」也加进 bytesTotal，改为只统计真正写入 USB 的字节 |
| 卡死无逃生口 | 新增 `POST /api/abort` |

**实机验证（缺纸状态）**：`printer-state=5`、`printer-state-reasons=media-empty`、`printer-is-accepting-jobs=0`、Web「纸张：缺纸」、RAW 作业 0 秒拒绝。装纸后应直接恢复。

---

## 七、v1.4.0：通用型固件 + 机型自动识别（2026-11-12）

把「HP M203d 专用」改造成「任意 USB Printer Class 打印机」的通用固件。**本轮只编译，未刷入设备**。

### 一、自动识别链路

新增 `src/printer_identity.{h,cpp}`，并在 USB 层新增 `GET_DEVICE_ID`（`bRequest=0x00`）异步控制请求：

```
上电 → USB 枚举成功 → GET_DEVICE_ID (1024 B)
     → 解析 MFG / MDL / CMD / DES / SN
     → 推导 PDL MIME 列表（PDF 优先）、URF 支持、彩色支持
     → 发布给 IPP / mDNS / SNMP / Web
读不到或 3s 无响应 → 回退 VID:PID + 保守能力集（页面标注「能力为推定值」）
```

CMD 逐 token 映射（避免 `EPSON` 被误判成 `PS`）：`PDF→application/pdf`、`URF→image/urf`、`PWG→image/pwg-raster`、`PCLm/PCLXL/PCL6/PCL5/PCL→vnd.hp-*`、`PS/POSTSCRIPT→application/postscript`、`JPEG/PNG→image/*`。`ESCPOS/ZPL/TSPL` 只保留在页面上，不注入 IPP 的 document-format 列表。

### 二、去机型化的字段

| 位置 | 原来写死 | 现在 |
|---|---|---|
| IPP | `printer-name/info/make-and-model/device-id` 全是 HP M203d | 来自 1284 Device ID |
| IPP | 固定 9 项 `document-format-supported`，PDF 第一 | 由 CMD 推导；PDF 仅在确认支持时提前；不谎报 URF/PWG |
| IPP | 固定 `color-supported=false` | 由 Device ID 的颜色声明决定 |
| IPP | 无条件 `urf-supported` + `airprint-1.7` | 仅在设备声明 URF 时上报 |
| mDNS | 实例名 `M203D-xxxxxx`、TXT 的 `ty/product/pdl/URF` | 实例名 = 识别出的型号 + MAC；每次应答前刷新（识别晚于网络启动） |
| SNMP | `sysDescr` / `prtGeneralPrinterName` 写死 | `printerIdInfo()` / `printerIdModel()`，序列号取 SN |
| 全局 | `FW_HOSTNAME=m203d-print`、`AP_SSID_PREFIX=M203D-Print-` | `printserver` / `PrintSrv-`；启动横幅、Server 头、信息页通用化 |

### 三、WebUI 重构

单页重写：顶部状态徽章（就绪 / 打印中 / 缺纸 / 故障 / 未连接）+ 自动识别出的型号；六张卡片（打印机身份、网络、打印服务、环形缓冲、打印选项、系统）+ 添加打印机说明。

- 打印机身份卡显示厂商 / 型号 / 发布名称 / 序列号 / VID:PID / 接口 / 端点 / 纸张 / URF / 彩色，并列出 CMD 与推导出的 MIME 芯片、可展开的 1284 原文；
- 环形缓冲改为可视化水位条（≥75% 变色）；
- 三通路指示（RAW/IPP/SNMP）、上次作业、双面开关、中止作业、清除配置、重启都有按钮；
- 添加打印机的命令行按识别出的名称与当前 IP 动态生成；
- 所有机型文案从 `/api/status` 的 `printer` 对象读取，页面本身不含任何品牌。

配套：`/api/status` 增加 `printer` 对象（含 512 B Device ID），JSON 静态缓冲 2560→4096 B，`jStr` 临时缓冲 200→640 B 以免长字段被截断。

### 四、验证（未刷入）

- `pio run` SUCCESS：RAM 20.3%（66,360 B）、Flash 16.4%（1,072,375 B）；
- `/api/status` 拼接做静态 `json.loads()` 校验：braces 8/8，结构 OK；
- 前端脚本用 `node --check` 校验语法 OK；
- **按用户要求，未调用任何 esptool write-flash**，设备仍运行上一版固件。

---

## 八、自动双面：识别 + 真正生效（2026-11-12，v1.4.1 待刷入）

用户反馈「现在烧录进硬件的固件无法识别自动双面打印机，无法支持自动双面打印」。定位为**两个独立缺陷**：

**缺陷 1：能力识别缺失。**
硬件上跑的是 v1.3.1（v1.4.0 只编译未刷），双面完全是一个手动布尔开关，默认值关；v1.4.0 加了机型识别，但**没有检测双面能力**。所以固件从头到尾都不知道打印机有没有双面单元。

**缺陷 2（更关键）：`sides` 属性被拆封装丢掉。**
IPP 的 `sides` 是**作业属性**，不在文档字节流里。固件只把 0x03 之后的文档字节透传给打印机，即使 Web 开关打开、IPP 报了 `two-sided-*`，客户端选双面也只会出单面纸。这不是「上报开关」能解决的，必须做协议桥接。

### 修复

| 层 | 改动 |
|---|---|
| `printer_identity` | 新增 `printerIdSupportsDuplex()` / `printerIdDuplexEvidence()`：按证据强度依次查 1284 `DUPLEX` 关键字、`CAP` 字段、`CMD` 字段、型号后缀 `d`（HP/Brother 表示 duplex 的弱证据）。另加 `printerIdSupportsPjl()`：只有 PDL 含 PCL/PS/PDF/URF/PWG 才允许注入 PJL，ESC/POS、ZPL 等标签语言自动禁用 |
| `printer_cfg` | 双面开关从 bool 改成三态 `auto / on / off`（NVS `duplex_mode`），`cfgDuplexEnabled()` 返回有效值；旧 `duplex` 布尔键自动迁移为强制开/关；新增 `pjl_duplex` 开关（默认开） |
| `ipp_print` | `IppCtx` 新增 `pjlPrefix/pjlLen/pjlSent` 与 `sidesPresent`；`ippDocumentRead()` 先把 PJL 头吐进缓冲再读文档；`handlePrintJob()` 在「需要双面且设备支持且允许注入」时写入 `ESC%-12345X @PJL SET DUPLEX=ON / SET BINDING=LONGEDGE|SHORTEDGE ESC%-12345X` |
| 默认双面 | 客户端没显式带 `sides` 时按 `sides-default` 走（报了默认双面就得真双面），并同步回执里的 `sides` 值，避免「按双面打、回执报 one-sided」 |
| `web_ui` | `/api/duplex` 支持 `mode=auto/on/off` 与 `pjl=1/0`（兼容旧 `on=1/0`）；`/api/status` 的 `cfg` 增加 `duplex_mode / duplex_detected / duplex_evidence / pjl_duplex`；页面改为三态按钮 + 注入开关 + 识别依据展示 |
| RAW 9100 | **不注入**——原厂驱动会自己发 PJL/PCL 双面命令，注入反而可能重复 |

边界说明：这是**作业控制指令注入**，不是页面格式转换（不解析、不渲染 PDL），没有违反既定的透传原则。自动识别是启发式的，识别不准时用户可在页面切「强制开」；更可靠的 PJL `INFO CONFIG` 主动查询需要双向状态通道，当前未实现。

### 验证

编译 SUCCESS：RAM 20.3%（66,600 B）、Flash 16.5%（1,078,367 B）；`/api/status` 静态 JSON 校验 braces 8/8 OK；前端 `node --check` OK。**按上一轮指令仍未刷入**，需用户确认后再烧。

---

## 八、v1.4.2：刷入实机后暴露的识别回归（2026-11-12 09:3x–09:5x）

用户要求刷入 v1.4.1（此前只编译未刷）。**只写 app 分区 `0x10000`**，NVS 里的 Wi-Fi 与双面配置完整保留，设备自动连回 `192.168.10.43`。刷入后实机状态立刻暴露两个「去机型化」引入的回归：

### 刷入 v1.4.1 后的实测

```
[id] 自动识别：HP / HP LaserJet M203-M206 / CMD=PJL,PML,PCLXL,URP,PCL,PDF,POSTSCRIPT
[id] PDL=application/pdf,application/vnd.hp-PCLXL,application/vnd.hp-PCL,application/postscript,application/octet-stream
cfg.duplex_detected = false  （"未检测到双面单元"）
printer.urf         = false
```

真实 1284 Device ID：
```
MFG:HP;CMD:PJL,PML,PCLXL,URP,PCL,PDF,POSTSCRIPT;MDL:HP LaserJet M203-M206;
CLS:PRINTER;DES:HP LaserJet M203d;...;CID:HPLJPDLV1;IPP-E:FF-04-01,...;MCL:DL;MCV:3.1;
```

### 根因与修复

| # | 问题 | 根因 | 修复 |
|---|---|---|---|
| 1 | **双面识别漏判** | `detectDuplexAndPjl()` 只看 `MDL` 末段后缀，而 HP 的 `MDL` 是系列名 `M203-M206`（末位 `6`）；真正的销售型号 `M203d` 在 `DES`。`kDes` 甚至已定义却被 `(void)kDes` 丢弃 | 提取 `DES`；后缀判定改为同时检查 `DES`/`MDL`，并支持 `d/dn/dw/dtn/fdw`（从末尾跳过 `n/w/t/x/s` 再看是否停在 `d`） |
| 2 | **AirPrint 能力丢失** | HP 的 CMD 写的是 `URP` 不是 `URF`；且 `IPP-E:` 字段（IPP Everywhere 声明）完全没被利用。结果 `urf=false` → IPP 不报 `airprint-1.7`/`urf-supported`/PWG Raster，mDNS 不发 URF 键，iOS/macOS 会把原本免驱的机器剔除 | `URP` 作为 URF 别名；`IPP-E:` 命中时补 `image/pwg-raster` + `image/urf` 并置 `urf=true` |
| 3 | `make_model = "HP HP LaserJet ..."` | `MDL` 已含 `HP`，又拼了一次 `MFG` | 前缀重复时不再拼接 |
| 4 | `print.bytes` 恒为 0 | `NetPrintStatus.bytesTotal` **从未被写入** —— 注释误以为由 `usb_printer` 累加，但那是 `UsbPrinterStatus.bytesTotal`（另一个结构体） | 由消费者在 `spoolReadCommit` 成功时累加，语义 = 真正写入 USB 的字节 |
| 5 | 每次启动 `[E] lastwdt NOT_FOUND` | `getString("lastwdt")` 读不存在的键 | `p.isKey("lastwdt")` 先判存在 |

**一次自己制造的误判（记录）**：曾认为 `ipp_print.cpp` 的 `inject` 条件漏了 `printerIdSupportsPjl()`，实际 `cfgPjlDuplexEnabled()` 内部已包含该判断，加检查纯属重复，已撤回。

### 用户澄清后的定案

用户说明实际走的是 **9100 RAW**、机器是 HP M203d 且支持双面。经确认选择：

- **RAW 保持零注入**：RAW 没有 `sides` 属性协商，双面指令必须由客户端驱动写进数据流（PJL/PCL），固件纯透传。IPP 侧的 `sides → PJL` 注入对 RAW 不生效，这是刻意设计（避免与原厂驱动重复）。README 新增「RAW 9100 上的双面打印」配置章节。
- **发布名称改用 `DES`**：新增 `pubModel`（DES 优先、回退 MDL），`printerIdModel()` 返回发布型号，新增 `printerIdMdl()` 返回原始 MDL 供 Web 身份卡显示。IPP `printer-name`、mDNS 实例名、TXT `ty/product`、SNMP、Web 标题统一变成 `HP LaserJet M203d`。

### 实机验证（v1.4.2，已刷入 COM11）

```
[id] DES=HP LaserJet M203d
[id] 发布名称：HP-LaserJet-M203d / HP LaserJet M203d
[id] 双面=是（1284 DES 型号后缀 d（推定双面）） URF=1 PJL=1
IPP: printer-name=HP-LaserJet-M203d, sides-supported=one-sided,two-sided-long-edge,two-sided-short-edge,
     sides-default=two-sided-long-edge, urf-supported=...DM3, ipp-features=ipp-everywhere,airprint-1.7
mDNS: 实例名 HP-LaserJet-M203d-560FB0, ty=HP LaserJet M203d, product=(HP LaserJet M203d), duplex=T
RAW 9100 探针：112 B 全部送入 USB，ok=true，零注入，spool w=122 r=122（无下溢）
IPP 双面探针：注入 PJL 头，ok=true，spool w=175 r=175，print.bytes 正确累加
启动日志无 [E] NOT_FOUND；纸张状态正常（装纸后 paper_empty=false）
```

编译：RAM 20.7%（67,784 B）、Flash 16.5%（1,079,323 B），无警告；JSON 静态 `json.loads` OK、前端 `node --check` OK。

---

## 九、v1.4.2 实机全面复验 → 又抓到 2 个（v1.4.4，2026-11-12 10:1x–10:4x）

用户要求「再实机校验下固件 v1.4.2」。写了一份 31 项自动验收脚本（`scripts/verify_firmware.py`），首轮 31/31 通过，但**重复运行时 mDNS 偶发 0 B**，顺藤摸瓜查出一个严重缺陷；期间用户又反馈「每次打印都多出来一张空白页」。

### 缺陷 1：每次打印多一张空白页（用户报告）

`net_print.cpp` 正常收尾注入的尾部是
```c
static const uint8_t kTail[] = {ESC,'%','-','1','2','3','4','5','X', 0x0C};  // UEL + FF
```
`UEL` 已经让打印机退出 PDL / 结束作业、页缓冲为空，紧跟的 **FF(0x0C) 强制走纸** → 吐出一张空白页。客户端驱动生成的流末尾本来就带 UEL + EOJ，固件再补 FF 纯属多余。

**修复**：`kTail` 去掉 `0x0C`，只留 9 字节 UEL（幂等、仍能保证作业边界，不产生空白页）。`config.h` 注释与 Web「作业尾标记」文案同步改为「仅 UEL（无 FF）」。
**实测证据**：RAW 探针文档 92 B，修复前 `spool reset (w=102 r=102)`（+10 = UEL+FF），修复后 `w=101`（+9 = 仅 UEL）；IPP 作业 `163 → 172` 同样是 +9。

### 缺陷 2：标准 5353 源端口 mDNS 查询被完全忽略（AirPrint 发现不了）

`mdns_responder.cpp` 的收包循环里有一条
```c
if (ntohs(from.sin_port) == 5353) continue;   // 注释：其它 responder 的包：忽略
```
本意是丢弃网络上其它 responder 的组播包，但 **Bonjour / Avahi / dns-sd / iOS·macOS AirPrint 的标准查询恰恰就是从 5353 源端口发出的**。对照实验：随机源端口查询设备应答 6/8，标准 5353 源端口 **0/8**。也就是说固件虽然上报 `airprint-1.7` / `URF`，却根本不会被 iPhone/Mac 发现。

**修复**：删掉端口过滤。区分「别人的响应」交给 `handleQuery` 里的 QR 位（`b[2] & 0x80`）；别人的查询 QR=0 但服务名不匹配，`answerOne()` 直接返回 false 不应答；本 responder 只发单播，也不会收到自己的包。另加一条限频日志 `[mdns] 应答标准 5353 查询 q=... bytes=...`，便于长期排错。

**验证的坑**：Windows 本机 UDP 5353 被 `msedge`(PID 276) 与 `WeChatAppEx`(PID 26744) 占用，设备的单播应答会被这些进程截走，客户端 socket 收到 0 B 并不代表设备没应答；原始套接字抓包又需要管理员权限（`WinError 10013`）。最终以**设备侧串口日志**为确凿证据，验收脚本相应改为检查日志而不是客户端收包。

### 验收脚本固化

`scripts/verify_firmware.py`（支持 `--host` / `--serial`）覆盖 34 项：状态接口与识别、IPP 属性、RAW 纯透传、IPP 双面 PJL 注入、单作业并发 busy + Retry-After、SNMP v2c GET、被动 mDNS（随机端口与标准 5353）、spool 指针一致性、尾部无 FF、串口异常扫描。组播查询在 Wi-Fi 上会偶发丢包，脚本用「同一 socket 重发 4 次 + 按设备 IP 过滤」模拟 Bonjour 行为，避免误报。

**最终实机结果：34 / 34 PASS**（v1.4.4，COM11，192.168.10.43），启动日志异常行 0；`stat` → tasks 21 / int free 142,344 / psram free 8,095,536 / alloc fails 0；Web 页 18,414 B 新 UI 元素齐全；Windows 打印默认仍为 `TwoSidedLongEdge`。

---

## 十、v1.5.0：debug / release 双版本固件 + WebUI 文案校订（2026-11-12 11:1x–11:4x）

### 1. 双版本固件（用户要求「正式固件去掉观测点」）

新增 `src/log.h`，把 95 处 `Serial.printf/println` 按语义分三级：

| 宏 | release | debug | 用途 |
|---|---|---|---|
| `FWLOGD` | 编译为空 | 输出 | 观测点：逐请求（IPP op）、逐作业字节/耗时、`PORT_STATUS` 轮询、USB 心跳、`spool reset` 指针、内存快照、自检 |
| `FWLOGI` | 输出 | 输出 | 事件：启动横幅、三路监听、Wi-Fi/USB 连断、配置变更、`stat` 命令 |
| `FWLOGE` | 输出 | 输出 | 异常：初始化失败、缓冲溢出、传输失败、作业被拒绝、重启原因 |

最终分布 **FWLOGD 30 / FWLOGI 30 / FWLOGE 35**。release 下 `FWLOGD` 定义为 `do { if (false) Serial.printf(...); } while (0)` —— 参数仍参与编译期检查（不会产生 unused 变量警告），GCC 把整段不可达分支连同字符串常量优化掉。

`platformio.ini` 拆成 `[env]` + `[env:release]`（`FW_DEBUG=0`、`CORE_DEBUG_LEVEL=1`）+ `[env:debug]`（`FW_DEBUG=1`、`CORE_DEBUG_LEVEL=4`），`default_envs = release`。构建/烧录脚本重写：`scripts/build.sh|bat [all|release|debug]`、`scripts/flash.sh|bat [端口] [版本]`，支持 `APP_ONLY=1` 只写 app 分区保留 NVS。

**实测**：release 启动日志 **1,378 B / 15 行**（debug 版 2,261 B / 22 行，减少 39%），异常 0；release Flash 1,079,184 B、debug 1,086,656 B，差 7.4 KB 全是日志字符串；两版均零警告。

### 2. WebUI 描述校订（用户问「红框里描述是否错误」+ 要求通俗易懂）

**红框「设备类型选 Generic Network Card」本身是对的**（它让 Windows 不做 SNMP 探测，正好绕开本机简化 SNMP），但原文没说明原因、也没给出下拉框只有 `TCP/IP Device` 时的替代做法。其余问题：

| # | 问题 | 修正 |
|---|---|---|
| 1 | RAW 段落只写死 `Generic Network Card` | 补充理由 + 「若只有 TCP/IP Device，就取消勾选『启用 SNMP 状态』」 |
| 2 | IPP 段落的命令行只有 `Add-Printer` | **缺 `Add-PrinterPort`，直接执行会失败**；改为两行，先建 IPP 端口再加打印机 |
| 3 | SNMP 指示灯绑定 `p.ipp \|\| p.raw` | SNMP 是独立服务，却拿打印服务状态代替 → `/api/status` 新增 `snmp:{listening,port}`，`snmp_agent` 导出 `snmpAgentListening()`，指示灯读真实 bind 状态 |
| 4 | SNMP 描述只说「只读」 | 补明「仅 GET/GETNEXT，不支持 SET」 |
| 5 | 双面卡「sides → PJL」术语 | 改「自动补发双面指令」，正文用大白话解释「电脑只告诉本机要双面，不会写进内容，所以要额外补一条打印机认得的命令」 |
| 6 | 技术标签（MDL/DES、VID:PID、OUT 端点、URF、PSRAM、环形缓冲、背压） | 换成「型号 / 完整型号 / USB 编号 / 数据通道 / iPhone·Mac 免驱 / 剩余扩展内存 / 数据缓冲区 / 因来不及处理而暂停接收」，术语放进括号或可展开区 |
| 7 | 「作业尾标记 UEL + FF」 | 改「只收尾，不多吐白纸」（与 v1.4.4 的 FF 修复一致） |
| 8 | `duplex_evidence` 后端文案技术化 | 「1284 DES 型号后缀 d（推定双面）」→「型号以 d 结尾，通常代表双面机型（DES）」等 |

页面标题也从「打印机身份 / 添加打印机 / 系统」改为「这台打印机 / 怎么在电脑上添加 / 设备状态」。

### 3. 验收脚本适配双版本

`scripts/verify_firmware.py` 新增 `--debug`：协议层 30 项两版都跑；依赖串口日志的 4 项只在 debug 下断言，release 下如实标 **SKIP**（不是 FAIL）。「作业尾部仅 UEL」改用 `print.bytes` 差值判断（92 B 文档应 +101 = +9），release 也能验空白页修复。mDNS 探测改「同一 socket 重发 6 次 + 整轮重试一次」，规避 Wi-Fi 组播抖动误报。

**实测**：release 模式 **30/30 通过**（4 项 SKIP），新 WebUI 16 项文案全部命中、旧文案 0 残留。
