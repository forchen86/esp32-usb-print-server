#!/usr/bin/env bash
# 构建两套固件（同一份源码，日志级别不同）
#   bash scripts/build.sh            # release + debug 都出
#   bash scripts/build.sh release    # 只出正式版
#   bash scripts/build.sh debug      # 只出调试版
# 产物：firmware/<env>/{firmware,bootloader,partitions,merged}.bin + SHA256SUMS.txt
set -euo pipefail
cd "$(dirname "$0")/.."
PY="${PYTHON_EXE:-python3}"
TARGET="${1:-all}"

BOOT_APP0="$(find "${HOME}/.platformio/packages" -name boot_app0.bin 2>/dev/null | head -1 || true)"

build_one() {
  local env="$1"
  echo "=== build: $env ==="
  "$PY" -m platformio run -e "$env"
  mkdir -p "firmware/$env"
  cp ".pio/build/$env/firmware.bin"   "firmware/$env/firmware.bin"
  cp ".pio/build/$env/bootloader.bin" "firmware/$env/bootloader.bin"
  cp ".pio/build/$env/partitions.bin" "firmware/$env/partitions.bin"
  if [ -n "$BOOT_APP0" ] && "$PY" -m esptool version >/dev/null 2>&1; then
    "$PY" -m esptool --chip esp32s3 merge-bin -o "firmware/$env/merged.bin" \
      --flash-mode dio --flash-freq 80m --flash-size 16MB \
      0x0     "firmware/$env/bootloader.bin" \
      0x8000  "firmware/$env/partitions.bin" \
      0xe000  "$BOOT_APP0" \
      0x10000 "firmware/$env/firmware.bin" >/dev/null
    echo "[OK] firmware/$env/merged.bin"
  else
    echo "[i] 缺 esptool 或 boot_app0.bin，跳过 merged.bin"
  fi
  ( cd "firmware/$env" && sha256sum *.bin > SHA256SUMS.txt )
  echo "[OK] firmware/$env/"
}

case "$TARGET" in
  all)           build_one release; build_one debug ;;
  release|debug) build_one "$TARGET" ;;
  *) echo "用法: bash scripts/build.sh [all|release|debug]"; exit 1 ;;
esac
