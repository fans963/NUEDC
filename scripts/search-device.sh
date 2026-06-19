#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"

mkdir -p "$(dirname "$CACHE_FILE")"

info "正在通过 mDNS 发现 SSH 服务 (3秒)..."
results=$(timeout 3 avahi-browse -r -p _ssh._tcp 2>/dev/null \
    | grep '^=;' \
    | awk -F';' '{print $4, $7, $8}' \
    | awk '{key=$2; if($3 ~ /\./) ipv4[key]=$0; else ipv6[key]=$0}
           END{for(k in ipv4) print ipv4[k]; for(k in ipv6) if(!(k in ipv4)) print ipv6[k]}') || true

if [[ -z "$results" ]]; then
    info "mDNS 未发现服务，尝试探测已知主机名..."
    candidates=("radxa-cubie-a7s.local" "radxa-cubie.local" "radxa.local" "orangepi.local" "rock.local")
    for host in "${candidates[@]}"; do
        if timeout 1 bash -c "echo > /dev/tcp/$host/22" 2>/dev/null; then
            ip=$(getent hosts "$host" 2>/dev/null | awk '{print $1}')
            results="$host ${ip:-$host}"
            break
        fi
    done
fi

[[ -z "$results" ]] && die "未发现任何 SSH 设备，请检查板子是否在线。"

count=$(echo "$results" | wc -l)

i=1
while IFS= read -r line; do
    name=$(echo "$line" | awk '{print $1}')
    ip=$(echo "$line" | awk '{print $3}')
    echo "  [$i] $name ($ip)" >&2
    ((i++))
done <<< "$results"

if [[ "$count" -eq 1 ]]; then
    host=$(echo "$results" | awk '{print $2}')
else
    echo -n "选择设备 [1-$count]: " >&2
    read -r choice < /dev/tty
    host=$(echo "$results" | sed -n "${choice}p" | awk '{print $2}')
fi

echo "$host" > "$CACHE_FILE"
info "已选择: $host (已缓存到 $CACHE_FILE)"
