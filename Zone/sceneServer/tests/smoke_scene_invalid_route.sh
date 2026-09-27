#!/usr/bin/env bash
# ============================================================================
# 场景服进程级 smoke：**无效路由的消息不得创建 Actor**（阶段 1 任务 1.16 的守卫）
#
# 为什么要进程级用例：单测覆盖不到"真实网络帧 → 网络线程 → MsgBus → 逻辑线程"这条链路；
# 而 1.16 的缺陷正好出现在这条链路的末端（路由校验与 Actor 创建的先后）。
#
# 做法：用仓库真实配置派生出临时配置（换端口、换日志名），起进程 →
#       发一条 Module/Method=9999/9999、playerId=4242 的帧并**保持连接打开**
#       （直接 close 会触发 CONN_CLOSE 把 Actor 回收，从而掩盖缺陷）→
#       SIGUSR1 让主线程打印 stats → 断言所有 worker 的 players=0。
#
# 用法: smoke_scene_invalid_route.sh <sceneServer 可执行> <工作目录> <源码目录>
# ============================================================================
set -u

BIN="${1:?用法: smoke_scene_invalid_route.sh <可执行> <工作目录> <源码目录>}"
WORK="${2:?用法: smoke_scene_invalid_route.sh <可执行> <工作目录> <源码目录>}"
SRC="${3:?用法: smoke_scene_invalid_route.sh <可执行> <工作目录> <源码目录>}"

# 先解析成绝对路径：脚本随后会 cd 到工作目录，相对路径会失效
# 先解析成绝对路径：脚本随后会 cd 到工作目录，相对路径会失效
if [ -x "$BIN" ]; then
    BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
fi
if [ ! -x "$BIN" ]; then
    echo "FAIL: 找不到可执行文件 $BIN"
    exit 1
fi

if [ -d "$SRC" ]; then
    SRC="$(cd "$SRC" && pwd)"
fi
if [ ! -f "$SRC/config/sceneConfig.yaml" ]; then
    echo "FAIL: 源码目录里找不到 config/sceneConfig.yaml（SRC=$SRC）"
    exit 1
fi

mkdir -p "$WORK"
cd "$WORK" || exit 1
rm -rf logs scene.tmp.yaml zone.tmp.yaml

PID=""
cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" 2>/dev/null
        sleep 0.3
        kill -KILL "$PID" 2>/dev/null
    fi
}
trap cleanup EXIT

# ---- 从仓库真实配置派生临时配置（打开测试监听，改端口 / 日志 / Lua 路径）----
PORT=$(( 30000 + (RANDOM % 20000) ))

sed -e "s#^\( *\)listenPort: .*#\1listenPort: ${PORT}#" \
    -e "s#^\( *\)listenEnable: .*#\1listenEnable: true#" \
    -e "s#^\( *\)luaDir: .*#\1luaDir: ${SRC}/lua_script#" \
    "$SRC/config/sceneConfig.yaml" > scene.tmp.yaml

sed -e "s#^\( *\)dir: .*#\1dir: ./logs#" \
    -e "s#^\( *\)name: .*#\1name: scene_smoke#" \
    "$SRC/../config/zoneConfig.yaml" > zone.tmp.yaml

grep -q "listenPort: ${PORT}" scene.tmp.yaml || { echo "FAIL: 临时配置未改到端口"; exit 1; }

# ---- 启动 ----
"$BIN" ./zone.tmp.yaml ./scene.tmp.yaml > stdout.log 2>&1 &
PID=$!

LISTENING=0
for _ in $(seq 1 120); do
    # 探测用子 shell：连接随子 shell 退出而关闭；且不会把 shell 的 stderr 永久重定向
    if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then
        LISTENING=1
        break
    fi
    sleep 0.1
done
if [ "$LISTENING" -ne 1 ]; then
    echo "FAIL: 端口 ${PORT} 在 12 秒内未监听"
    cat stdout.log
    exit 1
fi
echo "OK: 端口 ${PORT} 已监听"

# ---- 发「无效路由 + 非 0 playerId」的帧，并**保持连接打开** ----
# 32B 大端头：magic=C1EA ver=1 type=1 retr=0 module=0x270F(9999) method=0x270F(9999)
#              seq=1 playerId=0x1092(4242) totalLen=0x20(32) src=1 dst=8
#
# 用后台子 shell 完成"连接 → 写帧 → 保持 hold 秒"，这样：
#   1) 连接在取 stats 期间一直是打开的（关掉会触发 CONN_CLOSE 把 Actor 回收，掩盖缺陷）；
#   2) fd 与 stderr 的重定向都局限在子 shell 内，不污染主脚本。
HOLD_SECONDS=0.8
(
    exec 4<>"/dev/tcp/127.0.0.1/${PORT}" 2>/dev/null || exit 1
    printf '\xc1\xea\x00\x01\x01\x00\x27\x0f\x27\x0f\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x10\x92\x00\x00\x00\x20\x01\x08' >&4 || exit 1
    sleep "$HOLD_SECONDS"
) &
FRAME_SENDER=$!

# ---- 触发 stats（SIGUSR1 → 主线程打印 stats: ... players=N）----
kill -USR1 "$PID"
sleep 0.9

wait "$FRAME_SENDER"
echo "OK: 已发送无效路由帧（持有 ${HOLD_SECONDS}s 后断开）"

LOG="$(find logs -name 'scene_smoke.log' -exec cat {} \; 2>/dev/null || true)"

echo "$LOG" | grep -q "route not found" \
    || { echo "FAIL: 日志里没有 route not found（帧没被处理？）"; echo "$LOG" | tail -5; exit 1; }

PLAYERS="$(echo "$LOG" | grep -o 'players=[0-9]*' | sort -u | tr '\n' ' ')"
echo "stats 中出现的 players 取值: ${PLAYERS}"
if [ "$PLAYERS" != "players=0 " ]; then
    echo "FAIL: 无效路由创建了 Actor（期望全部 players=0）"
    echo "$LOG" | grep 'stats:'
    exit 1
fi
echo "OK: 无效路由未创建 Actor（players 全为 0）"

exec 4<&- 2>/dev/null || true

# ---- 优雅退出 ----
kill -TERM "$PID"
for _ in $(seq 1 50); do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "OK: SIGTERM 后进程已退出"
        PID=""
        exit 0
    fi
    sleep 0.1
done
echo "FAIL: SIGTERM 后进程仍在运行"
exit 1
