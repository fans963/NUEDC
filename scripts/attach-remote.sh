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

echo "正在连接 $BOARD_HOST，实时查看 $SERVICE_NAME 输出 (Ctrl+C 退出)..."
ssh -t "$BOARD_HOST" "sudo journalctl -u $SERVICE_NAME -f --no-pager"
