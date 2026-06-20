#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"

require_board_host
require_install_dir

CONFIG_SRC="$PROJECT_DIR/config/config.yaml"
CONFIG_DST="$INSTALL_DIR/config/config.yaml"

sync_config() {
    # 源 config 比 install 里的新则覆盖
    if [[ -f "$CONFIG_SRC" ]] && [[ ! -f "$CONFIG_DST" || "$CONFIG_SRC" -nt "$CONFIG_DST" ]]; then
        mkdir -p "$(dirname "$CONFIG_DST")"
        cp "$CONFIG_SRC" "$CONFIG_DST"
        info "config.yaml 已更新到 install 目录"
    fi
}

sync() {
    sync_config
    info "同步到 $BOARD_SSH:$REMOTE_DIR ..."
    rsync -avz --delete \
        --exclude='.git' \
        "$INSTALL_DIR/" \
        "$BOARD_SSH:$REMOTE_DIR/"
    info "同步完成"
}

sync

# --once 模式：只同步一次，不监听变化
[[ "${1:-}" == "--once" ]] && exit 0

info "监听 $INSTALL_DIR 和 config/config.yaml 变化 (Ctrl+C 退出)..."
prev_hash=""
while true; do
    # 计算 install 目录 + 源 config 的联合哈希
    hash_input=""
    if [[ -d "$INSTALL_DIR" ]]; then
        hash_input+=$(find "$INSTALL_DIR" -printf '%T@ %p\n' 2>/dev/null)
    fi
    if [[ -f "$CONFIG_SRC" ]]; then
        hash_input+=$(stat -c '%Y %n' "$CONFIG_SRC" 2>/dev/null)
    fi
    cur_hash=$(echo "$hash_input" | md5sum | cut -d' ' -f1)

    if [[ "$cur_hash" != "$prev_hash" ]]; then
        prev_hash="$cur_hash"
        sync
    fi
    sleep 1
done
