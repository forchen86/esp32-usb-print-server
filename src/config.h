#pragma once
// ============================================================================
// 全部可配置项都在这里。改完重新编译即可，无需改其它文件。
// ============================================================================
#include <stdint.h>
#include <stddef.h>

// 日志分级（FWLOGD / FWLOGI / FWLOGE）——所有 .cpp 都包含本头文件，统一在此引入
#include "log.h"

#define FW_VERSION              "1.5.1"   // 双版本固件：release 去除观测点 / debug 全量日志
#define FW_HOSTNAME             "printserver"   // mDNS: http://printserver.local

// --- AP 直连热点（配网完成后自动关闭，掉线 3 分钟重开）----------------------
#define AP_SSID_PREFIX          "PrintSrv-"     // 后面自动追加 MAC 后 3 字节
#define AP_PASSWORD             ""              // 留空 = 开放；需要加密就填 8~63 位

// NVS 命名空间（通用命名）。旧命名空间保留为迁移源，升级不丢已存的 Wi-Fi / 双面配置。
#define NVS_NS_WIFI             "printsrv-wifi"
#define NVS_NS_CFG              "printsrv-cfg"
#define NVS_NS_WIFI_OLD         "m203d-wifi"
#define NVS_NS_CFG_OLD          "m203d-cfg"
#define AP_CHANNEL              1
#define AP_MAX_CLIENTS          4               // AP 固定地址 192.168.4.1

// AP 配网后自动关闭（省电 + 降空口干扰），但必须留足回退空间：
//   · STA 连续在线满 60s 才关 —— 刚连上就掉的情况不会反复开关；
//   · STA 掉线满 3 分钟才重开 —— 短暂掉线由指数退避重连处理，不惊动用户。
#define AP_CLOSE_AFTER_STA_MS   60000UL
#define AP_REOPEN_AFTER_DOWN_MS 180000UL

// --- 打印协议 ---------------------------------------------------------------
// 两条通路并存，互不干扰：
//   9100  RAW / PDL-Datastream：默认通路，无封装、无协商，一个 TCP 连接就是一份
//         作业（close = EOF），最省 RAM/CPU，Windows「TCP/IP 端口 + Raw」直连。
//   631   IPP：AirPrint / Windows IPP Class Driver 用；同时是 SNMP 状态的唯一
//         数据源（printer-state）。两个监听任务分开，不复用任何 socket 或缓冲区。
// 两边共用同一个作业泵与同一个环形缓冲，任一时刻只允许一份作业，第二个请求回忙。
#define PRINT_PORT              9100
#define IPP_PORT                631
#define IPP_URI_PATH            "/ipp/print"

// --- 打印选项：自动双面 ------------------------------------------------------
// 三态策略（Web 可改、存 NVS、掉电保持）：
//   auto(0) → 跟随 printer_identity 从 1284 Device ID 识别出的双面能力
//   on(1)   → 强制上报双面（识别不准时手动覆盖）
//   off(2)  → 强制只报单面
// 有效值为「开」时：sides-supported 含 two-sided-*、sides-default=双面长边、
// mDNS duplex=T、URF DM3；IPP 收到 sides=two-sided-* 时还会在文档前注入
// PJL 双面作业头（SET DUPLEX=ON / SET BINDING），否则纯透传会把这个作业属性
// 丢掉，客户端选了双面也只出单面纸。
// RAW 通路不注入 PJL —— 原厂驱动自己会发双面命令。
#define DUPLEX_DEFAULT_MODE     0               // 0=自动识别 1=强制开 2=强制关
#define PJL_DUPLEX_DEFAULT      1               // 默认开启 sides → PJL 注入

// --- USB 传输 ---------------------------------------------------------------
// 16KB 单块 + 2 个 transfer 轮转（乒乓）：一个在飞时另一个已经排好队，
// 避免「提交 → 等完成 → 再提交」之间 USB 总线空转。
// 注意：每块 16KB 由 usb_host_transfer_alloc 分配在内部 DMA RAM 上，2 块共 32KB。
#define USB_XFER_SIZE           (16UL * 1024UL)
#define USB_XFER_SLOTS          2
#define USB_XFER_TIMEOUT_MS     8000
#define NET_READ_CHUNK          (8UL * 1024UL)  // 每次从 socket 读 8KB（贴近 lwIP 窗口）
#define CLIENT_IDLE_TIMEOUT_MS  60000           // 60s 无新数据即按完成/中止收尾
#define BACKPRESSURE_STALL_MS   120000UL        // 背压连续不恢复这么久 = 消费端卡死
#define MAX_JOB_MS              (20UL * 60UL * 1000UL)
#define MAX_JOB_BYTES           (32UL * 1024UL * 1024UL)

// 作业正常结束后只追加 UEL(ESC%-12345X)，不加 FF。
// 目的：把打印机从上一份 PDL 的格式状态里摘出来，防止下一份作业第一页串页。
// v1.4.4 起去掉 FF(0x0C)：UEL 已结束作业、页缓冲为空，再送 FF 会强制走纸
// 吐出一张空白页。客户端驱动自己生成的流末尾通常已带 UEL，重复一个幂等无害。
// 设 0 可完全关闭尾部注入。
#define JOB_TAIL_UEL            1

// --- 打印语言签名门卫 --------------------------------------------------------
// 作业开头的 1KB 必须能认出一种已知打印语言（PJL/UEL、PCL5/ESC、PCLXL、
// PostScript、ZPL、PDF、URF、PWG、JPEG、PNG），认不出来 = 大概率是局域网里
// 其它设备发来的垃圾数据（「偶发乱码白纸」的最常见来源），直接拒绝、
// 一个字节都不上 USB。注意：纯文本作业没有打印语言签名，也会被拒；
// 需要打印纯文本请走驱动。设 0 关闭门卫。
#define PDL_SIGNATURE_GATE      1
#define PDL_PROBE_BYTES         1024

// --- 环形缓冲（小缓存 + 双水位背压）-----------------------------------------
// 不再整包缓存：4 个固定 64KB 块组成 256KB 环，覆盖「一次 USB 传输的突发」即可。
// 设计要点（务必保持）：
//   1) 固定块池，开机一次性 ps_malloc，运行期零 malloc/free —— alloc_fails 恒为 0；
//   2) 高水位 75% 停收、低水位 25% 复收，滞回区间足够宽，不在阈值附近反复开关；
//   3) 背压时不丢数据，只是停止 recv，让 TCP 接收窗口自然收缩，由 TCP 自己降速；
//   4) 块数按 4→3→2→1 逐级降档，全失败才回落内部 RAM 单块，绝不一步到底。
#define SPOOL_BLOCK_BYTES       (64UL * 1024UL)
#define SPOOL_BLOCK_COUNT       4
#define SPOOL_CAPACITY          (SPOOL_BLOCK_BYTES * SPOOL_BLOCK_COUNT)
#define SPOOL_HIGH_WATER_PCT    75
#define SPOOL_LOW_WATER_PCT     25
// 分配环形缓冲时必须留住的 PSRAM 红线：Wi-Fi/LwIP 在内部 RAM 不够时会把
// 收发缓冲落到 PSRAM，这条红线就是它们的保命空间。
#define SPOOL_PSRAM_RESERVE     (1024UL * 1024UL)

// --- 双核分工（别让 WiFi 和 USB 抢同一个核）---------------------------------
// 核 0：Wi-Fi/LwIP 协议栈 + 两条网络通路的 socket 任务（9100 RAW / 631 IPP）。
// 核 1：USB Host（daemon + client）+ 环形缓冲搬运 + 主循环/Web + 监控。
// 两边只通过环形缓冲解耦：网络侧再忙也只是让 TCP 窗口收缩，USB 侧不会因为
// 抢不到 CPU 而把传输拖黄；反之 USB 的忙等也绝不进 Wi-Fi 所在的核。
#define CORE_NET                0
#define CORE_USB                1

// --- SNMP 状态代理 ----------------------------------------------------------
// 独立 UDP 161 任务 + 独立 socket，不复用任何打印通路（9100/631）的 socket、
// 任务或缓冲区。状态值统一取自 IPP 的 printer-state（ippPrinterState()），
// 因此 Web / IPP / SNMP 三处对「现在在不在打印」永远同一个答案。
#define SNMP_PORT               161
#define SNMP_COMMUNITY          "public"   // 只读 community，不开放 SET
#define SNMP_MAX_VARBINDS       8          // 一次请求最多处理多少个 varbind

// --- 给 LwIP / Wi-Fi 的 PSRAM 硬预留（防崩溃）-------------------------------
// 本固件编进的是 CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y +
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096：内部 RAM 不够时，Wi-Fi 的收发
// 缓冲、LwIP 的 TCP pbuf（>4KB 的分配）会直接落到 PSRAM。spool 是开机
// 一次性大块分配且永不释放，一旦把 PSRAM 吃净，Wi-Fi 任务里 malloc 失败
// 就是 abort/panic（现象 = 打印到一半随机重启）。所以 1MB 必须当成红线：
//   1) 开机：spool 分配完必须仍剩 >= SPOOL_PSRAM_RESERVE，否则缩容重试；
//   2) 运行期：free 掉到 CRIT 就强制暂停 spool 收包（收紧 TCP 窗口，
//      不再为新数据分配 pbuf），回到 RECOVER 才解除；
//   3) 持续临界过久 → 受控重启，留下重启原因，而不是等它崩。
#define PSRAM_WARN_FREE_BYTES   (512UL * 1024UL)  // 低于此值打印告警
#define PSRAM_CRIT_FREE_BYTES   (288UL * 1024UL)  // 低于此值强制停收（背压）
#define PSRAM_RECOVER_BYTES     (384UL * 1024UL)  // 回升到此值才解除强制停收
#define PSRAM_CRIT_HOLD_MS      20000UL           // 持续临界这么久 → 受控重启
#define PSRAM_MIN_LARGEST_BYTES (96UL * 1024UL)   // 最大连续块过小也算亚健康

// --- Wi-Fi 断线重连（指数退避）---------------------------------------------
#define WIFI_RETRY_MIN_MS       2000
#define WIFI_RETRY_MAX_MS       30000
#define WIFI_MAX_FAILS          30              // 连续失败这么多次后重启自救

// --- 任务看门狗 -------------------------------------------------------------
#define WDT_TIMEOUT_S           15              // 任一被监控任务卡死 15s → panic 重启
#define WDT_RESTART_DELAY_MS    3000           // 可控重启前的缓冲时间
#define LOW_HEAP_RESTART_BYTES  15000          // 内部堆（不是总堆）低于此值主动重启

// 串口诊断命令（115200，回车结尾）：wdt = 故意卡死验证看门狗；reboot = 受控重启；
// stat = 打印内存/缓冲状态。稳定后想彻底关掉就改成 0。
#define ENABLE_SERIAL_DEBUG_CMDS 1

// --- 状态指示灯（可选）------------------------------------------------------
// -1 = 关闭。常见板载灯: DevKitC-1 = GPIO48, SuperMini = GPIO8。
#define STATUS_LED_PIN          (-1)
