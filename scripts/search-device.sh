#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CACHE_FILE="$PROJECT_DIR/.cache/board-host"

mkdir -p "$PROJECT_DIR/.cache"

echo "正在通过 mDNS 发现 SSH 服务 (3秒)..." >&2
results=$(timeout 3 avahi-browse -r -p _ssh._tcp 2>/dev/null \
    | grep '^+;' \
    | awk -F';' '{print $5, $7}' \
    | sort -u) || true

if [[ -z "$results" ]]; then
    echo "未发现任何 SSH 设备，请检查板子是否在线。" >&2
    exit 1
fi

count=$(echo "$results" | wc -l)

if [[ "$count" -eq 1 ]]; then
    host=$(echo "$results" | awk '{print $1}')
else
    echo "发现多个设备:" >&2
    i=1
    while IFS= read -r line; do
        name=$(echo "$line" | awk '{print $1}')
        addr=$(echo "$line" | awk '{print $2}')
        echo "  [$i] $name ($addr)" >&2
        ((i++))
    done <<< "$results"

    echo -n "选择设备 [1-$count]: " >&2
    read -r choice < /dev/tty
    host=$(echo "$results" | sed -n "${choice}p" | awk '{print $1}')
fi

echo "$host" > "$CACHE_FILE"
echo "已选择: $host (已缓存到 .cache/board-host)" >&2
