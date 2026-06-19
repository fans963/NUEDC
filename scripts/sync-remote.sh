#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"

require_board_host
require_install_dir

sync() {
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

info "监听 $INSTALL_DIR 变化 (Ctrl+C 退出)..."
prev_hash=""
while true; do
    if [[ -d "$INSTALL_DIR" ]]; then
        cur_hash=$(find "$INSTALL_DIR" -printf '%T@ %p\n' 2>/dev/null | md5sum | cut -d' ' -f1)
        if [[ "$cur_hash" != "$prev_hash" ]]; then
            prev_hash="$cur_hash"
            sync
        fi
    fi
    sleep 1
done
