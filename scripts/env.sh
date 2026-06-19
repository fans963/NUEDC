#!/usr/bin/env bash
# 公共环境配置 — 所有脚本通过 source "$(dirname "$0")/env.sh" 引入
# 参数可通过环境变量覆盖，例如: REMOTE_USER=ubuntu ./scripts/sync-remote.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# ── 可调参数（按需覆盖） ──────────────────────────────
CACHE_FILE="${CACHE_FILE:-$PROJECT_DIR/.cache/board-host}"
INSTALL_DIR="${INSTALL_DIR:-$PROJECT_DIR/build/install}"
REMOTE_USER="${REMOTE_USER:-radxa}"
REMOTE_DIR="${REMOTE_DIR:-/home/$REMOTE_USER/workspace/nuedc}"
SERVICE_NAME="${SERVICE_NAME:-nuedc}"

# ── 工具函数 ──────────────────────────────────────────

die()  { echo "错误: $*" >&2; exit 1; }
info() { echo "[$(date +%H:%M:%S)] $*" >&2; }

require_board_host() {
    [[ -f "$CACHE_FILE" ]] || die "未找到设备缓存，请先运行: ./scripts/search-device.sh"
    BOARD_HOST="$(cat "$CACHE_FILE")"
    BOARD_SSH="$REMOTE_USER@$BOARD_HOST"
}

require_install_dir() {
    [[ -d "$INSTALL_DIR" ]] || die "构建产物不存在，请先构建: cd build && cmake --install . --prefix ./install"
}
