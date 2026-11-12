#!/usr/bin/env bash
# 烧录：bash scripts/flash.sh <串口> [release|debug]
# 只想升级固件、保留 Wi-Fi 配置：APP_ONLY=1 bash scripts/flash.sh <串口> [release|debug]
set -euo pipefail
cd "$(dirname "$0")/.."
PY="${PYTHON_EXE:-python3}"
PORT="${1:-}"
ENV="${2:-release}"
if [ -z "$PORT" ]; then
  echo "用法: bash scripts/flash.sh /dev/ttyUSB0 [release|debug]"
  exit 1
fi
if [ ! -d "firmware/$ENV" ]; then
  echo "[ERROR] 没有 firmware/$ENV，先跑: bash scripts/build.sh $ENV"
  exit 1
fi

if [ "${APP_ONLY:-0}" = "1" ]; then
  echo "只写 app 分区（保留 NVS 里的 Wi-Fi / 双面设置）..."
  "$PY" -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
    write-flash 0x10000 "firmware/$ENV/firmware.bin"
else
  echo "整片烧录 firmware/$ENV/merged.bin ..."
  "$PY" -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
    write-flash 0x0 "firmware/$ENV/merged.bin"
fi
echo "[OK] 烧录完成（$ENV）。串口 115200 查看日志。"
