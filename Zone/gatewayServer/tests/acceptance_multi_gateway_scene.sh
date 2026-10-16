#!/usr/bin/env bash
# Real-process 2 gateway x 2 scene acceptance and sustained load.
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
cleanup() { for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done; for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done; }
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
sed -i -e 's/enable: true/enable: false/' -e "s#^        dir: .*#        dir: $WORK/logs#" "$WORK/zone.yaml"

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

# A real scene business request per player ensures scene login produced a usable pinned actor.
for g in 1 2; do
    pub="$PUB1"; [[ $g == 2 ]] && pub="$PUB2"
    for i in $(seq 1 20); do
        python3 - "$pub" "$((500000+g*100000+i))" "$TOKEN" <<'PY' > "$WORK/client-$g-$i.out" 2>&1 || { cat "$WORK/client-$g-$i.out"; echo "FAIL: gateway $g real client $i"; exit 1; }
import socket,struct,sys
port,pid,ticket=map(int,sys.argv[1:]); s=socket.create_connection(('127.0.0.1',port),timeout=5); s.settimeout(5)
def frame(kind,module,method,rid,body=b''):
 return struct.pack('>HBBHHQI',0xC1EB,2,kind,module,method,rid,32+len(body))+bytes(12)+body
def read():
 h=b''
 while len(h)<32: h+=s.recv(32-len(h))
 magic,ver,kind,module,method,rid,total=struct.unpack('>HBBHHQI',h[:20]); body=b''
 while len(body)<total-32: body+=s.recv(total-32-len(body))
 return kind,module,method,rid,body
def response(rid,module,method):
 while True:
  f=read()
  if f[0]==3:
   continue
  assert f[0] in (2,4) and f[1:4]==(module,method,rid),f
  return f
s.sendall(frame(5,1,1,1,struct.pack('<QQ',pid,ticket))); auth=read(); assert auth[0]==5 and auth[2]==2,auth
s.sendall(frame(1,3,4,2)); enter=response(2,3,4); assert enter[0]==2 and enter[4][:1]!=b'\x08',enter
s.sendall(frame(1,3,2,3,bytes([8,0,16,0,24,10,32,20]))); move=response(3,3,2); assert move[0]==2 and move[4][:1]!=b'\x08',move
print('auth/enter/move OK')
PY
    done
done
sleep 1
# Restart one scene instance under the same sceneId, wait for all its gateway channels to re-register,
# then prove fresh sessions still route successfully through both gateways.
SCENE1_PID="${PIDS[2]}"
kill -TERM "$SCENE1_PID" 2>/dev/null || true
wait "$SCENE1_PID" 2>/dev/null || true
PIDS[2]=""
for _ in $(seq 1 50); do
    grep -q 'scene server stopped' "$WORK/scene10.stdout.log" 2>/dev/null && break
    sleep .1
done
(cd "$WORK" && "$SCENE_BIN" "$WORK/zone.yaml" "$WORK/scene10.yaml") > "$WORK/scene10-restart.stdout.log" 2>&1 & PIDS+=("$!")
deadline=$((SECONDS+45))
while (( SECONDS < deadline )); do
    count=$(grep -c 'backend registered: SCENE_1.*scene=10' "$LOG" || true)
    (( count >= 4*CHANNELS )) && break
    sleep .25
done
(( count >= 2*CHANNELS )) || { echo "FAIL: scene 10 did not re-register on both gateways after restart"; exit 1; }
for gateway in 1 2; do
    pub="$PUB1"; [[ $gateway == 2 ]] && pub="$PUB2"
    python3 - "$pub" "$((990000+gateway))" "$TOKEN" <<'PY'
import socket,struct,sys
port,pid,ticket=map(int,sys.argv[1:]); s=socket.create_connection(('127.0.0.1',port),timeout=5); s.settimeout(5)
def send(k,m,a,r,b=b''): s.sendall(struct.pack('>HBBHHQI',0xC1EB,2,k,m,a,r,32+len(b))+bytes(12)+b)
def recv():
 h=b''
 while len(h)<32: h+=s.recv(32-len(h))
 _,_,k,m,a,r,n=struct.unpack('>HBBHHQI',h[:20]); b=b''
 while len(b)<n-32: b+=s.recv(n-32-len(b))
 return k,m,a,r,b
def response(rid,module,method):
 while True:
  f=recv()
  if f[0]==3: continue
  assert f[0] in (2,4) and f[1:4]==(module,method,rid),f
  return f
send(5,1,1,1,struct.pack('<QQ',pid,ticket)); assert recv()[2]==2
send(1,3,4,2); f=response(2,3,4); assert f[0]==2 and f[4][:1]!=b'\x08',f
send(1,3,2,3,bytes([8,0,16,0,24,10,32,20])); f=response(3,3,2); assert f[0]==2 and f[4][:1]!=b'\x08',f
PY
done
echo "PASS: scene 10 restarted; new clients completed enter/move through both gateways."

# Concurrent sustained load against both public listeners.
"$LOAD_BIN" --host 127.0.0.1 --port "$PUB1" --connections "$CONNECTIONS_PER_GATEWAY" --rps "$RPS" --duration "$DURATION" --drain "$DRAIN" --scene --player-base 1100000 --ticket "$TOKEN" --gateway-metrics "$WORK/gw1.metrics.json" --scene-metrics "$WORK/scene10.metrics.json" --scene-metrics-extra "$WORK/scene20.metrics.json" --metrics-output "$WORK/metrics-gw1.jsonl" --output "$WORK/load-gw1.json" > "$WORK/load-gw1.out" 2>&1 & L1=$!
"$LOAD_BIN" --host 127.0.0.1 --port "$PUB2" --connections "$CONNECTIONS_PER_GATEWAY" --rps "$RPS" --duration "$DURATION" --drain "$DRAIN" --scene --player-base 2100000 --ticket "$TOKEN" --gateway-metrics "$WORK/gw2.metrics.json" --scene-metrics "$WORK/scene10.metrics.json" --scene-metrics-extra "$WORK/scene20.metrics.json" --metrics-output "$WORK/metrics-gw2.jsonl" --output "$WORK/load-gw2.json" > "$WORK/load-gw2.out" 2>&1 & L2=$!

if [[ "$MAX_QUEUE_SECONDS" -gt 0 ]]; then
    python3 - "$WORK" "$DURATION" "$MAX_QUEUE_SECONDS" <<'PY' &
import json,pathlib,sys,time
p=pathlib.Path(sys.argv[1]); duration=int(sys.argv[2]); limit=int(sys.argv[3])
stop_file=p/'EARLY_STOP'; drained_file=p/'QUEUE_DRAINED_DURING_LOAD.txt'; stale=p/'STALE_METRICS'
stop_file.unlink(missing_ok=True); drained_file.unlink(missing_ok=True)
stale.unlink(missing_ok=True)
last_elapsed=-1; streak=0; had_growth=False; started=False; end=time.monotonic()+duration+2
while time.monotonic()<end:
 rows=[]
 for g in (1,2):
  f=p/f'metrics-gw{g}.jsonl'
  try: rows.append([json.loads(x) for x in f.read_text().splitlines() if x.strip()])
  except (OSError,json.JSONDecodeError): rows=[]; break
 if len(rows)==2 and rows[0] and rows[1]:
  elapsed=min(rows[0][-1]['elapsedMs'],rows[1][-1]['elapsedMs'])
  if elapsed>last_elapsed:
   last_elapsed=elapsed
   started=started or elapsed>=1000
   latest=rows[0][-1]
   queues=max(max(sum(r[-1].get(f'scene{s}_worker{w}_queue',0) for w in range(4)) for s in (0,1)) for r in rows)
   ready=min(r[-1].get('gateway_backendReady',0) for r in rows)
   streak=streak+1 if queues>0 else 0
   pending=max(r[-1].get('gateway_pendingRoutes',0) for r in rows)
   invalid = any(r[-1].get('metricsValid') is not True for r in rows)
   stale_sample = any(not (p/f'gw{g}.metrics.json').exists() or (time.time()-(p/f'gw{g}.metrics.json').stat().st_mtime)>3 for g in (1,2))
   if started and (ready<4 or streak>=limit or stale_sample or invalid):
    stop_file.write_text(f'elapsedMs={elapsed} backendReady={ready} sceneWorkerQueueSum={queues} pendingRoutes={pending} consecutiveQueueSamples={streak}\n')
    if stale_sample or invalid: stale.write_text(f'elapsedMs={elapsed} staleOrInvalidMetrics=1\n')
    print('EARLY_STOP:',stop_file.read_text().strip(),flush=True)
    sys.exit(10)
   had_growth = had_growth or queues>0 or pending>0
   if elapsed >= duration*1000 and had_growth and queues==0 and pending==0:
    drained_file.write_text(f'elapsedMs={elapsed} queue=0 pendingRoutes=0; no early stop needed\n')
    sys.exit(0)
 time.sleep(.1)
sys.exit(0)
PY
    MONITOR=$!
    for _ in $(seq 1 $((DURATION*10+30))); do
        if ! kill -0 "$L1" 2>/dev/null || ! kill -0 "$L2" 2>/dev/null; then break; fi
        if [[ -f "$WORK/EARLY_STOP" || -f "$WORK/QUEUE_DRAINED_DURING_LOAD.txt" ]]; then break; fi
        sleep .1
    done
    if [[ -f "$WORK/EARLY_STOP" && ! -f "$WORK/QUEUE_DRAINED_DURING_LOAD.txt" ]]; then
        stop_load_clients
        wait "$MONITOR" 2>/dev/null || true
        wait "$L1" 2>/dev/null || true
        wait "$L2" 2>/dev/null || true
        echo "FAIL: load stopped by guard; clients were asked to stop and drain within their configured deadline."
        exit 1
    fi
    wait "$MONITOR" || { echo "FAIL: load health monitor failed"; exit 1; }
    MONITOR=""
    if [[ -f "$WORK/EARLY_STOP" && ! -f "$WORK/QUEUE_DRAINED_DURING_LOAD.txt" ]]; then
        stop_load_clients
        wait "$L1" 2>/dev/null || true
        wait "$L2" 2>/dev/null || true
        echo "FAIL: load stopped by guard; clients were asked to stop and drain within their configured deadline."
        exit 1
    fi
fi

if ! wait "$L1"; then cat "$WORK/load-gw1.out"; exit 1; fi
if ! wait "$L2"; then cat "$WORK/load-gw2.out"; exit 1; fi
python3 - "$WORK" <<'PY'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1])
for g in (1,2):
 d=json.loads((p/f'load-gw{g}.json').read_text())
 assert d['load_attempted']>0 and d['load_attempted']==d['completed']+d['unresolved'],(g,d)
 if d['drain_sec']>0:
  assert d['drain_final_unfinished'] <= d['cutoff_unfinished'],(g,d)
  if not (p/'EARLY_STOP').exists():
   assert d['errors']==0 and d['business_failed']==0 and d['timeouts']==0,(g,d)
 print(f"gateway{g}: attempted={d['attempted']} completed={d['completed']} errors={d['errors']} p95_us={d['p95_us']} p99_us={d['p99_us']}")
PY

python3 - "$WORK" "$DURATION" <<'PY'
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
echo "PASS: 2 gateways x 2 scene instances x $CHANNELS channels; concurrent client load passed."
echo "NOTE: established-client disconnect/reconnect identity is not covered; bound-session disconnection is intentionally fail-closed, not migrated."