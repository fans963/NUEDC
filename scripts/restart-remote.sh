#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"

require_board_host

info "重启 $BOARD_SSH 上的 $SERVICE_NAME ..."
ssh -tt "$BOARD_SSH" "sudo systemctl restart $SERVICE_NAME"
info "已重启 $SERVICE_NAME"
