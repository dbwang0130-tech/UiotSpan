#!/bin/zsh
# 一键:编译 → 烧录 → 看门狗复位启动(原生 USB S3 烧完会卡下载模式,必须这样复位)
set -e
export PATH="/opt/homebrew/bin:$PATH"
DIR="$(cd "$(dirname "$0")" && pwd)"
FQBN="esp32:esp32:esp32s3:PartitionScheme=huge_app,CDCOnBoot=cdc,PSRAM=opi"
ESPTOOL=$(ls ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | head -1)
cd "$DIR"
echo "==> 1/3 编译"; arduino-cli compile -b "$FQBN" .
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
echo "==> 2/3 烧录 $PORT"; arduino-cli upload -b "$FQBN" -p "$PORT" .
sleep 1; PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
echo "==> 3/3 看门狗复位启动"
"$ESPTOOL" --chip esp32s3 -p "$PORT" --before default_reset --after watchdog_reset flash_id >/dev/null 2>&1
echo "==> 完成"
