#!/usr/bin/env bash
# ============================================================================
# 网关端到端 smoke（两类链路同时在线）
#
# 验证的是**单测覆盖不到**的整条路径：
#   场景服（本工具扮演） --GateFrame--> 网关 --ClientFrame--> 客户端（本工具扮演）
# 以及"两类数据被严格区分"的 fail-closed 行为（见 tests/e2e_client.cc 的用例清单）。
#
# 用法: smoke_gateway_e2e.sh <gatewayServer 可执行> <gateway_e2e_client 可执行> <工作目录>
# ============================================================================
set -u

BIN="${1:?用法: smoke_gateway_e2e.sh <gatewayServer> <gateway_e2e_client> <工作目录>}"
E2E="${2:?用法: smoke_gateway_e2e.sh <gatewayServer> <gateway_e2e_client> <工作目录>}"
WORK="${3:?用法: smoke_gateway_e2e.sh <gatewayServer> <gateway_e2e_client> <工作目录>}"

# 先解析成绝对路径：脚本随后会 cd 到工作目录，相对路径会失效
for p in "$BIN" "$E2E"; do :; done
if [ -x "$BIN" ]; then BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"; fi
if [ -x "$E2E" ]; then E2E="$(cd "$(dirname "$E2E")" && pwd)/$(basename "$E2E")"; fi

if [ ! -x "$BIN" ]; then echo "FAIL: 找不到可执行文件 $BIN"; exit 1; fi
if [ ! -x "$E2E" ]; then echo "FAIL: 找不到可执行文件 $E2E"; exit 1; fi

mkdir -p "$WORK"
cd "$WORK" || exit 1
rm -rf logs e2e.tmp.yaml gateway_e2e.log e2e.out
PID=""

cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" 2>/dev/null
        sleep 0.3
        kill -KILL "$PID" 2>/dev/null
    fi
}
trap cleanup EXIT

# ---- 两个端口必须不同（配置校验也会拒绝同端口）----
PUB=$(( 20000 + (RANDOM % 8000) ))
PRIV=$(( PUB + 1 ))

cat > e2e.tmp.yaml <<EOF
gateway:
    zoneId: 1
    gatewayId: 7
    publicConfig:            # 公网：客户端
        listenAddr: 127.0.0.1
        listenPort: ${PUB}
    privateConfig:           # 内网：区服内其它服务器
        listenAddr: 127.0.0.1
        listenPort: ${PRIV}
        allowedServices:
            - 8
    eventThreadNum: 2
    clientToken: 998877      # 与 e2e_client.cc 里的 kClientToken 一致
    defaultServiceId: 8
    maxClientSessions: 64
    maxPendingRoutes: 256
    routeTtlMs: 5000
EOF

wait_port() {
    local port="$1" name="$2"
    for _ in $(seq 1 50); do
        # 用 `{ exec ... ; } 2>/dev/null`：fd 仍开在**当前** shell（子 shell 会立刻关掉它），
        # 同时把"端口还没起来"的 Connection refused 噪声丢掉
        if { exec 9<>"/dev/tcp/127.0.0.1/${port}"; } 2>/dev/null; then
            exec 9<&- 2>/dev/null || true
            echo "OK: ${name} 端口 ${port} 已监听"
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# ---- 启动网关 ----
"$BIN" ./e2e.tmp.yaml > gateway_e2e.log 2>&1 &
PID=$!

if ! wait_port "$PUB" "公网(客户端)"; then
    echo "FAIL: 公网端口 ${PUB} 未监听"
    cat gateway_e2e.log
    exit 1
fi
if ! wait_port "$PRIV" "内网(服务器)"; then
    echo "FAIL: 内网端口 ${PRIV} 未监听"
    cat gateway_e2e.log
    exit 1
fi

# ---- 跑端到端用例（本工具扮演场景服 + 多个客户端）----
echo "---- e2e client output ----"
GW_PUBLIC_PORT="$PUB" GW_PRIVATE_PORT="$PRIV" "$E2E" > e2e.out 2>&1
RC=$?
cat e2e.out
if [ "$RC" -ne 0 ]; then
    echo "FAIL: 端到端用例失败（rc=${RC}）"
    echo "---- gateway log ----"
    cat gateway_e2e.log
    exit 1
fi

# ---- SIGTERM：停服时会打印两类链路的统计（同时验证进程可被信号收尾）----
kill -TERM "$PID" 2>/dev/null
STOPPED=0
for _ in $(seq 1 30); do
    if ! kill -0 "$PID" 2>/dev/null; then STOPPED=1; break; fi
    sleep 0.1
done
if [ "$STOPPED" -ne 1 ]; then
    echo "FAIL: SIGTERM 后进程仍在运行（pid=${PID}）"
    exit 1
fi
PID=""
echo "OK: SIGTERM 后进程已退出"

# ---- 日志断言：两类链路的证据必须都在 ----
LOG=$(cat logs/*/gateway.log 2>/dev/null || true)
fail() { echo "FAIL: $1"; echo "---- gateway log ----"; echo "$LOG"; exit 1; }

echo "$LOG" | grep -q "backend registered: SCENE_1"   || fail "日志缺少后端注册记录"
echo "$LOG" | grep -q "relay c->b:"                   || fail "日志缺少 客户端->后端 转发记录"
echo "$LOG" | grep -q "relay b->c:"                   || fail "日志缺少 后端->客户端 回程记录"
echo "$LOG" | grep -q "push b->c:"                    || fail "日志缺少 后端推送记录"
echo "$LOG" | grep -q "client authed:"                || fail "日志缺少客户端鉴权记录"
echo "$LOG" | grep -q "client link got SERVICE frame" || fail "缺少 公网端口收到服务间帧 的告警"
echo "$LOG" | grep -q "backend link got CLIENT frame" || fail "缺少 内网端口收到客户端帧 的告警"

# 两类数据必须都真的走了（按条数断言；统计行只在优雅停服时打印，
# 而信号处理属于清单里的阶段 6 —— 当前 SIGTERM 直接终止进程，不会走到 stop()）
C2B=$(echo "$LOG" | grep -c "relay c->b:")
B2C=$(echo "$LOG" | grep -c "relay b->c:")
if [ "$C2B" -lt 2 ]; then fail "客户端->后端 的转发次数不足（期望 >=2，实际 ${C2B}）"; fi
if [ "$B2C" -lt 2 ]; then fail "后端->客户端 的回程次数不足（期望 >=2，实际 ${B2C}）"; fi
echo "OK: relay c->b=${C2B} 次, relay b->c=${B2C} 次"

exit 0
