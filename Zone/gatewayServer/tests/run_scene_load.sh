#!/usr/bin/env bash
set -u

GW_BIN="${1:?gatewayServer path}"
SCENE_BIN="${2:?sceneServer path}"
ROOT="${3:?Zone source root}"
LOAD_BIN="${4:?gateway_load_client path}"
WORK="${5:?work directory}"

PUB="${LOAD_PUBLIC_PORT:-25200}"
PRIV=$((PUB + 1))
mkdir -p "$WORK"
rm -rf "$WORK/logs" "$WORK"/*.yaml "$WORK"/*.json "$WORK"/*.jsonl "$WORK"/*.log

cat > "$WORK/gateway.yaml" <<EOF
gateway:
    zoneId: 1
    gatewayId: 88
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
    metricsPath: ${WORK}/gateway.metrics.json
EOF

sed -e "s#^\( *\)- 127.0.0.1:.*#\1- 127.0.0.1:${PRIV}#" \
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
    [ -n "$SCENE_PID" ] && kill -TERM "$SCENE_PID" 2>/dev/null || true
    [ -n "$GW_PID" ] && kill -TERM "$GW_PID" 2>/dev/null || true
    sleep 1
}
trap cleanup EXIT

"$GW_BIN" "$WORK/gateway.yaml" > "$WORK/gateway.stdout.log" 2>&1 & GW_PID=$!
for _ in $(seq 1 50); do
    [ -s "$WORK/gateway.metrics.json" ] && break
    sleep 0.1
done

"$SCENE_BIN" "$WORK/zone.yaml" "$WORK/scene.yaml" > "$WORK/scene.stdout.log" 2>&1 & SCENE_PID=$!
for _ in $(seq 1 80); do
    [ -s "$WORK/scene.metrics.json" ] && [ "$(grep -o 'backendReady[^,}]*' "$WORK/gateway.metrics.json" 2>/dev/null | tail -1 | cut -d: -f2)" = "1" ] && break
    sleep 0.1
done

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