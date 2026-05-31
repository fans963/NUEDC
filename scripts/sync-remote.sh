#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CACHE_FILE="$PROJECT_DIR/.cache/board-host"
INSTALL_DIR="$PROJECT_DIR/build/install"
REMOTE_DIR="/opt/nuedc"

if [[ ! -f "$CACHE_FILE" ]]; then
    echo "未找到设备缓存，请先运行: ./scripts/search-device.sh" >&2
    exit 1
fi

BOARD_HOST="$(cat "$CACHE_FILE")"

if [[ ! -d "$INSTALL_DIR" ]]; then
    echo "错误: 构建产物不存在，请先构建: cd build && cmake --install . --prefix ./install" >&2
    exit 1
fi

sync() {
    echo "[$(date +%H:%M:%S)] 同步到 $BOARD_HOST:$REMOTE_DIR ..."
    rsync -avz --delete \
        --exclude='.git' \
        "$INSTALL_DIR/" \
        "$BOARD_HOST:$REMOTE_DIR/"
    echo "[$(date +%H:%M:%S)] 同步完成"
}

# 首次同步
sync

# 监听变化自动同步
echo "监听 $INSTALL_DIR 变化 (Ctrl+C 退出)..."
while inotifywait -qr -e modify,create,delete,move "$INSTALL_DIR"; do
    sleep 0.5
    sync
done
