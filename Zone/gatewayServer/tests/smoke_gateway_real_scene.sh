#!/usr/bin/env bash
# ============================================================================
# 真实链路端到端：**真 gatewayServer + 真 sceneServer + 真客户端帧**
#
# 这是最有说服力的验证：
#   客户端(ClientFrame) --公网端口--> 网关(GateFrame) --内网端口--> 真实场景服
#        ^                                                              |
#        +------------------ WalkAck（按回程路由还原 requestId） <-------+
#
# 与 smoke_gateway_e2e.sh 的分工：
#   - 那个用**假后端**，覆盖协议/鉴权/路由的边界（快、确定性强）；
#   - 这个用**真场景服**，证明网关的帧格式与场景服既有实现完全兼容
#     （src/dst 服务号、握手体、链路标识、seq 回传），以及真实 Lua 业务能回包。
#
# 用法: smoke_gateway_real_scene.sh <gatewayServer> <sceneServer> <gateway 源码目录> <工作目录>
# ============================================================================
set -u

GW_BIN="${1:?用法: smoke_gateway_real_scene.sh <gatewayServer> <sceneServer> <gateway 源码目录> <工作目录>}"
SCENE_BIN="${2:?}"
GW_SRC="${3:?}"
WORK="${4:?}"

abs_path() {
    local p="$1"
    if [ -e "$p" ]; then
        printf '%s/%s' "$(cd "$(dirname "$p")" && pwd)" "$(basename "$p")"
    else
        printf '%s' "$p"
    fi
}

GW_BIN="$(abs_path "$GW_BIN")"
SCENE_BIN="$(abs_path "$SCENE_BIN")"
GW_SRC="$(abs_path "$GW_SRC")"

[ -x "$GW_BIN" ] || { echo "FAIL: 找不到 $GW_BIN"; exit 1; }
[ -x "$SCENE_BIN" ] || { echo "FAIL: 找不到 $SCENE_BIN"; exit 1; }

SCENE_SRC="$(cd "$GW_SRC/../sceneServer" && pwd)"

mkdir -p "$WORK"
cd "$WORK" || exit 1
rm -rf logs gw.tmp.yaml scene.tmp.yaml zone.tmp.yaml gateway.log scene_stdout.log client.out

GW_PID=""
SCENE_PID=""
cleanup() {
    for pid in "$SCENE_PID" "$GW_PID"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null
            sleep 0.2
            kill -KILL "$pid" 2>/dev/null
        fi
    done
}
trap cleanup EXIT

PUB=$(( 20000 + (RANDOM % 8000) ))
PRIV=$(( PUB + 1 ))
SCENE_PORT=$(( PRIV + 1 ))
GW_LOG_LEVEL="${GW_LOG_LEVEL:-info}"

# ---- 网关配置：公网/内网两个端口，白名单只放场景服的 8 ----
cat > gw.tmp.yaml <<EOF
gateway:
    zoneId: 1
    gatewayId: 3
    publicConfig:
        listenAddr: 127.0.0.1
        listenPort: ${PUB}
    privateConfig:
        listenAddr: 127.0.0.1
        listenPort: ${PRIV}
        allowedServices:
            - 8
    eventThreadNum: 2
    clientToken: 998877
    defaultServiceId: 8
    maxClientSessions: 64
    maxPendingRoutes: 256
    routeTtlMs: 5000
    logLevel: ${GW_LOG_LEVEL}   # 默认 debug（诊断脚本）；可用 GW_LOG_LEVEL=info 复跑以排除日志时序影响
EOF

# ---- 从仓库真实配置派生场景服配置：只改端口/日志名/日志目录/luaDir/网关地址 ----
sed -e "s#^\( *\)listenPort: .*#\1listenPort: ${SCENE_PORT}#" \
    -e "s#^\( *\)luaDir: .*#\1luaDir: ${SCENE_SRC}/lua_script#" \
    "$SCENE_SRC/config/sceneConfig.yaml" > scene.tmp.yaml
printf '\n    mapPath: %s/config/map.yaml\n' "$SCENE_SRC" >> scene.tmp.yaml

sed -e "s#^\( *\)- 127.0.0.1:.*#\1- 127.0.0.1:${PRIV}#" \
    -e 's/enable: true/enable: false/' \
    -e "s#^\( *\)dir: .*#\1dir: ./logs#" \
    -e "s#^\( *\)name: .*#\1name: scene_real_e2e#" \
    "$GW_SRC/../config/zoneConfig.yaml" > zone.tmp.yaml

grep -q "127.0.0.1:${PRIV}" zone.tmp.yaml || { echo "FAIL: 临时 zoneConfig 未指向网关内网端口"; exit 1; }
grep -q "listenPort: ${SCENE_PORT}" scene.tmp.yaml || { echo "FAIL: 临时 sceneConfig 未改到端口"; exit 1; }

wait_port() {
    local port="$1" name="$2"
    for _ in $(seq 1 80); do
        if { exec 9<>"/dev/tcp/127.0.0.1/${port}"; } 2>/dev/null; then
            exec 9<&- 2>/dev/null || true
            echo "OK: ${name} 端口 ${port} 已监听"
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# ---- 1. 起网关 ----
"$GW_BIN" ./gw.tmp.yaml > gateway.log 2>&1 &
GW_PID=$!
wait_port "$PRIV" "网关内网" || { echo "FAIL: 网关内网端口未监听"; cat gateway.log; exit 1; }

# ---- 2. 起场景服（它会反向连接网关并握手注册）----
"$SCENE_BIN" ./zone.tmp.yaml ./scene.tmp.yaml > scene_stdout.log 2>&1 &
SCENE_PID=$!

# ---- 3. 真客户端：鉴权 + PLAYER/MOVE(WalkReq) ----
# 注册完成的判定用**协议级**证据（客户端请求能拿到真实响应），而不是等日志文件出现某行：
#   日志是异步落盘的，用它做同步点会让测试偶发假失败（实测约 5%）。
ATTEMPTS=0
RC=1
while [ "$ATTEMPTS" -lt 60 ]; do
    ATTEMPTS=$(( ATTEMPTS + 1 ))

    if ! kill -0 "$SCENE_PID" 2>/dev/null; then
        echo "FAIL: 场景服启动即退出"
        cat scene_stdout.log
        exit 1
    fi

    python3 "$GW_SRC/tests/real_scene_client.py" "$PUB" 4242 998877 > client.out 2>&1
    RC=$?
    [ "$RC" -eq 2 ] && { sleep 0.25; continue; }   # 2 = 后端还没就绪，重试
    break
done

echo "---- client (第 ${ATTEMPTS} 次尝试) ----"
cat client.out
if [ "$RC" -eq 2 ]; then
    echo "FAIL: 等不到场景服注册（网关一直回 backend service unavailable）"
    cat logs/*/gateway.log 2>/dev/null | tail -40
    cat scene_stdout.log
    exit 1
fi
if [ "$RC" -ne 0 ]; then
    echo "FAIL: 客户端未走通（rc=${RC}）"
    echo "---- gateway log ----"; cat logs/*/gateway.log 2>/dev/null | tail -40
    echo "---- scene stdout ----"; cat scene_stdout.log
    exit 1
fi
echo "OK: 真实场景服已注册，客户端走通 鉴权 -> 请求 -> 回包"

# ---- 4. 断言：网关两侧都转了这条消息，且回程把 requestId 还原成客户端的 2 ----
LOG=$(cat logs/*/gateway.log 2>/dev/null || true)
fail() { echo "FAIL: $1"; echo "---- gateway log ----"; echo "$LOG"; exit 1; }

echo "$LOG" | grep -q "relay c->b: session=.* player=4242 dst=SCENE_1 module=3 method=2 requestId=2" \
    || fail "缺少 客户端->场景服 的转发记录（playerId 必须由网关绑定）"
echo "$LOG" | grep -q "relay b->c: session=.* from=SCENE_1 module=3 method=2 .* -> requestId=2" \
    || fail "缺少 场景服->客户端 的回程记录"
grep -q "response ok:" client.out || fail "客户端未收到 Response"

# WalkAck { code=0, x=10, y=20, dir=0 } 的 proto3 字节：
#   proto3 会**省略等于默认值(0)的字段**，所以 code/dir 不入流，只剩 x/y：
#   10 0A (x=10), 18 14 (y=20)
grep -q "response ok: module=3 method=2 body=100a1814" client.out \
    || fail "响应体不是预期的 WalkAck（期望 100a1814 = x:10,y:20；code/dir 为 0 被 proto3 省略）"
echo "OK: 真实链路 WalkAck 字节完全符合预期（x=10, y=20）"

grep -q "invalid route ok: module=9999 method=9999 requestId=3 body=08e907120f" client.out \
    || fail "未知模块的 RetTip 未沿原请求返回"
grep -q "invalid route ok: module=3 method=9999 requestId=4 body=08e907120f" client.out \
    || fail "已知模块的未知方法未沿原请求返回"
echo "OK: 无效路由 RetTip 通过网关恢复客户端 requestId"

# 两次退出（主动退出、最终 TCP 断开）均须在场景服产生 Actor 清理日志。
for _ in $(seq 1 40); do
    COUNT=$(grep -h 'player offline, pid=4242' logs/*/scene_real_e2e*.log 2>/dev/null | wc -l)
    [ "$COUNT" -ge 2 ] && break
    sleep 0.1
done
[ "$COUNT" -ge 2 ] || fail "场景服未完成主动退出和客户端断开清理"
grep -q 'enter scene ok:' client.out || fail "缺少场景登录确认"
grep -q 'logout ok:' client.out || fail "缺少主动退出验证"
echo "OK: 玩家登录进场、移动、主动退出和断开清理均通过"

exit 0
