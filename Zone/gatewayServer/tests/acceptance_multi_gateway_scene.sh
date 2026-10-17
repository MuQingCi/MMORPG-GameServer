#!/usr/bin/env bash
# Real-process 2 gateway x 2 scene acceptance and sustained load.
# Refuse the obsolete exploratory preflight/restart sequence: only the guarded
# READY-barrier flow below may start clients, once the documented online-actor
# preparation protocol is implemented.
set -Eeuo pipefail
GW_BIN="$(realpath "${1:?gatewayServer}")"
SCENE_BIN="$(realpath "${2:?sceneServer}")"
ROOT="$(realpath "${3:?Zone source root}")"
LOAD_BIN="$(realpath "${4:?gateway_load_client}")"
WORK="${5:?work directory}"
CONNECTIONS="${LOAD_CONNECTIONS:-8}"
CONNECTIONS_PER_GATEWAY="${LOAD_CONNECTIONS_PER_GATEWAY:-$CONNECTIONS}"
RPS="${LOAD_RPS:-10}"
DURATION="${LOAD_DURATION:-5}"
DRAIN="${LOAD_DRAIN:-0}"
CHANNELS="${LOAD_CHANNELS:-2}"
FRAME_LIMIT="${LOAD_FRAME_LIMIT:-1000}"
MAX_QUEUE_SECONDS="${LOAD_MAX_QUEUE_SECONDS:-0}"
TOKEN=998877
BASE_PORT="${LOAD_BASE_PORT:-$((22000 + RANDOM % 10000))}"
mkdir -p "$WORK"; WORK="$(realpath "$WORK")"
if [[ "${LOAD_PRESERVE_WORK:-0}" != 1 ]]; then rm -rf "$WORK"/*; fi
PIDS=()
L1=""; L2=""; MONITOR=""
cleanup() {
    for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    local deadline=$((SECONDS+20))
    while (( SECONDS < deadline )); do
        local alive=0
        for pid in "${PIDS[@]}"; do kill -0 "$pid" 2>/dev/null && alive=1; done
        (( alive == 0 )) && break
        sleep .1
    done
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            echo "FORCED_SERVICE_KILL pid=$pid reason=cleanup_timeout" >> "$WORK/forced-service-kill.log"
            kill -KILL "$pid" 2>/dev/null || true
        fi
        wait "$pid" 2>/dev/null || true
    done
}
stop_load_clients() {
    for pid in "$L1" "$L2"; do [[ -n "$pid" ]] && kill -TERM "$pid" 2>/dev/null || true; done
    local deadline=$((SECONDS+${LOAD_STOP_GRACE_SEC:-10}))
    while (( SECONDS < deadline )); do
        local alive=0
        for pid in "$L1" "$L2"; do [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null && alive=1; done
        (( alive == 0 )) && break
        sleep .1
    done
    for pid in "$L1" "$L2"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            echo "FORCED_CLIENT_KILL pid=$pid reason=grace_expired" >> "$WORK/forced-client-kill.log"
            kill -KILL "$pid" 2>/dev/null || true
        fi
    done
    for pid in "$L1" "$L2"; do [[ -n "$pid" ]] && wait "$pid" 2>/dev/null || true; done
}
cleanup_all() { [[ -n "$MONITOR" ]] && kill "$MONITOR" 2>/dev/null || true; stop_load_clients; cleanup; }
trap cleanup_all EXIT
PUB1=$BASE_PORT; PRIV1=$((BASE_PORT+1)); PUB2=$((BASE_PORT+2)); PRIV2=$((BASE_PORT+3))
cat > "$WORK/gw1.yaml" <<EOF
gateway:
    zoneId: 1
    gatewayId: 1
    backendChannels: $CHANNELS
    publicConfig:
        listenAddr: 127.0.0.1
        listenPort: $PUB1
    privateConfig:
        listenAddr: 127.0.0.1
        listenPort: $PRIV1
        allowedServices:
            - 8
    eventThreadNum: 2
    clientToken: $TOKEN
    defaultServiceId: 8
    defaultSceneId: 0
    maxClientSessions: 2048
    maxPendingRoutes: 20000
    routeTtlMs: 15000
    maxFramesPerSec: $FRAME_LIMIT
    logLevel: info
    metricsPath: $WORK/gw1.metrics.json
EOF
sed "s/gatewayId: 1/gatewayId: 2/; s/listenPort: $PUB1/listenPort: $PUB2/; s/listenPort: $PRIV1/listenPort: $PRIV2/" "$WORK/gw1.yaml" > "$WORK/gw2.yaml"
sed -i "s#metricsPath: $WORK/gw1.metrics.json#metricsPath: $WORK/gw2.metrics.json#" "$WORK/gw2.yaml"

make_scene_config() {
    local id="$1" port="$2" name="$3"
    sed -e "s/^    sceneId: .*/    sceneId: $id/" \
        -e "s/^        listenPort: .*/        listenPort: $port/" \
        -e "s#^    luaDir: .*#    luaDir: $ROOT/sceneServer/lua_script#" \
        "$ROOT/sceneServer/config/sceneConfig.yaml" > "$WORK/$name.yaml"
    printf '\n    mapPath: %s/sceneServer/config/map.yaml\n    metricsPath: %s/%s.metrics.json\n    net:\n        maxFramesPerSec: %s\n' "$ROOT" "$WORK" "$name" "$FRAME_LIMIT" >> "$WORK/$name.yaml"
}
make_scene_config 10 "$((PRIV1+3))" scene10
make_scene_config 20 "$((PRIV1+4))" scene20
python3 - "$ROOT/config/zoneConfig.yaml" "$WORK/zone.yaml" "$PRIV1" "$PRIV2" "$CHANNELS" <<'PY'
import pathlib,sys
source,out,p1,p2,channels=sys.argv[1:]
text=pathlib.Path(source).read_text()
text=text.replace('        - 1=127.0.0.1:13145',f'        - 1=127.0.0.1:{p1}\n        - 2=127.0.0.1:{p2}')
text=text.replace('    backendChannels: 1',f'    backendChannels: {channels}')
pathlib.Path(out).write_text(text)
PY
# Disable external persistence and isolate logs for this run.
if [[ -n "${LOAD_PERSISTENCE_ZONE_CONFIG:-}" ]]; then
    [[ -n "${LOAD_PERSISTENCE_CONTEXT:-}" && -f "$LOAD_PERSISTENCE_CONTEXT" ]] || { echo "FAIL: missing isolated persistence context"; exit 1; }
    cp "$LOAD_PERSISTENCE_ZONE_CONFIG" "$WORK/zone.yaml"
    chmod 600 "$WORK/zone.yaml"
    python3 - "$WORK/zone.yaml" "$PRIV1" "$PRIV2" "$CHANNELS" <<'PY'
import pathlib,re,sys
path,p1,p2,channels=sys.argv[1:]
text=pathlib.Path(path).read_text()
text=re.sub(r'(?m)^        - \d+=127\.0\.0\.1:\d+$',f'        - 1=127.0.0.1:{p1}\n        - 2=127.0.0.1:{p2}',text)
text=re.sub(r'(?m)^    backendChannels:.*$',f'    backendChannels: {channels}',text)
assert 'enable: true' in text
pathlib.Path(path).write_text(text)
PY
    sed -i 's/flushIntervalMs: 30000/flushIntervalMs: 1000/' "$WORK/scene10.yaml" "$WORK/scene20.yaml"
else
    sed -i 's/enable: true/enable: false/' "$WORK/zone.yaml"
fi
sed -i "s#^        dir: .*#        dir: $WORK/logs#" "$WORK/zone.yaml"
if [[ -n "${LOAD_PERSISTENCE_ZONE_CONFIG:-}" || "${LOAD_EXPLICIT_SCENE:-0}" == 1 ]]; then
    # Explicit placement also supports matched no-DB staircase controls.
    sed -i 's/defaultSceneId: 0/defaultSceneId: 10/' "$WORK/gw1.yaml"
    sed -i 's/defaultSceneId: 0/defaultSceneId: 20/' "$WORK/gw2.yaml"
fi

for n in 1 2; do (cd "$WORK" && "$GW_BIN" "$WORK/gw$n.yaml") > "$WORK/gw$n.stdout.log" 2>&1 & PIDS+=("$!"); done
wait_files() {
    local deadline=$((SECONDS+12))
    while (( SECONDS < deadline )); do
        local ready=1; for file in "$@"; do [[ -e "$file" ]] || ready=0; done
        (( ready )) && return 0
        for pid in "${PIDS[@]}"; do kill -0 "$pid" 2>/dev/null || { echo "FAIL: service exited pid=$pid"; return 1; }; done
        sleep .2
    done
    echo "FAIL: readiness timeout: $*"
    for file in "$WORK"/*.stdout.log; do [[ -f "$file" ]] && { echo "--- $file"; cat "$file"; }; done
    return 1
}
wait_files "$WORK/logs/$(date +%F)/gateway.log"

# The async logger creates gateway.log before GatewayServer::start() has bound
# both listeners. Do not infer readiness from the log file: prove each private
# endpoint accepts a TCP connection before starting scenes (which connect only
# once and would otherwise race the listen()).
wait_private_ports() {
    local deadline=$((SECONDS+12))
    while (( SECONDS < deadline )); do
        local ready=1
        for port in "$PRIV1" "$PRIV2"; do
            if ! python3 - "$port" <<'PY'
import socket, sys
s = socket.socket()
s.settimeout(0.25)
try:
    s.connect(("127.0.0.1", int(sys.argv[1])))
except OSError:
    sys.exit(1)
finally:
    s.close()
PY
            then
                ready=0
            fi
        done
        (( ready )) && return 0
        for pid in "${PIDS[@]}"; do
            kill -0 "$pid" 2>/dev/null || {
                echo "FAIL: gateway exited before private ports became ready (pid=$pid)"
                return 1
            }
        done
        sleep .1
    done
    echo "FAIL: gateway private ports did not become ready: $PRIV1 $PRIV2"
    for file in "$WORK"/gw*.stdout.log; do
        [[ -f "$file" ]] && { echo "--- $file"; cat "$file"; }
    done
    return 1
}
wait_private_ports

(cd "$WORK" && "$SCENE_BIN" "$WORK/zone.yaml" "$WORK/scene10.yaml") > "$WORK/scene10.stdout.log" 2>&1 & PIDS+=("$!")
(cd "$WORK" && "$SCENE_BIN" "$WORK/zone.yaml" "$WORK/scene20.yaml") > "$WORK/scene20.stdout.log" 2>&1 & PIDS+=("$!")
wait_files "$WORK/scene10.stdout.log" "$WORK/scene20.stdout.log"
wait_backends() {
    local deadline=$((SECONDS+45))
    while (( SECONDS < deadline )); do
        local ready=1
        local count10=0 count20=0
        for scene_id in 10 20; do
    local count; count=$(grep -c "backend registered: SCENE_1.*scene=$scene_id" "$WORK/logs/$(date +%F)/gateway.log" 2>/dev/null || true)
            [[ $scene_id == 10 ]] && count10=$count || count20=$count
            (( count >= 2*CHANNELS )) || ready=0
        done
        (( ready )) && return 0
        sleep .25
    done
    echo "FAIL: expected two scenes x $CHANNELS channels on both gateways; scene10=$count10 scene20=$count20"
    cat "$WORK/logs/$(date +%F)/gateway.log"
    return 1
}
wait_backends

LOG="$WORK/logs/$(date +%F)/gateway.log"
for gateway_id in 1 2; do
    for scene_id in 10 20; do
        count=$(grep -c "backend registered: SCENE_1.*scene=$scene_id" "$LOG" || true)
        (( count >= 2*CHANNELS )) || { echo "FAIL: scene=$scene_id registered channels=$count expected=$((2*CHANNELS)) across gateways"; exit 1; }
    done
done


# One load client per Gateway; all connections pause until global READY.
mkdir -p "$WORK/start-barrier"
rm -f "$WORK/start-barrier/START" "$WORK/start-barrier/ABORT" "$WORK/start-barrier"/*.ready
"$LOAD_BIN" --host 127.0.0.1 --port "$PUB1" --connections "$CONNECTIONS_PER_GATEWAY" --rps "$RPS" --duration "$DURATION" --drain "$DRAIN" --scene --player-base 1100000 --ticket "$TOKEN" --barrier-dir "$WORK/start-barrier" --gateway-metrics "$WORK/gw1.metrics.json" --scene-metrics "$WORK/scene10.metrics.json" --scene-metrics-extra "$WORK/scene20.metrics.json" --metrics-output "$WORK/metrics-gw1.jsonl" --output "$WORK/load-gw1.json" > "$WORK/load-gw1.out" 2>&1 & L1=$!
"$LOAD_BIN" --host 127.0.0.1 --port "$PUB2" --connections "$CONNECTIONS_PER_GATEWAY" --rps "$RPS" --duration "$DURATION" --drain "$DRAIN" --scene --player-base 2100000 --ticket "$TOKEN" --barrier-dir "$WORK/start-barrier" --gateway-metrics "$WORK/gw2.metrics.json" --scene-metrics "$WORK/scene10.metrics.json" --scene-metrics-extra "$WORK/scene20.metrics.json" --metrics-output "$WORK/metrics-gw2.jsonl" --output "$WORK/load-gw2.json" > "$WORK/load-gw2.out" 2>&1 & L2=$!
expected_ready=$((2*CONNECTIONS_PER_GATEWAY))
deadline=$((SECONDS+40))
while (( SECONDS < deadline )); do
    ready_count=$(find "$WORK/start-barrier" -maxdepth 1 -name 'load-gw*.json.*.ready' | wc -l)
    if (( ready_count == expected_ready )); then break; fi
    if ! kill -0 "$L1" 2>/dev/null || ! kill -0 "$L2" 2>/dev/null; then
        touch "$WORK/start-barrier/ABORT"
        echo "FAIL: load client exited before global READY barrier"
        exit 1
    fi
    sleep .1
done
(( ready_count == expected_ready )) || { touch "$WORK/start-barrier/ABORT"; echo "FAIL: global READY barrier timeout ($ready_count/$expected_ready)"; exit 1; }
python3 - "$WORK/start-barrier/START" <<'PY'
import pathlib,sys,time
target=time.clock_gettime_ns(time.CLOCK_MONOTONIC)//1_000_000+100
pathlib.Path(sys.argv[1]).write_text(f"{target}\n")
PY
# Execution boundaries only: queue depth, stale metrics, latency and errors are data.
# Keep the deprecated queue-seconds variable inert for old callers.
RECOVERY="${LOAD_RECOVERY:-0}"
load_deadline=$((SECONDS+DURATION+DRAIN+20))
while kill -0 "$L1" 2>/dev/null || kill -0 "$L2" 2>/dev/null; do
    for pid in "${PIDS[@]}"; do
        kill -0 "$pid" 2>/dev/null || { echo "FAIL: service crashed pid=$pid"; exit 1; }
    done
    if (( SECONDS > load_deadline )); then
        echo "FAIL: load execution deadline exceeded"; exit 1
    fi
    sleep .2
done
client_status=0
wait "$L1" || client_status=1
wait "$L2" || client_status=1
# Observe recovery before any acceptance assertion or service shutdown.
python3 - "$WORK" "$RECOVERY" <<'PY'
import json,pathlib,sys,time
p=pathlib.Path(sys.argv[1]); duration=int(sys.argv[2]); start=time.monotonic()
with (p/'recovery.jsonl').open('w') as out:
 for i in range(duration+1):
  row=dict(elapsedMs=int((time.monotonic()-start)*1000),snapshots={})
  for name in ('gw1','gw2','scene10','scene20'):
   try: row['snapshots'][name]=json.loads((p/f'{name}.metrics.json').read_text())
   except (OSError,ValueError) as error: row['snapshots'][name]=dict(error=str(error))
  out.write(json.dumps(row)+'\n');out.flush()
  if i<duration: time.sleep(1)
PY
acceptance_status=0
python3 - "$WORK" <<'PY' || acceptance_status=1
import json,pathlib,sys
p=pathlib.Path(sys.argv[1])
for g in (1,2):
 d=json.loads((p/f'load-gw{g}.json').read_text())
 assert d['load_attempted']>0 and d['load_attempted']==d['completed']+d['unresolved'],(g,d)
 assert d['errors']==0 and d['business_failed']==0 and d['timeouts']==0,(g,d)
 assert d['drain_final_unfinished']==0 and d['unresolved']==0,(g,d)
 assert d['logout_succeeded']==d['connections'] and d['logout_failed']==0,(g,d)
 print(f"gateway{g}: attempted={d['attempted']} completed={d['completed']} errors={d['errors']} p95_us={d['p95_us']} p99_us={d['p99_us']}")
PY

python3 - "$WORK" "$DURATION" <<'PY' || acceptance_status=1
import json,os,pathlib,sys
p=pathlib.Path(sys.argv[1]); duration=int(sys.argv[2])
for gateway in (1,2):
 path=p/f'metrics-gw{gateway}.jsonl'
 rows=[json.loads(line) for line in path.read_text().splitlines() if line.strip()]
 assert rows[0]['elapsedMs']==0 and all(a['elapsedMs']<b['elapsedMs'] for a,b in zip(rows,rows[1:])),(gateway,rows)
 assert rows[-1]['finalSample'] is True,(gateway,rows)
 assert all(r['metricsValid'] for r in rows[:-1]),(gateway,rows)
 assert all(f'gateway_{k}' in rows[0] for k in ('clientSessions','backendReady'))
 assert all(any(k.startswith(prefix) for k in rows[0]) for prefix in ('scene0_playerCount','scene1_playerCount'))
 print(f'gateway{gateway} metrics: {len(rows)} valid samples through {rows[-1]["elapsedMs"]}ms, final sample present')
PY
if (( client_status == 0 && acceptance_status == 0 )); then
    echo "PASS: 2 gateways x 2 scene instances x $CHANNELS channels; concurrent client load passed."
else
    echo "FAIL: client/metrics acceptance; evidence retained and persistence checks still attempted."
fi
if [[ -n "${LOAD_PERSISTENCE_CONTEXT:-}" ]]; then
    python3 -B "$ROOT/sceneServer/tests/persistence_load_e2e.py" --verify "$LOAD_PERSISTENCE_CONTEXT" "$WORK" || acceptance_status=1
    expected_failure=0
    [[ -f "$WORK/EXPECT_DRAIN_FAILURE" ]] && expected_failure=1
    scene_index=0
    for scene_pid in "${PIDS[@]:2}"; do kill -TERM "$scene_pid"; done
    for scene_pid in "${PIDS[@]:2}"; do
        # Bounded graceful shutdown; cleanup trap remains responsible on failure.
        for _ in $(seq 1 200); do kill -0 "$scene_pid" 2>/dev/null || break; sleep .1; done
        if kill -0 "$scene_pid" 2>/dev/null; then kill -KILL "$scene_pid"; echo "FAIL: scene shutdown timeout"; exit 1; fi
        shutdown_status=0
        wait "$scene_pid" || shutdown_status=$?
        if (( expected_failure && scene_index == 0 )); then
            (( shutdown_status == 1 )) || { echo "FAIL: save denial must report unsuccessful drain, status=$shutdown_status"; exit 1; }
        else
            (( shutdown_status == 0 )) || { echo "FAIL: scene shutdown failed"; exit 1; }
        fi
        scene_index=$((scene_index+1))
    done
    PIDS=("${PIDS[@]:0:2}")
    if (( expected_failure )); then
        grep -R -q 'shutdown persistence drain incomplete' "$WORK/logs" || { echo "FAIL: missing failed-drain evidence"; exit 1; }
        echo "PASS: save denied; bounded shutdown reports undrained state without false success"
    else
        if grep -R -E 'shutdown persistence drain incomplete|shutdown not durably drained' "$WORK/logs"; then exit 1; fi
        if (( acceptance_status == 0 )); then
            echo "PASS: isolated persistence verified and scenes durably drained"
        else
            echo "FAIL: persistence verification incomplete; scenes exited cleanly"
        fi
    fi
fi
echo "NOTE: established-client disconnect/reconnect identity is not covered; bound-session disconnection is intentionally fail-closed, not migrated."
(( client_status == 0 && acceptance_status == 0 ))
