#!/usr/bin/env bash
# External supervisor: only the named transient service is stopped on failure.
set -euo pipefail
unit=${1:?unit name required}
work=${2:?absolute evidence directory required}
shift 2
[[ $unit =~ ^[a-zA-Z0-9_-]+$ && $work = /* && $# -gt 0 ]] || exit 2
mkdir -p "$work"
work=$(realpath "$work")
log=$work/guard.log
min_available=${GUARD_MIN_AVAILABLE_KB:-4194304}
file_max=${GUARD_FILE_MAX_BYTES:-67108864}
total_max=${GUARD_TOTAL_MAX_BYTES:-268435456}
rss_max=${GUARD_SCENE_RSS_MAX_KB:-1572864}
for n in "$min_available" "$file_max" "$total_max" "$rss_max"; do
    [[ $n =~ ^[0-9]+$ ]] || exit 2
done
systemctl --user is-active --quiet "$unit" && exit 2
cleanup() { systemctl --user stop "$unit" >/dev/null 2>&1 || true; }
trap cleanup EXIT
trap 'exit 130' INT TERM
abort() { echo "ABORT $*" >> "$log"; exit 1; }
available=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
[[ $available -ge $min_available ]] || abort available-memory
systemd-run --user --quiet --unit="$unit" --working-directory="$work" \
    -p MemoryHigh=3G -p MemoryMax=4G -p MemorySwapMax=0 -p TasksMax=256 \
    -p RuntimeMaxSec=180 -p TimeoutStopSec=5 -p KillMode=control-group \
    -p LimitFSIZE=67108864 -p LimitCORE=0 \
    -p "StandardOutput=append:$work/runner.log" -p StandardError=inherit "$@"
for ((i=0;i<180;i++)); do
    state=$(systemctl --user show "$unit" -p ActiveState --value)
    [[ $state == active || $state == activating ]] && break
    [[ $state == failed || $state == inactive ]] && break
    sleep .05
done
cg=$(systemctl --user show "$unit" -p ControlGroup --value)
[[ $cg = /user.slice/* && -d /sys/fs/cgroup$cg ]] || abort missing-cgroup
cg=/sys/fs/cgroup$cg
[[ $(cat "$cg/memory.max") == 4294967296 && $(cat "$cg/memory.high") == 3221225472 && $(cat "$cg/memory.swap.max") == 0 ]] || abort invalid-limits
echo "cgroup=$cg" >> "$log"
while systemctl --user is-active --quiet "$unit"; do
    available=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
    echo "$(date -Iseconds) memory.current=$(cat "$cg/memory.current") availableKB=$available" >> "$log"
    cat "$cg/memory.events" >> "$log"
    cat "$cg/memory.pressure" >> "$log"
    [[ $available -ge $min_available ]] || abort available-memory
    awk '$1 != "low" && $2 != 0 {bad=1} END {exit bad}' "$cg/memory.events" || abort memory-event
    while read -r pid; do
        [[ -r /proc/$pid/comm ]] || continue
        if [[ $(cat /proc/"$pid"/comm) == sceneServer ]]; then
            rss=$(awk '/VmRSS/{print $2}' /proc/"$pid"/status)
            echo "scene pid=$pid rssKB=${rss:-0}" >> "$log"
            [[ ${rss:-0} -le $rss_max ]] || abort scene-rss
        fi
    done < "$cg/cgroup.procs"
    read -r largest total < <(find "$work" -type f -printf '%s\n' | awk '{s+=$1;if($1>m)m=$1} END{print m+0,s+0}')
    [[ $largest -le $file_max ]] || abort file-limit
    [[ $total -le $total_max ]] || abort total-limit
    sleep 1
done
result=$(systemctl --user show "$unit" -p Result --value)
echo "result=$result" >> "$log"
[[ $result == success ]]