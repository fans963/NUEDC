#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"

require_board_host
require_install_dir

SERVICE_FILE="$SCRIPT_DIR/nuedc.service"

info "=== 同步程序文件 ==="
"$SCRIPT_DIR/sync-remote.sh" --once

info "=== 部署 systemd 服务 ==="
scp "$SERVICE_FILE" "$BOARD_SSH:/tmp/$SERVICE_NAME.service"

ssh -tt "$BOARD_SSH" 'bash -s' << EOF
sudo mv /tmp/$SERVICE_NAME.service /etc/systemd/system/$SERVICE_NAME.service
sudo systemctl daemon-reload
sudo systemctl enable $SERVICE_NAME.service
sudo systemctl restart $SERVICE_NAME.service
echo "=== 服务状态 ==="
sudo systemctl status $SERVICE_NAME.service --no-pager
exit
EOF

info "=== 部署完成 ==="
info "查看日志: ssh $BOARD_SSH 'journalctl -u $SERVICE_NAME -f'"
