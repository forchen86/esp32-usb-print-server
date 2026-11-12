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

### RAW 9100 上的双面打印

RAW 只有一条裸字节流，没有 IPP 那样的属性协商，固件在这条通路**不做任何注入**——双面指令由客户端驱动写进数据流，固件原样转发；页面识别出的双面能力只影响 IPP 属性与状态显示，不改变 RAW 字节流。

要点：驱动须用打印机**原厂 PCL / PS**（`Generic / Text Only`、`Microsoft IPP Class Driver` 都不会发双面指令）；装好后到「打印机属性 → 设备设置 → 可安装选项」把**双面打印单元**设为**已安装**（固件无双向通道，驱动探测不到，不声明则双面选项是灰的）。若仍只出单面，抓 9100 数据流搜 `DUPLEX`——搜不到就是驱动没发指令，问题在客户端不在固件。

---


## 支持的 ESP32 板型

### 板型对照

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

### 接线、供电与常见坑

- **必须有独立的原生 USB-OTG 口**（GPIO19 = D-、GPIO20 = D+）和**另一个 UART/USB 转串口口用于烧录**。双 Type-C 板子要认清丝印：标 `USB/OTG` 的接打印机，标 `COM/UART` 的接电脑。
- 很多板子（尤其 SuperMini / DevKitC 兼容板）背面有 **OTG 焊桥**需要短接，否则 USB Host 永远枚举不到设备。查你板子的原理图确认。
- **打印机必须自己供电**（接市电），USB 线只传数据。ESP32-S3 板子由独立 5V 电源供电；VBUS 只作为「插入检测」，不要指望板子给打印机供电。
- 需要 OTG 转接线（USB-C 母 ↔ USB-B / A 母），必须是**数据线**，充电线不行。
- 2.4GHz Wi-Fi（ESP32-S3 不支持 5GHz）。

上电顺序建议：先给打印机上电 → 给板子上电 → 再插 USB 数据线；或直接插好再一起上电。

---


## 编译与烧录

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



### 用 PlatformIO 直烧

```bash
pio run -t upload --upload-port COM7      # Windows
pio run -t upload --upload-port /dev/ttyUSB0
pio device monitor -b 115200
```

### 用 esptool 手动烧

```bash
# 整片（推荐）：bootloader + 分区表 + app 一次写完
esptool.py --chip esp32s3 --port COM7 --baud 460800 write-flash 0x0 firmware/release/merged.bin

# 或只升级固件（保留 Wi-Fi / 双面配置）
esptool.py --chip esp32s3 --port COM7 --baud 460800 write-flash 0x10000 firmware/release/firmware.bin
```

串口（115200）出现 `[usb] 打印机就绪 VID:...` 即 USB 枚举成功。

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


## 使用

### 第一次开机与配网

1. 按「编译与烧录」一章烧录后，打印机接市电，USB 数据线插到板子的 **OTG** 口（不是 COM/UART 那个）。
2. 上电，等约 10 秒。串口 115200 会打印 `[usb] 打印机就绪 VID:...`。
3. 手机/电脑连热点 **`PrintSrv-XXXXXX`**（默认无密码）。
4. 浏览器打开 **`http://192.168.4.1`**。
5. 「网络」卡片 → 「扫描」或手动填 SSID/密码 → 「保存并连接」。
6. 连上后页面「路由器分配的地址」会变成 `192.168.x.x`，**60 秒后热点自动关闭**。
7. 之后改用 `http://<该地址>/` 或 `http://printserver.local/` 访问。

> 想换 Wi-Fi：点页面「清除配置」，热点会立刻重开。STA 掉线满 3 分钟也会自动重开，但主动清除更快。

### Windows：RAW 9100（推荐，最省事）

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

### Windows：IPP 631（免驱）

用页面「怎么在电脑上添加」里生成的两行命令，**先建端口再加打印机**（少了 `Add-PrinterPort` 会失败）：

```powershell
Add-PrinterPort -Name "http://192.168.10.43:631/ipp/print"
Add-Printer -Name "HP-LaserJet-M203d" -DriverName "Microsoft IPP Class Driver" -PortName "http://192.168.10.43:631/ipp/print"
```

也可以图形操作：「我需要的打印机不在列表中」→「按名称选择共享打印机」→ 粘贴 `http://192.168.10.43:631/ipp/print`。

> 只会 PCL 的机型建议走 5.2 的 RAW —— IPP 客户端可能发来打印机不认识的 URF / PWG raster。

### macOS / iOS（AirPrint）

- 同一局域网；macOS「系统设置 → 打印机与扫描仪 → +」里会出现 **HP LaserJet M203d**，直接添加。
- iPhone / iPad 打印时在打印机列表里选它。
- 搜不到先查路由器：**AP 隔离 / 客户端隔离 / 组播过滤**会阻断 mDNS（5353），关掉即可。
- 本机 mDNS 是「被问才答」、不主动广播，首次发现可能比商业打印机慢 1~2 秒，属正常。

### Linux / CUPS

```bash
# 免驱（IPP Everywhere）
lpadmin -p m203d -E -v ipp://192.168.10.43:631/ipp/print -m everywhere

# 或走 RAW（需要对应 PPD）
lpadmin -p m203d -E -v socket://192.168.10.43:9100 -m <你的 PPD>
```

### 双面打印怎么开

| 通路 | 双面由谁负责 | 要做的事 |
|---|---|---|
| **RAW 9100** | **客户端驱动** | 驱动里声明「双面打印单元已安装」，打印时选双面。固件纯透传、不注入 |
| **IPP 631** | **固件** | 页面「双面打印」设为「自动」或「总是双面」，并保持「自动补发双面指令」开启 |

「自动」的判定依据会显示在页面上（例：`型号以 d 结尾，通常代表双面机型（DES）`）。识别不准就手动切「总是双面 / 总是单面」。

### 升级固件（保留配置）

```bash
APP_ONLY=1 bash scripts/flash.sh COM11 release
```

只写 `0x10000` 的 app 分区，NVS 里的 Wi-Fi 与双面设置都保留。整片烧 `merged.bin` 会清掉这些配置。

### 排障速查

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

### 日志与诊断

- **release 版**：串口只有事件与异常（启动约 15 行），日常够用。
- **debug 版**：逐请求、逐作业、内存快照全开，排障时临时刷：
  ```bash
  bash scripts/flash.sh COM11 debug
  ```
- 自动验收：`python scripts/verify_firmware.py`（release 模式）或加 `--debug` 验证调试版。

---


## 故障排查

| 现象 | 检查 |
|---|---|
| 串口没有 `打印机就绪`，一直「等待 USB 打印机」 | ① 用错 USB 口（必须原生 OTG 口）② OTG 焊桥未短接 ③ 充电线 ④ 打印机未开机 ⑤ Arduino core 被升到 3.3.11 触发回归 ⑥ `usb_host_install` 报错说明 CDC/MSC/DFU 没关干净 |
| Web 显示 USB 就绪但打印没反应 | 用 `python scripts/ipp_probe.py <IP>` 看 IPP 能力响应；确认打印机 URL 是 `http://<IP>:631/ipp/print`；换 debug 版固件复现（`bash scripts/flash.sh COM11 debug`），串口会打印逐作业日志 |
| 「扫描附近网络」失败 | 已改为单一 Wi-Fi 任务异步扫描 + 4 次重试；若仍失败，**直接手动填 SSID 和密码即可**（扫描非必需）。串口会打印 `scan start FAILED r=-2 mode=... status=...` 帮助定位 |
| Windows 找不到打印机 | 确认电脑和设备在同一网段（或连着设备热点）；mDNS 在 AP 与 STA 两侧都发布 `_ipp._tcp`；也可用 PowerShell 手动 `Add-PrinterPort` |
| 打印队列一直不消 / 报「脱机」 | IPP 端口不要启用 SNMP 状态；驱动选系统自带的 IPP Class Driver，不要装 HP V4 驱动包 |
| 打印很慢 / 大作业很久 | USB Full Speed 12 Mbps 上限；URF/PWG raster 体积本来就大，由 spool 背压平滑，属正常；想加快可让客户端用 300dpi 或 PDF |
| 缓冲占用长期贴 75%、「暂停接收」次数一直涨 | 正常：USB 比 Wi-Fi 慢，背压正在发挥作用，数据没有丢；属正常现象：USB 比 Wi-Fi 慢，背压在工作、数据没丢；想减少可让客户端用 300dpi 或 PDF |
| 页面「数据缓冲区」的总量远小于 256 KB | PSRAM 没启用：确认 `board = esp32-s3-devkitc1-n16r8`、编译输出用的是 `qio_opi` 库变体，或板子实际不带 PSRAM；此时仍能打印，只是缓冲小、大图作业易触发暂停接收 |
| 反复重启 | 看 `/api/status` 的 `reset` 与 `boot`；`TASK_WDT` 说明某任务卡死，`BROWNOUT` 说明供电不足（换 5V 电源）；`pending_restart` 会给出上次可控重启原因 |
| 改了 Wi-Fi 密码连不上 | AP 仍在，`192.168.4.1` 进去重填；「清除 Wi-Fi 配置」可回到纯 AP 模式 |

## 已知限制

- **9100 RAW 没有作业状态回传**：一个 TCP 连接就是一份作业，固件不知道内容对不对——缺纸时会直接拒绝连接，但「打出来是空白」这类内容级问题看不到。
- **IPP 只实现单文档 `Print-Job`**：`Create-Job` + `Send-Document`（多文档作业）返回 `server-error-operation-not-supported`。Windows 与 AirPrint 都走 Print-Job，不受影响。
- **不实现打印认证与加密**，请只在可信局域网使用，不要把 9100 / 631 / 80 端口映射到公网。
- **一次只打一份**：第二个连接会被立即拒绝（RAW 直接关连接、IPP 回 busy + Retry-After），这是刻意的取舍，换来简单且不会交错。
- **不做断点续打**：缓冲是易失内存，掉电或重启会丢掉尚未送入 USB 的字节，需要客户端重发；作业不落盘（避免 flash 磨损与掉电损坏）。
- **USB Full Speed 上限约 0.7–1.0 MB/s**：ESP32-S3 硬件约束，Hi-Speed 打印机也会被降到 FS，大位图作业会明显变慢。
- **透传边界**：需要固件渲染（GDI / 主机渲染型）、需要 1284.4 双向握手、需要上电固件下载的打印机不在支持范围，详见「支持的打印机范围」。

## 引用、致谢与许可

### 参考过的公开实现

架构调研阶段参考过以下项目的公开实现，**本工程代码为重新编写，未复制其源码**：

- [WARMW00D/ESP32-S3-PrintServer](https://github.com/WARMW00D/ESP32-S3-PrintServer)（MIT）
- [NitsuaFox/thermal-bridge](https://github.com/NitsuaFox/thermal-bridge)（MIT）
- [B4nd1/esp32-s3-printer](https://github.com/B4nd1/esp32-s3-printer)

### 运行时与构建期依赖

| 项目 | 许可证 | 用途 |
|---|---|---|
| [Arduino-ESP32](https://github.com/espressif/arduino-esp32) | LGPL-2.1 | `WiFi` / `WebServer` / `Preferences` 等基础框架 |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Apache-2.0 | USB Host 栈（`usb_host_*`）、lwIP、FreeRTOS、PSRAM 驱动 |
| [PlatformIO](https://platformio.org/) | Apache-2.0 | 构建系统（双环境 release / debug） |
| [esptool](https://github.com/espressif/esptool) | GPL-2.0 | 烧录与镜像合并（仅开发机使用，不进入固件） |
| [pyserial](https://github.com/pyserial/pyserial) | BSD-3-Clause | 验收脚本读取串口 |

### 参考的公开规范

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

### 实测样机

**HP LaserJet Pro M203d**（USB `03F0:632A`）：RAW 出纸、双面识别、缺纸检测、IPP 属性、AirPrint 发现均已实机验证。感谢 HP 公开 1284 Device ID 与 PJL 文档，让「去机型化」可以靠标准字段实现，而不是维护机型白名单。

### 商标声明

HP、LaserJet 是 HP Inc. 的商标；AirPrint、macOS、iOS 是 Apple Inc. 的商标；Windows 是 Microsoft Corporation 的商标。本项目与上述公司无隶属关系，仅做协议互通。

### 许可

代码以 **MIT License** 发布，见 [LICENSE](LICENSE)。固件按「尽力而为」提供，因打印质量、纸张、硬件差异造成的损失不承担责任。
