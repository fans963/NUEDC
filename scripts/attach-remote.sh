#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"

require_board_host

info "正在连接 $BOARD_SSH，实时查看 $SERVICE_NAME 输出 (Ctrl+C 退出)..."
ssh -tt "$BOARD_SSH" "sudo journalctl -u $SERVICE_NAME -f --no-pager"
