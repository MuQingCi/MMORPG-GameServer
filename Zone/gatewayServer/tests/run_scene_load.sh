#!/usr/bin/env bash
set -u

GW_BIN="${1:?gatewayServer path}"
SCENE_BIN="${2:?sceneServer path}"
ROOT="${3:?Zone source root}"
LOAD_BIN="${4:?gateway_load_client path}"
WORK="${5:?work directory}"

PUB="${LOAD_PUBLIC_PORT:-25200}"
PRIV=$((PUB + 1))
GATEWAY_ID="${LOAD_GATEWAY_ID:-88}"
mkdir -p "$WORK"
rm -rf "$WORK/logs" "$WORK"/*.yaml "$WORK"/*.json "$WORK"/*.jsonl "$WORK"/*.log

cat > "$WORK/gateway.yaml" <<EOF
gateway:
    zoneId: 1
    gatewayId: ${GATEWAY_ID}
    publicConfig:
        listenAddr: 127.0.0.1
        listenPort: ${PUB}
    privateConfig:
        listenAddr: 127.0.0.1
        listenPort: ${PRIV}
        allowedServices:
            - 8
    eventThreadNum: 4
    clientToken: 998877
    defaultServiceId: 8
    maxClientSessions: 1024
    maxPendingRoutes: 10000
    routeTtlMs: 15000
    maxFramesPerSec: ${LOAD_GATEWAY_MAX_FRAMES_PER_SEC:-500}
    logLevel: info
    logDir: ${WORK}/gateway-logs
    logName: gateway_load
    metricsPath: ${WORK}/gateway.metrics.json
EOF

sed -e "s#^\\( *\\)- [0-9][0-9]*=127.0.0.1:.*#\\1- ${GATEWAY_ID}=127.0.0.1:${PRIV}#" \
    -e 's/enable: true/enable: false/' \
    -e "s#^\( *\)dir: .*#\1dir: ${WORK}/logs#" \
    -e 's#^\( *\)name: .*#\1name: scene_load#' \
    "$ROOT/config/zoneConfig.yaml" > "$WORK/zone.yaml"
sed -e "s#^\( *\)listenPort: .*#\1listenPort: $((PRIV + 1))#" \
    -e "s#^\( *\)luaDir: .*#\1luaDir: ${ROOT}/sceneServer/lua_script#" \
    -e "s#^\( *\)maxFramesPerSec: .*#\1maxFramesPerSec: ${LOAD_SCENE_MAX_FRAMES_PER_SEC:-1000}#" \
    "$ROOT/sceneServer/config/sceneConfig.yaml" > "$WORK/scene.yaml"
printf '\n    mapPath: %s/sceneServer/config/map.yaml\n    metricsPath: %s/scene.metrics.json\n' "$ROOT" "$WORK" >> "$WORK/scene.yaml"

GW_PID=""; SCENE_PID=""
cleanup() {
    [ -n "$GW_PID" ] && kill -TERM "$GW_PID" 2>/dev/null || true
    [ -n "$SCENE_PID" ] && kill -TERM "$SCENE_PID" 2>/dev/null || true
    [ -n "$GW_PID" ] && wait "$GW_PID" 2>/dev/null || true
    [ -n "$SCENE_PID" ] && wait "$SCENE_PID" 2>/dev/null || true
    sleep 1
}
trap cleanup EXIT

mkdir -p "$WORK/gateway-logs" "$WORK/scene-logs"
(cd "$WORK" && exec "$GW_BIN" "$WORK/gateway.yaml") > "$WORK/gateway.stdout.log" 2>&1 & GW_PID=$!
for _ in $(seq 1 50); do
    [ -s "$WORK/gateway.metrics.json" ] && break
    if ! kill -0 "$GW_PID" 2>/dev/null; then
        echo "FAIL: gateway exited before metrics readiness"
        cat "$WORK/gateway.stdout.log"
        exit 1
    fi
    sleep 0.1
done
[ -s "$WORK/gateway.metrics.json" ] || { echo "FAIL: gateway metrics readiness timeout"; cat "$WORK/gateway.stdout.log"; exit 1; }

(cd "$WORK" && exec "$SCENE_BIN" "$WORK/zone.yaml" "$WORK/scene.yaml") > "$WORK/scene.stdout.log" 2>&1 & SCENE_PID=$!
READY=0
for _ in $(seq 1 120); do
    if ! kill -0 "$GW_PID" 2>/dev/null || ! kill -0 "$SCENE_PID" 2>/dev/null; then
        echo "FAIL: gateway or scene exited before backend registration"
        cat "$WORK/gateway.stdout.log" "$WORK/scene.stdout.log"
        exit 1
    fi
    if [ -s "$WORK/scene.metrics.json" ] && [ "$(grep -o 'backendReady[^,}]*' "$WORK/gateway.metrics.json" 2>/dev/null | tail -1 | cut -d: -f2 | tr -d '[:space:]')" = "1" ]; then
        READY=1
        break
    fi
    sleep 0.1
done
if [ "$READY" != 1 ]; then
    echo "FAIL: backend registration readiness timeout (gatewayId=$GATEWAY_ID, peer port=$PRIV)"
    echo "--- gateway stdout ---"; cat "$WORK/gateway.stdout.log"
    echo "--- gateway log ---"; cat "$WORK/gateway-logs/$(date +%F)/gateway_load.log" 2>/dev/null || true
    echo "--- scene stdout ---"; cat "$WORK/scene.stdout.log"
    echo "--- scene log ---"; cat "$WORK/scene-logs/$(date +%F)/scene_load.log" 2>/dev/null || true
    echo "--- metrics ---"; cat "$WORK/gateway.metrics.json" "$WORK/scene.metrics.json" 2>/dev/null || true
    exit 1
fi
echo "PASS: backend registered before load (gatewayId=$GATEWAY_ID)"

"$LOAD_BIN" --host 127.0.0.1 --port "$PUB" --connections "${LOAD_CONNECTIONS:-10}" \
    --rps "${LOAD_RPS:-20}" --duration "${LOAD_DURATION:-10}" --scene \
    --gateway-metrics "$WORK/gateway.metrics.json" \
    --scene-metrics "$WORK/scene.metrics.json" \
    --metrics-output "$WORK/metrics.jsonl" \
    --output "$WORK/load-summary.json"
RC=$?

echo "--- load summary ---"; cat "$WORK/load-summary.json" 2>/dev/null || true
echo "--- last metrics sample ---"; tail -1 "$WORK/metrics.jsonl" 2>/dev/null || true
echo "--- gateway metrics ---"; cat "$WORK/gateway.metrics.json" 2>/dev/null || true
echo "--- scene metrics ---"; cat "$WORK/scene.metrics.json" 2>/dev/null || true
exit "$RC"