# 通用 USB 无线打印服务器（ESP32-S3）

[![release](https://img.shields.io/github/v/release/forchen86/esp32-usb-print-server)](https://github.com/forchen86/esp32-usb-print-server/releases)
[![license](https://img.shields.io/github/license/forchen86/esp32-usb-print-server)](LICENSE)
[![platform](https://img.shields.io/badge/platform-ESP32--S3-informational)](https://github.com/forchen86/esp32-usb-print-server)

**直接下载固件** → [Releases](https://github.com/forchen86/esp32-usb-print-server/releases/latest)
· `merged.bin` 整片烧 `0x0`（含 bootloader/分区表）
· `firmware.bin` 只升级写 `0x10000`（保留 Wi-Fi 与双面配置）

把**任意符合 USB Printer Class 的打印机**变成 **9100 RAW + IPP 631 双通路**网络打印机。固件只拆协议封装、把文档数据流式送进打印机接口的 Bulk OUT 端点：不解析、不渲染、**不做任何格式转换**、不落盘，只用一块 256 KB 环形缓冲做收发解耦。

**v1.4.0 起不再绑定具体机型**：上电后通过 USB Printer Class 的 `GET_DEVICE_ID` 控制请求读取 IEEE 1284 Device ID（`MFG / MDL / CMD / DES / SN`），自动识别厂商、型号、命令集；IPP 的 `printer-name / make-and-model / device-id / document-format-supported / color-supported / urf-supported`、Bonjour TXT 的 `ty / product / pdl / URF / color`、SNMP 的 `sysDescr / prtGeneralPrinterName`、以及 Web 页标题全部由识别结果动态生成。**v1.4.2 起对外发布的型号优先取 `DES`**：HP 的 `MDL` 常是系列名（实测 `HP LaserJet M203-M206`），`DES` 才是具体销售型号（`HP LaserJet M203d`）；原始 MDL 仍保留在 Web 身份卡里。读不到 Device ID 时回退到 `VID:PID`，能力使用保守默认值并在页面上标注「能力为推定值」。

```
   电脑 / 手机                     ESP32-S3                       USB 打印机
 ┌──────────────┐   Wi-Fi    ┌──────────────────┐   USB OTG   ┌──────────────┐
 │ RAW 9100     │ ─────────→ │ 拆封装 → 256 KB   │ ──────────→ │ 自己解释      │
 │ IPP  631     │            │ 环形缓冲 → 透传   │             │ PCL/PS/PDF/URF│
 │ SNMP 161(读) │ ←───────── │ 状态 / mDNS / Web │             │ 并执行双面    │
 └──────────────┘            └──────────────────┘             └──────────────┘
        不渲染 · 不转换格式 · 不落盘，固件只负责搬运字节
```

## 支持的打印机范围

| 项 | 支持情况 |
|---|---|
| USB 接口类 | **USB Printer Class**：`bInterfaceClass=0x07`、`bInterfaceSubClass=0x01`，协议 1（单向）或 2（双向），至少一个 Bulk OUT 端点 |
| USB 速率 | Full Speed / Low Speed（ESP32-S3 USB-OTG 硬件上限，Hi-Speed 设备会降速协商到 FS） |
| 打印语言 | **透传，与语言无关**：PCL5/PCL6/PCLXL/PCLm、PostScript、PDF、URF、PWG Raster、ESC/POS、ZPL、TSPL 等——只要客户端生成的是打印机自己能解释的字节流 |
| 作业来源 | `9100 RAW`（一个 TCP 连接 = 一份作业）、`631 IPP Print-Job`、`/api/abort` 中止 |
| 能力声明 | 从 1284 `CMD` 推导 MIME 列表；只有设备声明 URF 才发布 AirPrint 的 URF/PWG 能力，绝不向只支持 PCL 的机器谎报 raster |
| 状态 | `GET_PORT_STATUS` 轮询缺纸 / 选中状态，映射到 IPP `printer-state`、SNMP、Web；缺纸时直接拒绝作业，不再卡到超时 |
| 自动双面 | 从 1284 `CAP/CMD/DUPLEX` 与 `MDL`/`DES` 型号后缀 `d/dw/dn` 识别双面单元；IPP 侧 `sides` → PJL 作业头注入；识别不准可用「强制开/关」覆盖。**RAW 9100 不注入，双面由客户端驱动负责**，见下节 |
| 并发 | 单作业串行；第二个连接 IPP 回 `busy + Retry-After`，RAW 直接关闭 |
| 已实机验证 | HP LaserJet Pro M203d（`03F0:632A`） |

**不支持 / 明确边界**：

- **GDI / 主机渲染型打印机**（如 HP 1020 这类需要固件生成 JBIG/ZjStream 位图的机型）——本固件不做任何渲染；
- 复合 USB 设备里「非 Printer Class」的接口（扫描仪、读卡器、HID）——只占用打印接口；
- 需要双向 1284.4 状态通道或厂商私有握手才能收数据的驱动——本固件只读 `GET_PORT_STATUS` 与 `GET_DEVICE_ID`；
- 需要固件下载 / 上电初始化的打印机；
- 任何需要**在板端做格式转换**的场景（设计上就排除了）；
- USB High-Speed 吞吐需求：FS 实际约 0.7–1.0 MB/s，大位图作业会明显变慢。

### 按打印语言看

| 打印语言 | 是否支持 | 说明 |
|---|---|---|
| PCL5 / PCL5c / PCL6 / PCLXL | ✅ | HP LaserJet 主力语言，透传 |
| PostScript 3 | ✅ | 激光机常见 |
| PDF（直接打印） | ✅ | 免驱首选，体积比 raster 小一个数量级 |
| URF（Apple Raster） | ✅ | AirPrint 用；HP 在 1284 里常写作 `URP`，固件已按别名识别 |
| PWG Raster | ✅ | IPP Everywhere 用 |
| PCLm | ✅ | 移动打印常见 |
| ESC/POS | ✅ | 小票机，走 9100 直接发 |
| ZPL / TSPL / CPCL | ✅ | 标签机，走 9100 |
| GDI / 主机渲染型 | ❌ | 需要驱动端生成位图，固件不渲染 |

### 机型参考（哪些验证过）

| 厂商 | 机型 | 状态 |
|---|---|---|
| **HP** | **LaserJet Pro M203d**（`03F0:632A`） | ✅ **实机验证**：RAW 出纸、双面识别、缺纸检测、IPP 属性 |
| HP | LaserJet M203-M206 系列（PCL6） | ✅ 同系列同语言，理论即可用 |
| HP | 其他 PCL5 / PCL6 / PS 激光机 | ⚠️ 协议层支持，未逐台实测 |
| Brother | HL / MFC 系列（PCL6 / PS） | ⚠️ 理论支持；型号后缀 `DW/DN` 可被识别为双面机 |
| Canon | LBP / i-SENSYS 的 PCL 机型 | ⚠️ 理论支持；UFR II 属私有语言，需驱动端生成后再透传 |
| Epson | 激光机（PCL/PS）、小票机（ESC/POS） | ⚠️ 理论支持 |
| Zebra / TSC | ZPL / TSPL 标签机 | ⚠️ 理论支持，纯透传、无状态回读 |
| 任意 USB Printer Class 设备 | — | ✅ 协议层通用（类 07 / 子类 01 / 协议 1 或 2） |

> ⚠️ 表示硬件与协议条件都满足、但没有实机跑过。欢迎开 issue 补充你的机型与结果，我会并进这张表。

> 下文各历史章节（第一~十一节）记录了以 HP M203d 为样机的调参、排错与验证过程，其中的机型专属数字与结论仅供背景参考；**现行架构与能力以本节为准**。

### RAW 9100 上的双面打印（重要）

RAW 9100 只有一条裸字节流，**没有 IPP 那样的 `sides` 属性协商**，所以固件在这一侧**刻意不做任何注入**：双面指令必须由客户端驱动写进它生成的打印数据流（PJL `@PJL SET DUPLEX=ON` / `@PJL SET BINDING=LONGEDGE`，或 PCL `&l1S` 等），固件原样透传。自动识别出的双面能力只影响 IPP `sides-supported`、Bonjour `duplex=T`、URF `DM3`、SNMP 与 Web 显示，**不会改变 RAW 字节流**。

Windows 下开启双面（以 HP LaserJet Pro M203d 为例）：

1. 添加打印机 →「使用 TCP/IP 地址或主机名添加」→ 填设备 IP（如 `192.168.10.43`），**取消勾选**「查询打印机并自动选择要使用的驱动程序」；
2. 端口设置：协议 **RAW**、端口号 **9100**、**关闭「启用 SNMP 状态」**（本固件 SNMP 是只读简化实现，开着容易让 Windows 误判离线）；
3. 驱动必须选 **HP 原厂 PCL6 / PCL5 驱动**（如 `HP LaserJet Pro M203-M206 PCL-6`），不要用 `Microsoft IPP Class Driver` 或 `Generic / Text Only` —— 这些驱动不会生成双面指令；
4. 打印机属性 → **设备设置 → 可安装选项 → 双面打印单元：已安装**。固件没有双向状态通道，驱动无法自动探测，必须手动声明，否则双面选项是灰的；
5. 打印首选项选「双面打印 / 长边装订」，打印双页文档验证。

排查：若仍只出单面，用 Wireshark 抓 9100 数据流搜 `DUPLEX`；搜不到说明驱动没发指令，问题在客户端而不是固件。

- **两条打印通路并存**：`9100 RAW`（默认，Windows「TCP/IP 端口 + Raw」直连，close 即作业结束）+ `631 IPP`（IPP Everywhere / AirPrint 兼容机型）。两个监听任务分开，共用同一个作业泵
- **自动识别机型**：1284 Device ID → 厂商/型号/序列号/命令集/PDL，IPP、mDNS、SNMP、Web 四处同源
- **自动双面**：从 1284 `CAP/CMD/DUPLEX` 与型号后缀 `d` 识别双面单元，三态策略（自动 / 强制开 / 强制关）；IPP 的 `sides` 作业属性会在文档前翻译成 PJL `SET DUPLEX=ON / SET BINDING=…` 注入，解决「选了双面却出单面纸」；对不懂 PJL 的标签机自动禁用，RAW 通路不注入
- **单作业并发**：第二个连接不再排队 —— IPP 直接回 `server-error-busy` + `Retry-After: 30`，RAW 直接关闭连接让客户端重试，backlog 里不堆内存
- **Web 界面**：设备身份卡片 + 状态徽章 + 三通路指示 + 环形缓冲水位条 + 上次作业 + Wi-Fi 配网 + 双面开关 + 中止/重启
- **AP 只在需要时开**：STA 连续在线 60s 自动关闭热点（省电、降空口干扰）；STA 掉线满 3 分钟自动重开，仍可重新配网
- **被动 mDNS**：自己实现 5353 responder，**只应答查询、不做任何周期 announce**（ESPmDNS 的 TTL 固定 120s，每 60s 一轮广播）。`_ipp._tcp` / `_pdl-datastream._tcp` / `_http._tcp` 全支持
- **SNMP 161 状态**：只读 v1/v2c GET/GETNEXT，`hrDeviceStatus` / `hrPrinterStatus` 等取值统一来自 **IPP 的 printer-state**，与 Web、IPP 三处口径一致
- **Wi-Fi 断线重连**：关闭自动重连，自己实现 2s→4s→8s→…→30s 指数退避；长期失败自动重启自救
- **任务看门狗**：硬件 TWDT 监控全部自建任务，任一任务卡死 15s 即 panic 重启
- **双核分工**：Wi-Fi/LwIP + 9100 + 631 固定在核 0；USB Host / 缓冲搬运 / Web / 监控固定在核 1，互不抢核
- **256 KB 环形缓冲 + 双水位背压**：4 × 64 KB 固定块池（PSRAM），运行期零 malloc/free；75% 停收、25% 复收，背压靠**停止 recv 让 TCP 窗口自己收缩**，不丢数据
- **USB 16 KB 双缓冲乒乓**：两个 transfer 轮转，一个在飞时另一个已排队；短包即判失败并清端点，绝不把错位数据继续喂给打印机
- **作业收尾**：正常结束**只补 `UEL`（ESC%-12345X）**，防止下一页与前一份的格式残留粘在一起；v1.4.4 起去掉 `FF(0x0C)` —— UEL 后页缓冲已空，再送 FF 会强制走纸吐出一张空白页（「每次打印都多一张白纸」的根因）。异常中止走 `abort`（halt + flush + clear）并回报 `job-state=aborted(8)`

---

## 一、GitHub 调研结论

用 GitHub API 检索了 `esp32-s3 print server` / `esp32 usb printer 9100` / `esp-idf usb host printer` 等关键词，主要候选如下（2026-11-11）：

| 项目 | Star | 许可 | 做法 | 评价 |
|---|---|---|---|---|
| [ngxson/esp32-usb-to-airprint](https://github.com/ngxson/esp32-usb-to-airprint) | ~289 | MIT | 移植 Gutenprint/SANE，实现 AirPrint + AirScan | 功能最全，但只覆盖 Canon PIXMA 系列，代码量大，与「越简单越好」相悖 |
| [WARMW00D/ESP32-S3-PrintServer](https://github.com/WARMW00D/ESP32-S3-PrintServer) | 10 | MIT | Arduino + [EspUsbHost](https://github.com/tanakamasayuki/EspUsbHost)，RAW 9100 + LPR 515 + IPP/PWG→PCL5 + OLED + 企业网 | 硬件资料（OTG 跳线、双 Type-C 口、core 版本坑）非常有价值；功能太多 |
| [NitsuaFox/thermal-bridge](https://github.com/NitsuaFox/thermal-bridge) | 0 | MIT | Arduino-CLI，4 KiB 有界缓冲 + 背压、AP 配网、故障检测、单元测试，锁定 core 3.3.10 | 架构最贴近需求，USB Host / 背压 / 超时的实现被本固件参考 |
| [B4nd1/esp32-s3-printer](https://github.com/B4nd1/esp32-s3-printer) | 0 | — | 单文件 `.ino`，`usb/usb_host.h` + `WiFiServer(9100)` | 最小可跑通范例（已实测 Canon MG2550S），但 Wi-Fi 硬编码、无 Web、无重连、无看门狗，且每次 write 都新建 transfer 无背压 |
| [LoveLH/ESP32-S3-USB-PrintServer](https://github.com/LoveLH/ESP32-S3-USB-PrintServer) | 3 | MIT | RAW 9100 + Web UI + OTA + mDNS + IPP + USBIP + crashlog | 功能齐全但偏复杂 |
| [arslansadiq87/print-server-esp32-s3](https://github.com/arslansadiq87/print-server-esp32-s3) | 42 | — | ESP-IDF，面向 TSC TTP-244 Pro 标签机 + HTTP 标签编辑 | 面向特定机型，不适用 M203d |
| [SMe12435/ESP32S3-HP1020-PrintServer](https://github.com/SMe12435/ESP32S3-HP1020-PrintServer) | 0 | — | HP 1020（ZjStream/JBIG）+ Flask 云端的实验 | 反例：GDI/主机型打印机需要固件参与渲染，不适合透传方案 |

**选型结论**：以 `B4nd1` 的 USB Host 骨架 + `thermal-bridge` 的背压/超时/AP 配网思路 + `WARMW00D` 的硬件避坑说明为基础，裁掉 IPP/AirPrint/LPR/OTA/OLED/文件系统，重写成 7 个源文件的纯透传固件；PSRAM 只用来做收发解耦的 spool，不做整作业缓存。

### 两个关键事实（决定了方案可行）

1. **M203d 有真正的页描述语言**：HP 官方规格列出 `PCL5c / PCL6 / PS / PCLm / PDF / URF / PWG`，且只有 `1× Hi-Speed USB 2.0`、`Network ready: No`。Windows 驱动会把文档渲染成 PCL6/PS 字节流，ESP32 只需透传，**不需要理解打印语言**——这正是最稳的做法。相比之下 HP 1020 这类 GDI/主机型打印机要在固件里做 JBIG/ZjStream 渲染，完全不可行。
2. **USB 速度**：ESP32-S3 的 USB-OTG 只有 **Full Speed 12 Mbps**，M203d 会降速协商到 FS。文本/普通 PCL6 作业没问题；整页 600dpi 位图类作业会明显变慢，这是硬件上限，不是固件 bug。

### 踩坑预警：Arduino-ESP32 版本

`arduino-esp32 3.3.11`（IDF 5.5.5）存在已确认的 **USB Host 回归**（`HUB: Root port reset failed`，设备永不枚举，见 [espressif/arduino-esp32#12783](https://github.com/espressif/arduino-esp32/issues/12783)）。本工程锁定 **Arduino-ESP32 3.3.0 / IDF 5.5**（pioarduino 平台 `55.03.30`），已实测编译通过。**不要随意升级 core**。

---

## 二、支持的 ESP32 板型

### 2.1 板型对照

| 板型 / 模组 | `platformio.ini` 里的 `board` | PSRAM | 支持情况 |
|---|---|---|---|
| **ESP32-S3 N16R8**（推荐、默认） | `esp32-s3-devkitc1-n16r8` | 8 MB OPI | ✅ 完整支持；本项目就是在该板 + HP M203d 上实测的 |
| ESP32-S3 N8R8 | `esp32-s3-devkitc1-n8r8` | 8 MB OPI | ✅ 只改 `board` 一行即可，Flash 尺寸/分区表随之切换 |
| 其他 ESP32-S3 板（含 N8R2 / 无 PSRAM） | `esp32-s3-devkitc-1` | 2 MB 或无 | ⚠️ 能跑。spool 会按实测可用内存自动降档（PSRAM 256 KB → 内部 RAM 128~16 KB），只影响大图作业的缓冲余量，不影响功能 |
| ESP32-S3-Zero / SuperMini 等小板 | 视板而定 | 视板而定 | ⚠️ 多数要**先短接背面的 OTG 焊桥**，否则 USB Host 永远枚举不到打印机 |
| ESP32-S2 系列 | — | 通常 2 MB | ❌ 不支持：代码按双核分工实现，S2 是单核 |
| ESP32 经典款（WROOM-32 / WROVER） | — | — | ❌ 不支持：没有 USB Host 控制器 |
| ESP32-C3 / C6 | — | — | ❌ 不支持：单核 + 无 USB Host |
| ESP32-P4 | — | — | ❌ 未适配（USB Host 与 PSRAM 配置都不同） |

**为什么必须双核**：`net_print.cpp` / `usb_printer.cpp` 用 `xTaskCreatePinnedToCore(..., CORE_NET=0 / CORE_USB=1)` 把 Wi-Fi·LwIP 与 USB 传输分到两个核，避免互相抢占导致吞吐抖动。单核芯片上这些调用会失败或退化，所以 S2 / C3 不在支持范围。

**换板只改一行**：`platformio.ini` 里 `[env]` 段的 `board`。Flash 尺寸、分区表、PSRAM 库变体会一起切换，源码不用动。spool 容量在 `src/config.h` 按编译期宏算，运行期再按实际可用 PSRAM 逐档退让。

### 2.2 接线、供电与常见坑

- **必须有独立的原生 USB-OTG 口**（GPIO19 = D-、GPIO20 = D+）和**另一个 UART/USB 转串口口用于烧录**。双 Type-C 板子要认清丝印：标 `USB/OTG` 的接打印机，标 `COM/UART` 的接电脑。
- 很多板子（尤其 SuperMini / DevKitC 兼容板）背面有 **OTG 焊桥**需要短接，否则 USB Host 永远枚举不到设备。查你板子的原理图确认。
- **打印机必须自己供电**（接市电），USB 线只传数据。ESP32-S3 板子由独立 5V 电源供电；VBUS 只作为「插入检测」，不要指望板子给打印机供电。
- 需要 OTG 转接线（USB-C 母 ↔ USB-B / A 母），必须是**数据线**，充电线不行。
- 2.4GHz Wi-Fi（ESP32-S3 不支持 5GHz）。

上电顺序建议：先给打印机上电 → 给板子上电 → 再插 USB 数据线；或直接插好再一起上电。

---

## 三、编译（两个版本）

依赖：[Python 3](https://www.python.org/) + [PlatformIO Core](https://docs.platformio.org/page/core.html)（`pip install platformio`）。本工程无外部库依赖。

同一份源码编译出两套固件，区别只在日志：

| 环境 | 命令 | 串口日志 | 用途 |
|---|---|---|---|
| **release**（默认） | `pio run -e release` | 只留事件与异常，启动约 1.4 KB / 15 行 | 日常使用 |
| **debug** | `pio run -e debug` | 全量观测点：逐请求、逐作业、轮询、内存快照 | 排障 / 验收 |

release 下 `FWLOGD(...)` 由编译器整体消除（`do { if (false) ... } while (0)`），参数不求值、日志字符串不进 Flash；`FWLOGI`（事件）与 `FWLOGE`（异常）两版都保留，正式版仍有最基本的可运维性。

```bash
bash scripts/build.sh            # 两套都出（Windows: scripts\build.bat）
bash scripts/build.sh release    # 只出正式版
bash scripts/build.sh debug      # 只出调试版
```

产物：`firmware/release/` 与 `firmware/debug/`，各含 `merged.bin` / `firmware.bin` / `bootloader.bin` / `partitions.bin` / `SHA256SUMS.txt`。

Windows 用户**请用 PowerShell 或 CMD 执行首次构建**，不要从 Git Bash/MSYS 里跑（Espressif 的 `idf_tools.py` 拒绝 MSYS 环境）。工具链装好之后 Git Bash 也能编译。

### 烧录

```bash
bash scripts/flash.sh COM11 release              # 整片烧录（含 bootloader / 分区表）
APP_ONLY=1 bash scripts/flash.sh COM11 release   # 只写 app 分区，保留 Wi-Fi / 双面配置
```

Windows：`scripts\flash.bat COM11 release`；只升级 app 时先 `set APP_ONLY=1` 再执行。

### 已附编译好的固件（可直接烧）

| 文件 | 说明 |
|---|---|
| `firmware/release/merged.bin` | **推荐**：正式版合并镜像，从 `0x0` 一次烧完 |
| `firmware/release/firmware.bin` | 正式版 app，烧到 `0x10000`（只升级用这个） |
| `firmware/debug/merged.bin` | 调试版合并镜像（全量日志） |
| `firmware/debug/firmware.bin` | 调试版 app |
| `firmware/<env>/SHA256SUMS.txt` | 上述文件的校验值 |
| `firmware/legacy-v1.4.4/` | 上一版归档，可删 |

编译实测（v1.5.0）：release Flash **1,079,184 B（16.5%）**、RAM **67,784 B（20.7%）**；debug Flash 1,086,656 B —— 多出的约 7.4 KB 全是观测日志字符串。

> 镜像按 **N16R8：16MB Flash / DIO / 80MHz + 8MB OPI PSRAM** 生成（bootloader 头 `0x4f` = 16MB/80MHz；DIO 对 QIO 接线也兼容）。8MB flash 的 N8R8 请把 `platformio.ini` 的 `board` 改成 `esp32-s3-devkitc1-n8r8` 后重新编译；无 PSRAM 板改 `esp32-s3-devkitc-1`，固件会沿同一阶梯逐档退到内部 RAM 的 KB 级档位。
>
> 实机状态：v1.5.0 release 已烧 COM11（`192.168.10.43`），启动日志 15 行、异常 0；`scripts/verify_firmware.py` 在 release 模式 30/30 通过（4 项日志断言自动 SKIP），加 `--debug` 复验 debug 版时全项断言。

---

## 四、烧录

方式 A（推荐，PlatformIO 自动找端口）：

```bash
pio run -t upload --upload-port COM7      # Windows
pio run -t upload --upload-port /dev/ttyUSB0
pio device monitor -b 115200
```

方式 B（esptool，适合用合并镜像）：

```bash
esptool.py --chip esp32s3 --port COM7 --baud 460800 write_flash 0x0 firmware/M203D_merged.bin
# 或分开烧：
esptool.py --chip esp32s3 --port COM7 --baud 460800 write_flash \
  0x0 firmware/bootloader.bin 0x8000 firmware/partitions.bin 0x10000 firmware/firmware.bin
```

烧录后串口（115200）应能看到：

```
[sysmon] TWDT 15s, boot #1, reset=POWERON
[spool] PSRAM spool 7192576 bytes (85.7% of 8388608), Wi-Fi/LwIP reserve 1175096 bytes
[wifi] AP PrintSrv-XXXXXX / 192.168.4.1
[wifi] mDNS http://printserver.local, _ipp._tcp:631
[web] UI ready on port 80 (AP + LAN)
[usb] 打印机就绪 VID:03F0 PID:xxxx 接口0 OUT:0x01
[ipp] IPP 打印服务已监听 TCP 631 /ipp/print
[guard] after stack up: psram free 1162360 (largest 1146868), internal heap 211692 / largest 167924, alloc fails 0
```

`VID:03F0` = HP。看到这行说明 USB Host 枚举和接口占用都成功了。

---

## 五、使用

### 5.1 第一次开机与配网

1. 按第四节烧录后，打印机接市电，USB 数据线插到板子的 **OTG** 口（不是 COM/UART 那个）。
2. 上电，等约 10 秒。串口 115200 会打印 `[usb] 打印机就绪 VID:...`。
3. 手机/电脑连热点 **`PrintSrv-XXXXXX`**（默认无密码）。
4. 浏览器打开 **`http://192.168.4.1`**。
5. 「网络」卡片 → 「扫描」或手动填 SSID/密码 → 「保存并连接」。
6. 连上后页面「路由器分配的地址」会变成 `192.168.x.x`，**60 秒后热点自动关闭**。
7. 之后改用 `http://<该地址>/` 或 `http://printserver.local/` 访问。

> 想换 Wi-Fi：点页面「清除配置」，热点会立刻重开。STA 掉线满 3 分钟也会自动重开，但主动清除更快。

### 5.2 Windows：RAW 9100（推荐，最省事）

| 步骤 | 操作 |
|---|---|
| 1 | 设置 → 蓝牙和其他设备 → 打印机和扫描仪 → 添加设备 → **手动添加** |
| 2 | 选「使用 IP 地址或主机名添加打印机」→ 下一步 |
| 3 | 设备类型：**Generic Network Card**（下拉框没有就选 TCP/IP Device） |
| 4 | 主机名或 IP：设备页面上显示的地址，如 `192.168.10.43`；端口名自动生成 |
| 5 | 端口设置：协议 **RAW**、端口 **9100**。若第 3 步只能选 TCP/IP Device，**取消勾选「启用 SNMP 状态」** |
| 6 | 驱动：选这台打印机的原厂 PCL6 / PCL5 驱动（HP 例：`HP LaserJet M203-M206 PCL-6`）。列表里没有就「从磁盘安装」指向官网驱动包 |
| 7 | 装完 → 打印机属性 → 设备设置 → 可安装选项 → **双面打印单元：已安装** |
| 8 | 打印首选项 → 双面打印（长边装订）→ 打一份双页文档验证 |

为什么强调第 3 / 5 步：在「TCP/IP Device」模式下 Windows 会尝试用 SNMP 问设备状态，而本机的 SNMP 是**只读简化实现**（只答少数 OID），问不到就可能把打印机标成「离线」。选 Generic Network Card 或关掉「启用 SNMP 状态」就绕开了。

### 5.3 Windows：IPP 631（免驱）

用页面「怎么在电脑上添加」里生成的两行命令，**先建端口再加打印机**（少了 `Add-PrinterPort` 会失败）：

```powershell
Add-PrinterPort -Name "http://192.168.10.43:631/ipp/print"
Add-Printer -Name "HP-LaserJet-M203d" -DriverName "Microsoft IPP Class Driver" -PortName "http://192.168.10.43:631/ipp/print"
```

也可以图形操作：「我需要的打印机不在列表中」→「按名称选择共享打印机」→ 粘贴 `http://192.168.10.43:631/ipp/print`。

> 只会 PCL 的机型建议走 5.2 的 RAW —— IPP 客户端可能发来打印机不认识的 URF / PWG raster。

### 5.4 macOS / iOS（AirPrint）

- 同一局域网；macOS「系统设置 → 打印机与扫描仪 → +」里会出现 **HP LaserJet M203d**，直接添加。
- iPhone / iPad 打印时在打印机列表里选它。
- 搜不到先查路由器：**AP 隔离 / 客户端隔离 / 组播过滤**会阻断 mDNS（5353），关掉即可。
- 本机 mDNS 是「被问才答」、不主动广播，首次发现可能比商业打印机慢 1~2 秒，属正常。

### 5.5 Linux / CUPS

```bash
# 免驱（IPP Everywhere）
lpadmin -p m203d -E -v ipp://192.168.10.43:631/ipp/print -m everywhere

# 或走 RAW（需要对应 PPD）
lpadmin -p m203d -E -v socket://192.168.10.43:9100 -m <你的 PPD>
```

### 5.6 双面打印怎么开

| 通路 | 双面由谁负责 | 要做的事 |
|---|---|---|
| **RAW 9100** | **客户端驱动** | 驱动里声明「双面打印单元已安装」，打印时选双面。固件纯透传、不注入 |
| **IPP 631** | **固件** | 页面「双面打印」设为「自动」或「总是双面」，并保持「自动补发双面指令」开启 |

「自动」的判定依据会显示在页面上（例：`型号以 d 结尾，通常代表双面机型（DES）`）。识别不准就手动切「总是双面 / 总是单面」。

### 5.7 升级固件（保留配置）

```bash
APP_ONLY=1 bash scripts/flash.sh COM11 release
```

只写 `0x10000` 的 app 分区，NVS 里的 Wi-Fi 与双面设置都保留。整片烧 `merged.bin` 会清掉这些配置。

### 5.8 排障速查

| 现象 | 最可能原因 | 处理 |
|---|---|---|
| 网页打不开 | 不在同一网段 / 热点已自动关闭 | 连 `PrintSrv-XXXXXX`，或查路由器租约 |
| AirPrint 搜不到 | 路由器开了 AP 隔离 / 组播过滤 | 关隔离，或改用 IP 直接添加 |
| 9100 连不上 | 防火墙拦 / IP 变了 | `Test-NetConnection <IP> -Port 9100` |
| 一直显示「正在打印」 | 缺纸 / 门开 / 卡纸 | 看页面「纸张」状态；`POST /api/abort` 可中止 |
| 每次多一张白纸 | v1.4.4 之前的 FF 问题 | 升级到 v1.5.0+ |
| 双面不生效 | 驱动没声明双面单元 / RAW 下驱动不发指令 | 见 5.6；抓 9100 数据流搜 `DUPLEX` |
| 打出来乱码 | 驱动语言与打印机不匹配 | 换对应的 PCL / PS 驱动 |
| 出纸偏慢 | USB Full Speed 上限（0.7–1.0 MB/s） | 正常现象；大图作业优先用 PDF |

### 5.9 日志与诊断

- **release 版**：串口只有事件与异常（启动约 15 行），日常够用。
- **debug 版**：逐请求、逐作业、内存快照全开，排障时临时刷：
  ```bash
  bash scripts/flash.sh COM11 debug
  ```
- 串口命令（115200、回车结尾）：`stat` 看任务/内存/缓冲，`wdt` 看看门狗，`reboot` 重启。
- 自动验收：`python scripts/verify_firmware.py`（release 模式）或加 `--debug` 验证调试版。

---

## 六、PSRAM spool：容量与防顶满（v1.2.0 及以前）

> **v1.3.0 已废弃本节做法**：整包大缓存改成 4 × 64 KB 固定块池组成的 256 KB 环形缓冲，
> 双水位改为 75% / 25%。本节保留作为历史与调参参考，现行架构见 **第十一节**。

> 完整的并发性论证与缺陷清单见 [BUG_REVIEW.md](BUG_REVIEW.md)。

### 架构

```
IPP 客户端 ──HTTP/631──▶ 拆 IPP 封装 ──▶ [ PSRAM 环形 spool ] ──4KiB──▶ USB Bulk OUT ──▶ M203d
                          (URF / PWG / PDF / PCLm 原样透传，打印机自己解释)
                        ▲ 生产者 ipp631 任务     ▲ 消费者 spoolWriterTask
                        └── 90%/70% 水位背压 ──┘
```

网络收包和 USB 发送跑在两个独立任务里：Wi-Fi 抖动、驱动突发大包由 spool 吸收，USB 端按自己的节奏稳定喂数据。USB Host 的 transfer buffer 必须在内部 DMA RAM，所以消费者每次从 PSRAM `memcpy` 最多 4 KiB 到内部缓冲，等完成回调后才推进读指针把空间还给生产者。

### 容量怎么定（`src/config.h`）

`spoolBegin()` 按下面步骤分配（4 KiB 对齐），并**用实测值反查红线**，不靠公式推算：

```
1) cap  = min(SPOOL_TARGET_BYTES, free_psram - SPOOL_PSRAM_RESERVE - 边际)
2) ps_malloc(cap)，成功后实测 free_psram 是否仍 >= SPOOL_PSRAM_RESERVE
3) 不达标 / 分配失败 → 只降一档再试，绝不一步到底：
      >4MB 每次 -25% ｜ 1~4MB 每次 -12.5% ｜ 256K~1MB 每次减半
      128~256KB 每次 -64KB ｜ <128KB 每次 -32KB，探到 64KB 才算 PSRAM 放不下
4) PSRAM 一档都放不下才进内部 RAM，而内部 RAM 自己也分 8 档：
      128 → 96 → 80 → 64 → 48 → 32 → 24 → 16KB
      每档都要求「分配后仍留 64KB 给 Wi-Fi/任务栈」，且显式 MALLOC_CAP_INTERNAL
```

所以任何一次失败都只意味着「下一档更小」，不存在从 7MB 直接摔到最小档的情况；串口里能看到完整下探轨迹（`rung 5: 3,043,328 B alloc failed -> next 2,662,912 B`）。

内部 RAM 到底多大，这里也说清楚（之前文档写错过）：**ESP32-S3 片内 SRAM 总计 512KB**，但链接脚本 `ld/memory.ld` 给应用的 DRAM 段是 `dram0_0_seg: org = 0x3FC88000, len = 0x53700 = 341,760 B（≈334KB）`，其余约 178KB 被 ROM 的 bss/data 与 cache 占用；Wi-Fi + LwIP + Web + IPP + USB Host 全起来后，实测空闲内部堆约 **211KB**。这就是为什么内部 RAM 兜底只能给到 KB 级档位。

N16R8（8MB PSRAM）默认 `SPOOL_TARGET_BYTES = 8MB`（硬上限）、`SPOOL_PSRAM_RESERVE = 1MB`，COM10 实测：首次分配超出红线 4,552 B → 按缺额缩容一次后成功，得到 **7,192,576 B ≈ 6.86 MiB spool（占 8,388,608 B 的 85.7%）**，留给 Wi-Fi/LwIP 的 PSRAM 实测 **1,175,096 B**。Web 的「介质 / 容量」与「Wi-Fi 预留」两行会直接显示这两个数字。

留 1MB 而不是把 8MB 全占，是因为 SDK 打开了 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` + `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`：内部 RAM 不够时，Wi-Fi 的收发缓冲、LwIP 的 TCP pbuf（>4KB 的分配）会直接落到 PSRAM。spool 是开机一次性大块分配且永不释放，一旦把 PSRAM 吃净，Wi-Fi 任务里 `malloc` 失败就是 abort/panic——现象就是「打印到一半随机重启」。所以这 1MB 被当成**硬红线**，三层守住：

1. **开机强校验**（`spool.cpp`）：分配完必须实测剩余 ≥ 1MB，不达标就按缺额精确缩容重试。
2. **运行期守护**（`sysmon.cpp` 的 `psramGuardService`，200 ms 一次）：free 掉到 **288 KB** 以下 → `spoolForcePause(true)` 强制停止收包（收紧 TCP 窗口，Wi-Fi 不再为新数据分配 pbuf）；回升到 **384 KB** 才解除（滞回，防抖）。持续临界 20 s → 受控重启并记下原因，而不是等它崩。
3. **分配失败可诊断**：`heap_caps_register_failed_alloc_callback()` 注册钩子，任何 OOM 都会用 ROM 级 print 打出 `size / caps / 调用函数 / 当前 internal & psram 余量`，回调里不分配任何内存（否则会二次崩）。

启动后打印一次「栈全起来」的内存快照，COM10 实测（v1.1.0 阶梯版）：

```
[spool] rung 1: 7266304 B breaks reserve by 4552 -> refine 7192576 B
[spool] PSRAM spool 7192576 bytes (85.7% of 8388608), Wi-Fi/LwIP reserve 1175096 bytes, ladder 2
[mem] internal heap total 341604, free 211692, largest 167924
[mem] DMA-capable free 203964, largest 167924
[mem] psram total 8388608, free 1162360, largest 1146868, alloc fails 0
```

`internal heap total 341604` 正是链接脚本给应用的 DRAM 段实测值（≈334KB），印证了上面关于 512KB/334KB 的说法。Wi-Fi + mDNS + Web + IPP + USB Host 全部就绪后，PSRAM 仍余 **1.11 MiB、最大连续块 1.09 MiB（无碎片）、OOM 0 次**，才是这 1MB 真的留住了的证据。IPP 免驱下 URF / PWG raster 单页就有好几 MB，所以这 7MB 级 spool 是必要的：背压压住 TCP 窗口，数据不丢、通路不卡死。

| 想怎么调 | 改哪里 |
|---|---|
| 更激进（约 7.5 MiB，风险自担） | `SPOOL_TARGET_BYTES = 8MB`（保持）、`SPOOL_PSRAM_RESERVE = 512KB`；系统可用余量变小，若出现 Wi-Fi 不稳请改回 1MB |
| 保守一点 | `SPOOL_TARGET_BYTES = (6UL * 1024UL * 1024UL)` |
| 对齐参考项目的 512KB 保守值 | `SPOOL_TARGET_BYTES = (512UL * 1024UL)` |
| 无 PSRAM 板 | 不用改：`ps_malloc` 一档档往下退，最后按 128→96→80→64→48→32→24→16KB 分级落到内部 RAM（`CONFIG_SPIRAM_IGNORE_NOTFOUND=1` 保证能启动），仍可打印，只是缓冲小 |

### 三重保障：spool 永远不会被顶满

1. **写入长度硬上限（数学上不可能溢出）**：生产者把文档字节写进 `spoolWriteAcquire()` 给出的连续区域，长度 = `min(当前空闲, 回绕前连续段, 4KiB)`。IPP 的 `Content-Length` 路径直接 `recv()` 进 spool（零拷贝），`chunked` 路径经 1 KiB 小缓冲再拷一次；空间为 0 时干脆不收。没有任何路径能写超过空闲量。
2. **双水位滞回背压**：占用达 **90%** 暂停收包，降到 **70%** 才恢复。暂停时 TCP 接收窗口自然收窄，客户端被流控减速而不是被丢数据；滞回避免在水位线附近反复开关窗口。
3. **背压期间不判空闲超时**：被水位挡住时，60 秒 idle 超时不计时，避免把「客户端在等我们消化」误判成断线。

单生产者/单消费者环形队列，读写索引的提交都在临界区内完成：写提交后消费者才可见、读提交后空间才可复用，双核下不会错位。

### 作业完成语义

客户端发完数据（FIN）后**不立刻关闭连接**，而是等 spool 完全排空、全部字节送进 USB 之后才正常 `close()`。这样 CUPS 的 `waiteof=true` 才真的等到传输结束，下一个作业不会和上一个交错；失败则用 `SO_LINGER` 发 RST 明确告诉打印队列。

### 可观测性

Web 界面「PSRAM spool」卡片实时显示介质/容量、当前占用（70% 变黄、90% 变红）、**历史峰值**、**背压触发次数**、**Wi-Fi 预留**（开机实测值 / 当前剩余 / 最大连续块 / 分配失败次数）、**累计收 / 送**、当前是否暂停收包（区分「背压暂停」与「PSRAM 临界强制停收」）；`/api/status` 的 `spool` 与 `print.received/sent` 提供同样数据。

打印一个大作业时，「累计收」和「累计送」应**同时增长**——这就是边 `recv()` 边送 USB 的直接证据：生产者在 `ipp631` 任务里不停收包，消费者在 `spool-writer` 任务里并行搬运，两者互不等待。峰值长期贴着 90%、背压次数持续增加，说明 USB Full Speed 明显慢于网络，背压正在起作用（数据不会丢，通路也不会卡死）；想减少背压可以调大 spool 或换 PCL6 驱动减小作业体积。

---

## 七、稳定性设计（对应你的每一项要求）

| 需求 | 实现位置 | 行为 |
|---|---|---|
| 功能简单 | 全工程 8 个 `.cpp` | 只有 USB Host、IPP 631、Wi-Fi、HTTP、PSRAM spool；没有 RAW 9100 / LPR / OTA / 文件系统 / 渲染器 |
| Wi-Fi 断线重连 | `wifi_manager.cpp` | 关闭 `setAutoReconnect`，轮询 `WiFi.status()`，2s→30s 指数退避；成功即清零；连上过之后长期掉线达 30 次触发重启；**从未配好密码时不重启**，保持 AP 可用 |
| Wi-Fi 扫描稳定 | `wifi_manager.cpp` | 所有 `WiFi.*` 扫描 API 收敛在 `monitorTask` 单任务里，异步扫描 + 4 次重试 + 串口诊断；Web handler 只投递请求/取结果，不与重连任务并发；扫描期间暂停发起连接 |
| AP + STA 同时可用 | `wifi_manager.cpp` / `ipp_print.cpp` | `WIFI_AP_STA`，AP 常开；631 绑定 `INADDR_ANY`，两个地址都能打印；mDNS 在 AP 启动时就发布，直连热点也能被 AirPrint 发现 |
| 任务看门狗 | `sysmon.cpp` | SDK 默认已初始化 TWDT，用 `esp_task_wdt_reconfigure()` 把超时改成 15s 并强制 `trigger_panic=true`（配置失败会 `deinit` 后重建，确保一定武装上）；usb-daemon / usb-client / spool-writer / ipp631 / sysmon / Arduino loopTask 全部注册并周期性喂狗；usb-daemon 用 1s 有限阻塞而非永久阻塞，保证能喂狗。**已实测**：串口发 `wdt` 注入一个停止喂狗的任务，15s 后 `task_wdt: Task watchdog got triggered - hangs` → `Aborting` → `Rebooting` → 重新起来 `boot #N+1, reset=TASK_WDT` |
| IPP 服务抗拖死 | `ipp_print.cpp` | HTTP 行有 8 KiB 上限（否则客户端不发换行就能长期占死 631）；属性区有 30s **整体**预算（不是每字节重开 30s）；`Get-Printer-Attributes` 响应实测 2,519 / 8,192 B，不会溢出退化成错误响应；listen backlog 提到 8；打印机未就绪回 `device-error(0x0504)`、并发作业回 `busy(0x0507)`，语义正确 |
| 作业不会无限挂起 | `net_print.cpp` | 背压连续 120s 不恢复 = USB 消费端卡住，直接判作业失败并释放 631（原来只能等 20 分钟 `MAX_JOB_MS`）；`total==0` 就失败的作业**不再**回 `successful-ok`，避免「客户端以为印了、纸上是空的」 |
| USB 卡死自恢复 | `usb_printer.cpp` | 单次 4 KiB transfer，30s 未完成 → `endpoint_halt` + `flush` → 等回调回收 DMA → 3s 后 `esp_restart()`；写入失败（非拔线）同样重启自救，不会永久停在故障态 |
| PSRAM spool 吃满 8MB | `spool.cpp` | 按实测可用量分配 `min(8MB, free-1MB-64KB)`，分配后**实测复查**是否仍剩 1MB，不够就按缺额精确缩容；N16R8 实测 **7,192,576 B（85.7%），预留 1,175,096 B**；写入长度 = min(空闲, 连续段, 4KiB)；URF/PWG 大 raster 由 90%/70% 滞回背压压住 TCP 窗口，不会丢也不会顶满 |
| 1MB PSRAM 硬保留（防随机崩） | `spool.cpp` / `sysmon.cpp` | 开机强校验红线；运行期 200 ms 巡检，free < 288 KB 强制停收（收紧 TCP 窗口）、回升到 384 KB 解除；持续 20 s 临界转受控重启；`heap_caps_register_failed_alloc_callback()` 记录 OOM 的 size/caps/调用者 |
| 打印作业不乱序 / 不重复打印 | `net_print.cpp` / `ipp_print.cpp` | 一次只接一个作业（`s_busy` 原子 test-and-set）；收包与发送解耦；**IPP 文档收齐即回 `successful-ok`（job-state=processing），spool 后台继续排空**——等 USB 吐完再回会让大 raster 作业触发客户端 HTTP 超时并重发整份文档；作业级超时 20 分钟 / 32 MiB、空闲 60s；已送出字节后绝不回错误码 |
| 热插拔 | `usb_printer.cpp` | `DEV_GONE` 后等 in-flight transfer 回收再 release/close；重新插入自动重新枚举 |
| 内存 / 崩溃 | `sysmon.cpp` | **内部堆**（不是含 PSRAM 的总堆）空闲 < 15 KB 或最大连续块 < 4 KB 时主动重启；NVS 记录启动次数与上次复位原因，`/api/status` 可查（`TASK_WDT` / `PANIC` / `BROWNOUT` 等） |

---

## 八、故障排查

| 现象 | 检查 |
|---|---|
| 串口没有 `打印机就绪`，一直「等待 USB 打印机」 | ① 用错 USB 口（必须原生 OTG 口）② OTG 焊桥未短接 ③ 充电线 ④ 打印机未开机 ⑤ Arduino core 被升到 3.3.11 触发回归 ⑥ `usb_host_install` 报错说明 CDC/MSC/DFU 没关干净 |
| Web 显示 USB 就绪但打印没反应 | 用 `python scripts/ipp_probe.py <IP>` 看 IPP 能力响应；确认打印机 URL 是 `http://<IP>:631/ipp/print`；看串口有没有 `[ipp] op=0x0002` 与「正在接收打印作业…」「作业完成」 |
| 「扫描附近网络」失败 | 已改为单一 Wi-Fi 任务异步扫描 + 4 次重试；若仍失败，**直接手动填 SSID 和密码即可**（扫描非必需）。串口会打印 `scan start FAILED r=-2 mode=... status=...` 帮助定位 |
| Windows 找不到打印机 | 确认电脑和设备在同一网段（或连着设备热点）；mDNS 在 AP 与 STA 两侧都发布 `_ipp._tcp`；也可用 PowerShell 手动 `Add-PrinterPort` |
| 打印队列一直不消 / 报「脱机」 | IPP 端口不要启用 SNMP 状态；驱动选系统自带的 IPP Class Driver，不要装 HP V4 驱动包 |
| 打印很慢 / 大作业很久 | USB Full Speed 12 Mbps 上限；URF/PWG raster 体积本来就大，由 spool 背压平滑，属正常；想加快可让客户端用 300dpi 或 PDF |
| spool 峰值长期贴 90%、背压次数一直涨 | 正常：USB 比 Wi-Fi 慢，背压正在发挥作用，数据没有丢；想减少可调大 `SPOOL_TARGET_BYTES` 或减小作业体积 |
| 串口显示 `internal RAM spool 32768 bytes` | PSRAM 没启用：确认 `board = esp32-s3-devkitc1-n16r8`、编译输出用的是 `qio_opi` 库变体，或板子实际不带 PSRAM；此时仍能打印，只是缓冲小 |
| 反复重启 | 看 `/api/status` 的 `reset` 与 `boot`；`TASK_WDT` 说明某任务卡死，`BROWNOUT` 说明供电不足（换 5V 电源）；`pending_restart` 会给出上次可控重启原因 |
| 改了 Wi-Fi 密码连不上 | AP 仍在，`192.168.4.1` 进去重填；「清除 Wi-Fi 配置」可回到纯 AP 模式 |

### 串口诊断命令（115200，行尾回车）

`ENABLE_SERIAL_DEBUG_CMDS=1`（默认开）时，串口直接敲命令即可，不需要联网：

| 命令 | 作用 |
|---|---|
| `stat` | 打印任务数 / 内部堆 / PSRAM / spool 容量占用 / OOM 次数 |
| `reboot` | 受控重启（会先打印原因，3s 后重启） |
| `wdt` | **看门狗自测**：创建一个订阅了 TWDT 但停止喂狗的任务，15s 后应看到 `task_wdt: ... hangs` → `Aborting` → `Rebooting`，重启后 `boot #` 递增且 `reset=TASK_WDT` |

实测输出（COM10）：

```
[stat] int free 212224 largest 167924 | psram free 1162360 | spool 0/7192576 peak 0 | alloc fails 0
[selftest] 受控重启
[sysmon] restart requested: serial cmd
rst:0xc (RTC_SW_CPU_RST),boot:0x28 (SPI_FAST_FLASH_BOOT)
[sysmon] TWDT 15s, boot #3, reset=SW
```

```
E (27391) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (27391) task_wdt:  - hangs (CPU 0/1)
E (27391) task_wdt: Aborting.
Rebooting...
[sysmon] TWDT 15s, boot #3, reset=TASK_WDT
```

这两条就是「能不能自恢复」的直接证据：**受控重启**与**看门狗 panic 重启**都能把整机拉回可用状态。

---

## 九、已知限制

- 主通路是 **IPP 631**，**没有任何其它打印端口**（9100 RAW 已移除）。没有 LPR / 扫描 / 双向打印机状态回传 / SNMP。
- IPP 只实现单文档 `Print-Job`；`Create-Job` + `Send-Document`（多文档作业）返回 `server-error-operation-not-supported`。Windows 与 AirPrint 都走 Print-Job。
- 不实现打印认证与加密，请只在可信局域网使用，**不要把 631 / 80 端口映射到公网**。
- 一次一个打印作业，第二个连接会被立即拒绝（这是刻意的：简单且不会交错）。
- 不做打印数据重试：USB 失败后不回错误码（避免客户端重发整份文档），但也不会续打半页，需要人工重新提交。
- spool 是易失内存：掉电或重启会丢掉尚未送入 USB 的字节，需要客户端重发；不做断点续打，也不把作业写到 flash（避免磨损与掉电损坏）。
- spool 为单个作业独占，第二个连接仍会被立即拒绝。
- M203d 的 USB 是 Hi-Speed，ESP32-S3 只能 Full Speed，属于硬件约束。
- **板子已实机烧录（COM10）且启动日志正常**，但 M203d 尚未接入 OTG 口、也**未做真实打印验证**。USB 枚举逻辑与多个已实测开源项目一致；IPP 能力声明可用 `scripts/ipp_probe.py` 先验证，再打一张测试页。

## 十、v1.2.0：移除 RAW 9100 与负载优化

> **v1.3.0 已恢复 RAW 9100**（双通路并存）。本节记录的是当时的取舍与优化，
> 其中「读块上限 / 整行上限 / 超时收敛」等措施仍然有效。

### 移除的东西

RAW 9100 对这台机器毫无价值：它没有作业边界（不知道一份文档从哪结束）、没有格式协商、没有错误回执，M203d 的免驱能力完全由 IPP 631 提供。留着它等于白养一个监听 socket、一个 6 KB 栈的任务，以及一条和 IPP 共享 `netPrintRunJob` 的并发入口。所以整条通路被删干净：

| 删除项 | 原来在哪 |
|---|---|
| `ENABLE_RAW_9100` / `PRINT_PORT` 两个宏 | `config.h` |
| 9100 监听 socket、`closeFd()`、`ensureListener()`、`rawRead()`、`handleRawJob()`、`printTask()` | `net_print.cpp`（共约 100 行） |
| `print9100` 任务（6 KB 栈 + 优先级 3） | `net_printBegin()` |
| `NetPrintStatus::rawListening` / `ippListening` / `protocol` 三个字段 | `net_print.h` |
| mDNS `_pdl-datastream._tcp` 服务 | `wifi_manager.cpp` |
| `/api/status` 的 `print.raw`、网页「协议 / RAW」标签 | `web_ui.cpp` |

`netPrintRunJob()` 仍然保留（它就是 IPP 的作业泵），只是现在只有一个调用者。

验证方式不是「看代码」，而是扫二进制：`M203D_merged.bin` 里已经找不到 `pdl-datastream` / `rawListening`，`RAW` / `9100` 只剩网页里那句给用户看的「本固件只开 631，已彻底移除 9100 RAW 端口」，协议字符串 `_ipp._tcp` / `/ipp/print` / `image/urf` 都还在。

### 顺手做的四件优化

| # | 优化 | 效果 |
|---|---|---|
| 1 | **spool 消费者空闲改事件唤醒**（`net_print.cpp`） | 原来没有作业时也是 `vTaskDelay(1)` 死转 = 每秒 1000 次唤醒、24 小时 8640 万次。改成 `ulTaskNotifyTake(pdTRUE, 100ms)`，生产者开工/清 spool 时 `xTaskNotifyGive` 叫醒它。空闲唤醒降到每秒 10 次，作业开始那条路径还省掉 100 ms 空窗 |
| 2 | **IPP accept 改 `select()` 等待**（`ipp_print.cpp`） | 原来 `accept` 失败就 `delay(10ms)`，每秒 100 次空转。现在 `select` 最多等 200 ms，有 SYN 立刻醒 —— 唤醒降到每秒 5 次，**连接建立延迟反而更低**（不再最多等 10 ms 才 accept） |
| 3 | **USB 传输完成改事件通知**（`usb_printer.cpp`） | 一次 4 KB bulk 传输原来要 `vTaskDelay(1)` 轮询约 4 次。完成回调里直接 `xTaskNotifyGive` 等待者，5 ms 仅作兜底。作业期间的空转唤醒大幅减少，写完立刻进下一包 |
| 4 | **`/api/status` 去掉 String 拼接**（`web_ui.cpp`） | 网页每 5 秒拉一次状态，原来一次请求要做几十次 `String` 拼接 → 几十次 `malloc/free`。这是长期运行下内部 RAM 碎片（进而 OOM）的主要来源之一。改成静态 `json[2048]` + `vsnprintf` 追加写入：**零堆分配**，溢出时会打一条告警而不是静默截断 |

代价与取舍：静态 JSON 缓冲吃掉 2 KB `.bss`（RAM 从 59,272 B 涨到 61,312 B），换来的是运行期零碎片；删掉 `print9100` 任务省下 6 KB **内部堆**，净收益 +4 KB 内部 RAM，并且少一个每秒被调度器扫到的任务。

### 复验方法（板子插回来后）

```
python -m esptool --chip esp32s3 --port COM10 --baud 460800 write-flash 0x0 firmware\M203D_merged.bin
```

启动日志里应当**没有** `RAW 兼容端口已监听 TCP 9100` 这一行；串口敲 `stat` 会看到任务数比 v1.1.0 少 1、`int free` 比 v1.1.0 的 211,692 B 多约 6 KB。



## 十一、v1.3.0：双通路 + 小环形缓冲 + 被动 mDNS

这一版是一次结构性调整，目标是把「占用」压下去、把「边界」说清楚。

### 12.1 两条打印通路

| 通路 | 端口 | 作业边界 | 谁在用 |
|---|---|---|---|
| RAW / PDL-Datastream | 9100 | TCP 连接关闭 = 作业结束；60s 无新数据也按完成收尾 | Windows「TCP/IP 端口 + Raw」、脚本直接 socket |
| IPP | 631 `/ipp/print` | IPP Print-Job 的 document 区结束 | AirPrint、Windows IPP Class Driver、`lp`/CUPS |

两条通路各有一个监听任务（`raw9100` / `ipp631`），共用同一个作业泵和同一块环形缓冲。
**任何时刻只允许一份作业**：

- IPP：第二个 `Print-Job` 立刻收到 `0x0507 server-error-busy` + `Retry-After: 30`；
- RAW：第二个连接被直接关闭（RAW 没有协议层可以回错误码），客户端打印后台会自动重试。

accept 与「跑作业」是分开的任务 + 深度 1 的队列，所以第二个连接**不会**堵在 backlog 里占内存。
在跑作业期间，IPP 的属性查询（Get-Printer-Attributes / Identify）照常响应。

### 12.2 环形缓冲参数（`src/config.h`）

| 参数 | 值 | 理由 |
|---|---|---|
| 环形缓冲 | 256 KB = 4 × 64 KB 固定块池（PSRAM） | 只覆盖一次 USB 传输的突发，不需要整包 |
| 高 / 低水位 | 75% / 25% | 50 个百分点的滞回，不在阈值附近反复开关 |
| 读块 | 8 KB | 贴近 lwIP 的接收窗口，减少拷贝次数 |
| USB 传输块 | 16 KB × 2 双缓冲 | 一个在飞时另一个已排队，总线不空转 |
| 缓冲分配 | 固定块池，运行期零 `malloc/free` | 杜绝碎片，`alloc_fails` 恒为 0 |
| 空闲超时 | 60s | 客户端不关连接也不发数据时按完成收尾，不重发作业 |

背压的要点是**不丢数据**：到 75% 就停止 `recv`，让 TCP 接收窗口自己收缩，客户端由 TCP 降速。
缓冲占用从 v1.2.0 的 7.2 MB（85.7% PSRAM）降到 256 KB（3%），PSRAM 余量回到 8.1 MB。

### 12.3 双核分工

| 核 | 任务 |
|---|---|
| 0 | Wi-Fi/LwIP 协议栈、`raw9100`、`raw-job`、`ipp631`、`ipp-job`、`snmp161`、`mdns5353` |
| 1 | `usb-daemon`、`usb-client`、`spool-writer`、`sysmon`、Web（loopTask） |

USB 侧的忙等与 DMA 搬运永远不占 Wi-Fi 所在的核 0；网络侧再忙也只是让 TCP 窗口收缩。
实测启动后：内部堆 free 147,968 B（最大连续 104,436 B）、PSRAM free 8,098,756 B、`alloc fails 0`。

### 12.4 USB：16 KB 双缓冲乒乓

`usbPrinterSubmit()` 只排队不等，`usbPrinterWait()` 回收最早提交的那一块，严格 FIFO。
短包（`actual < num_bytes`）时，第二块在环里的起点与设备实际收到的字节流已经错位，
所以直接 `usbPrinterAbort()`：halt + flush + clear 端点，整份作业判失败让客户端重发——
绝不把错位的数据继续喂给打印机。

### 12.5 SNMP 161（只读）

- 只处理 GET / GETNEXT，SET 直接丢弃；community 在 `config.h`（默认 `public`）；
- `sysDescr` / `sysUpTime` / `sysName` / `hrDeviceStatus` / `hrPrinterStatus` /
  `hrPrinterDetectedErrorState` / `prtGeneralPrinterName` / `prtMarkerSuppliesLevel` 等；
- **状态唯一来源是 IPP 的 `printer-state`**（`ippPrinterState()`：3 idle / 4 processing / 5 stopped），
  不另建状态机。拿不到的项（真实页数、墨粉余量、序列号）如实报 0 / `-3 unknown` / 空串，不编数据；
- 独立 UDP socket + 独立任务，和 9100/631 完全不复用。

验证：`snmpwalk -v2c -c public <IP> 1.3.6.1.2.1.25.3`，打印中 `hrPrinterStatus` 应为 4。

### 12.6 被动 mDNS

ESPmDNS 的 TTL 硬编码 120s，会在 TTL/2 时周期性发 unsolicited announce（每 60s 一轮组播），
无法通过公开 API 关掉。v1.3.0 换成了自己写的 responder（`mdns_responder.cpp`）：

- 加入 224.0.0.251:5353，**只在收到查询时应答**，从不 probe、从不主动广播；
- 支持 `_services._dns-sd._udp`（服务枚举）、`_ipp._tcp`、`_pdl-datastream._tcp`、`_http._tcp`
  的 PTR / SRV / TXT / A，SRV 的 target A 记录放在 additional 里，省掉客户端第二轮查询；
- TXT 每次应答时实时读 `cfgDuplexEnabled()` / `cfgUrfString()`，双面开关改了立刻生效，不存在缓存不一致；
- 忽略来自 5353 端口的包（其它 responder 的响应），避免组播风暴。

### 12.7 AP 自动关闭

| 条件 | 动作 |
|---|---|
| STA 连续在线满 60s | `softAPdisconnect(true)` 关闭热点 |
| STA 掉线满 3 分钟 | 重开热点（`PrintSrv-XXXXXX` / 192.168.4.1）供重新配网 |
| 从未配网 | AP 一直开着，不受影响 |

### 12.8 双面开关与作业收尾

- Web 上的「双面打印」开关存 NVS，改动立刻生效（`/api/duplex`）：`sides-supported`、`sides-default`、
  Bonjour `duplex=T/F`、URF 的 `DM3/DM1` 四处同源；
- 关着时客户端仍要双面 → 回 `successful-ok-ignored-or-substituted-attributes` + `unsupported-attributes: sides`；
- 正常作业结束追加 `UEL (ESC%-12345X) + FF`，防止下一页与前一份的格式残留粘页；
- 异常中止走 abort，并回报 `job-state=8 (aborted)` + `aborted-by-system`。

### 12.9 关于「加大 lwIP 接收窗口」

当前固件**仍使用默认 5760 B**，但结论不是「改不了」，而是「这台机器上暂时改不了」，如实记录：

- Arduino-ESP32 的 lwIP 是预编译的 `liblwip.a`，`CONFIG_LWIP_TCP_WND_DEFAULT` 在
  `framework-arduinoespressif32-libs` 里固定为 **5760 B**，只改头文件宏不会改变库行为；
- 但 pioarduino（本工程用的 55.03.30）**支持**在 Arduino 框架下重建这份库：
  在 `[env:esp32s3]` 里加 env 级 `custom_sdkconfig = sdkconfig.defaults`，
  `arduino.py` 会调用 `call_compile_libs()` 重新编译 IDF 组件（含 lwIP）。
  **注意键名是 `custom_sdkconfig`，不是 `board_build.custom_sdkconfig`**；
- 该路径需要 **cmake + ninja**。本机 `packages/tool-cmake/bin` 与 `tool-ninja` 只有
  `package.json`（没有可执行文件），系统也未安装；联网补装 12 分钟无进展
  （GitHub 下载被阻断），因此本次未启用，`sdkconfig.defaults` 已移除。

当前实测与取舍：

- 5760 B 窗口在 RTT 5 ms 时理论上限约 9.2 Mbps（1.15 MB/s），已高于 USB Full Speed 的
  实际吞吐（0.7–1.0 MB/s），**所以局域网内不是瓶颈**；只有 RTT > 7 ms 才会限制；
- 真正决定出纸速度的是 USB FS 与打印引擎：300 dpi sgray 的 A4 单页约 8.7 MB，
  光传 USB 就要 9–12 秒；
- 运行时能做的都已做：`TCP_NODELAY`（去掉小尾巴等 ACK 的 RTT）、
  `SO_RCVBUF=32768`（受 `TCP_WND` 上限约束）、8 KB 读块、连续收包不额外 sleep。

若以后要改：补上 cmake/ninja → 放回 `sdkconfig.defaults`（`CONFIG_LWIP_TCP_WND_DEFAULT=32768`
+ `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=32768`）→ 加 `custom_sdkconfig` → 全量重建（约 20–40 分钟）。
删掉这两项即可回退，代码无需改动。

### 12.10 验证清单

```
# 1) 串口（115200）：应看到 9100 / 631 / 161 三行监听 + ring 262144 B
stat                      # tasks / int free / psram free / spool 0/262144 / alloc fails 0

# 2) RAW 通路（Windows 或 Linux）
printf '%%!PS-Adobe-3.0\nshowpage\n' | nc <IP> 9100

# 3) IPP 属性
curl -s http://<IP>:631/ipp/print -o /dev/null -w '%{http_code}\n'   # 探测页 200

# 4) SNMP
snmpget -v2c -c public <IP> 1.3.6.1.2.1.25.3.5.1.1.1     # 3 idle / 4 printing

# 5) mDNS（不主动广播，只在查询时回）
dns-sd -B _ipp._tcp
```



## 十二、引用、致谢与许可

### 12.1 参考过的公开实现

架构调研阶段参考过以下项目的公开实现，**本工程代码为重新编写，未复制其源码**：

- [WARMW00D/ESP32-S3-PrintServer](https://github.com/WARMW00D/ESP32-S3-PrintServer)（MIT）
- [NitsuaFox/thermal-bridge](https://github.com/NitsuaFox/thermal-bridge)（MIT）
- [B4nd1/esp32-s3-printer](https://github.com/B4nd1/esp32-s3-printer)

### 12.2 运行时与构建期依赖

| 项目 | 许可证 | 用途 |
|---|---|---|
| [Arduino-ESP32](https://github.com/espressif/arduino-esp32) | LGPL-2.1 | `WiFi` / `WebServer` / `Preferences` 等基础框架 |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Apache-2.0 | USB Host 栈（`usb_host_*`）、lwIP、FreeRTOS、PSRAM 驱动 |
| [PlatformIO](https://platformio.org/) | Apache-2.0 | 构建系统（双环境 release / debug） |
| [esptool](https://github.com/espressif/esptool) | GPL-2.0 | 烧录与镜像合并（仅开发机使用，不进入固件） |
| [pyserial](https://github.com/pyserial/pyserial) | BSD-3-Clause | 验收脚本读取串口 |

### 12.3 参考的公开规范

协议实现均照规范自写：

| 规范 | 用在哪 |
|---|---|
| **RFC 8011**（IPP/1.1） | 属性编码、`Print-Job`、`Get-Printer-Attributes`、错误码、`unsupported-attributes` 组 |
| **PWG 5100.14**（IPP Everywhere） | `document-format-supported`、`urf-supported`、`ipp-features-supported`、PWG Raster 能力 |
| **RFC 6762 / 6763**（mDNS / DNS-SD） | 5353 responder：PTR / SRV / TXT / A 记录与报文格式 |
| **USB Printer Class 1.1** | `GET_DEVICE_ID`（bRequest 0x00）、`GET_PORT_STATUS`（0x01）、Bulk OUT |
| **IEEE 1284 Device ID** | `MFG / MDL / CMD / DES / SN / CLS` 字段解析与能力推导 |
| **Apple URF** | `image/urf` 与 URF 能力串（`DM3` / `DM1` 等） |
| **HP PJL 技术参考** | `@PJL SET DUPLEX / BINDING`、`UEL`（`ESC%-12345X`）作业边界 |

### 12.4 实测样机

**HP LaserJet Pro M203d**（USB `03F0:632A`）：RAW 出纸、双面识别、缺纸检测、IPP 属性、AirPrint 发现均已实机验证。感谢 HP 公开 1284 Device ID 与 PJL 文档，让「去机型化」可以靠标准字段实现，而不是维护机型白名单。

### 12.5 商标声明

HP、LaserJet 是 HP Inc. 的商标；AirPrint、macOS、iOS 是 Apple Inc. 的商标；Windows 是 Microsoft Corporation 的商标。本项目与上述公司无隶属关系，仅做协议互通。

### 12.6 许可

代码以 **MIT License** 发布，见 [LICENSE](LICENSE)。固件按「尽力而为」提供，因打印质量、纸张、硬件差异造成的损失不承担责任。
