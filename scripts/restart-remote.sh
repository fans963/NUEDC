#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CACHE_FILE="$PROJECT_DIR/.cache/board-host"
SERVICE_NAME="nuedc"

if [[ ! -f "$CACHE_FILE" ]]; then
    echo "未找到设备缓存，请先运行: ./scripts/search-device.sh" >&2
    exit 1
fi

BOARD_HOST="$(cat "$CACHE_FILE")"

echo "重启 $BOARD_HOST 上的 $SERVICE_NAME ..."
ssh "$BOARD_HOST" "sudo systemctl restart $SERVICE_NAME"
echo "已重启 $SERVICE_NAME"
