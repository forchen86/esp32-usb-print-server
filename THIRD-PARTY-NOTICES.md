# 第三方组件声明（THIRD-PARTY NOTICES）

本仓库**自有代码**（`src/`、`scripts/`、文档）以 MIT License 发布，见 [LICENSE](LICENSE)。

仓库同时分发了**编译好的固件二进制**（`firmware/release/`、`firmware/debug/`）。这些二进制包含下列第三方组件的编译产物，本文件用于满足各许可证对"分发二进制"的要求。

## 固件二进制中包含的组件

| 组件 | 许可证 | 来源 | 在固件中的角色 |
|---|---|---|---|
| ESP-IDF（含 USB Host 栈、Wi-Fi 驱动、mbedTLS、第二阶段 bootloader） | Apache-2.0 | <https://github.com/espressif/esp-idf> | 底层运行时与 bootloader |
| lwIP（TCP/IP 栈） | BSD-3-Clause | <https://github.com/lwip-tcpip/lwip>（随 ESP-IDF 分发） | 网络协议栈 |
| FreeRTOS 内核 | MIT | <https://github.com/FreeRTOS/FreeRTOS-Kernel>（随 ESP-IDF 分发） | 任务调度 |
| wpa_supplicant | BSD-3-Clause | 随 ESP-IDF 分发 | Wi-Fi 认证 |
| Arduino-ESP32 核心（`WiFi` / `WebServer` / `Preferences` / `esp32-hal-*` 等胶水层） | LGPL-2.1 | <https://github.com/espressif/arduino-esp32> | 应用框架 |

上述组件的完整许可证文本可在各上游仓库获取；ESP-IDF 随源码附带各组件的 license 文件。

## LGPL-2.1 合规说明

固件对 Arduino-ESP32 核心（LGPL-2.1）采用静态链接。依据 LGPL-2.1 第 4/6 条，本仓库通过以下方式满足"允许用户修改库并重新链接"的要求：

1. **完整源码公开**：应用层源码（`src/`）与构建配置（`platformio.ini`）均在本仓库，任何人可获取；
2. **可复现构建**：执行 `bash scripts/build.sh`（或 `pio run -e release -e debug`）即可从源码重新构建出与本仓库分发的二进制等价的固件，无需专有工具或密钥；
3. **未修改库本身**：本工程未修改 Arduino-ESP32 / ESP-IDF 的任何库源码，全部以官方预编译形式使用。

如你修改了库代码并希望分发修改后的固件，请同样公开你的修改并允许他人重新链接。

## 仅开发期使用、不进入固件的工具

| 工具 | 许可证 | 说明 |
|---|---|---|
| PlatformIO Core | Apache-2.0 | 构建系统 |
| esptool | GPL-2.0 | 烧录与镜像合并；**只运行在开发机上，其输出文件（merged.bin）不构成该工具的衍生作品** |
| pyserial | BSD-3-Clause | 验收脚本 `scripts/verify_firmware.py` 读取串口 |

## 商标声明

HP、LaserJet 是 HP Inc. 的商标；AirPrint、macOS、iOS 是 Apple Inc. 的商标；Windows 是 Microsoft Corporation 的商标。本项目与上述公司无隶属关系，仅做协议互通。

## 参考过的公开实现（未复制代码）

架构调研阶段参考过 [WARMW00D/ESP32-S3-PrintServer](https://github.com/WARMW00D/ESP32-S3-PrintServer)、[NitsuaFox/thermal-bridge](https://github.com/NitsuaFox/thermal-bridge)、[B4nd1/esp32-s3-printer](https://github.com/B4nd1/esp32-s3-printer) 的公开实现。本工程代码为重新编写，未复制其源码，故不触发其许可证的代码保留义务。
