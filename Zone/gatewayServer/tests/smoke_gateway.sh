#!/usr/bin/env bash
# ============================================================================
# 网关进程启动 smoke
#
# 验证"构建产物真的能起来、能在配置的**两个**端口上监听、脏数据不会打死进程、
# 能被 SIGTERM 收尾"——这是单元测试覆盖不到的部分。
#
# 协议级的收发断言在 smoke_gateway_e2e.sh（那边才有真实的帧）。
#
# 用法: smoke_gateway.sh <gatewayServer 可执行路径> <工作目录>
# ============================================================================
set -u

BIN="${1:?用法: smoke_gateway.sh <可执行路径> <工作目录>}"
WORK="${2:?用法: smoke_gateway.sh <可执行路径> <工作目录>}"

# 先解析成绝对路径：脚本后续会 cd 到工作目录，相对路径会失效
if [ -x "$BIN" ]; then
    BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
fi

if [ ! -x "$BIN" ]; then
    echo "FAIL: 找不到可执行文件 $BIN"
    exit 1
fi

mkdir -p "$WORK"
cd "$WORK" || exit 1
rm -rf logs config.tmp.yaml
PID=""

cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" 2>/dev/null
        sleep 0.3
        kill -KILL "$PID" 2>/dev/null
    fi
}
trap cleanup EXIT

# ---- 选两个大概率空闲的端口（必须不同：配置校验会拒绝同端口）----
PORT=$(( 20000 + (RANDOM % 20000) ))
PRIV_PORT=$(( PORT + 1 ))

cat > config.tmp.yaml <<EOF
gateway:
    zoneId: 1
    gatewayId: 9
    publicConfig:
        listenAddr: 127.0.0.1
        listenPort: ${PORT}
    privateConfig:
        listenAddr: 127.0.0.1
        listenPort: ${PRIV_PORT}
        allowedServices:
            - 8
    eventThreadNum: 2
    clientToken: 998877
    defaultServiceId: 8
EOF

# ---- 启动进程 ----
"$BIN" ./config.tmp.yaml > stdout.log 2>&1 &
PID=$!

wait_port() {
    local port="$1" name="$2"
    for _ in $(seq 1 50); do
        # 用 `{ exec ... ; } 2>/dev/null`：fd 仍开在**当前** shell（子 shell 会立刻关掉它，
        # 从而误判为"探测成功"），同时把"端口还没起来"的 Connection refused 噪声丢掉
        if { exec 3<>"/dev/tcp/127.0.0.1/${port}"; } 2>/dev/null; then
            exec 3<&- 2>/dev/null || true
            echo "OK: ${name} 端口 ${port} 已监听"
            return 0
        fi
        sleep 0.1
    done
    return 1
}

if ! wait_port "$PORT" "公网(客户端)"; then
    echo "FAIL: 公网端口 ${PORT} 在 5 秒内未监听"
    echo "---- 进程输出 ----"
    cat stdout.log 2>/dev/null
    cat logs/*/gateway.log 2>/dev/null
    exit 1
fi

if ! wait_port "$PRIV_PORT" "内网(服务器)"; then
    echo "FAIL: 内网端口 ${PRIV_PORT} 在 5 秒内未监听"
    echo "---- 进程输出 ----"
    cat stdout.log 2>/dev/null
    cat logs/*/gateway.log 2>/dev/null
    exit 1
fi

# ---- 客户端端口上发**脏数据**：必须被当成垃圾丢掉，而不是打死进程/占满内存 ----
if exec 4<>"/dev/tcp/127.0.0.1/${PORT}" 2>/dev/null; then
    printf 'smoke-payload-not-a-frame' >&4 || { echo "FAIL: 写入失败"; exit 1; }
    # 先让服务端读到数据，再断开：模拟正常的"请求 -> 断开"时序
    sleep 0.3
    exec 4<&- 2>/dev/null || true
    sleep 0.5
else
    echo "FAIL: 无法连接到 ${PORT}"
    exit 1
fi

if ! kill -0 "$PID" 2>/dev/null; then
    echo "FAIL: 收到脏数据后进程退出了（协议解析不能把进程打死）"
    cat logs/*/gateway.log 2>/dev/null
    exit 1
fi
echo "OK: 脏数据被安全丢弃，进程存活"

LOG=$(cat logs/*/gateway.log 2>/dev/null || true)
fail() { echo "FAIL: $1"; echo "---- 日志 ----"; echo "$LOG"; exit 1; }

echo "$LOG" | grep -q "gateway started, public(clients) on 127.0.0.1:${PORT}, private(servers) on 127.0.0.1:${PRIV_PORT}" \
    || fail "日志缺少双端口启动行"
echo "$LOG" | grep -q "client connection .* established" \
    || fail "日志缺少客户端连接建立记录（accept 未生效？）"
echo "$LOG" | grep -q "client connection .* closed" \
    || fail "日志缺少客户端连接关闭记录（关闭未通知上层？）"
echo "OK: 双端口启动/accept/关闭 日志齐备"

# ---- SIGTERM 收尾：进程必须退出 ----
kill -TERM "$PID" 2>/dev/null
for _ in $(seq 1 30); do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "OK: SIGTERM 后进程已退出"
        PID=""
        exit 0
    fi
    sleep 0.1
done

echo "FAIL: SIGTERM 后进程仍在运行（pid=${PID}）"
exit 1
