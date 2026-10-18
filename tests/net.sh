#!/usr/bin/env bash
# Netplay over loopback (recomp/src/net.h): a host and a guest batman-rt play random two-player
# input through a simulated network, then
#   - both must end in the same state, and have settled on the same inputs;
#   - replaying those inputs offline from the start state must give that state too;
#   - a guest whose state is knocked off on purpose must be found out and resynced.
#   tests/net.sh <workdir> [start] [frames] [lat,jitter,loss%]
# start: the vblank of session1 to start from (6000: Street Party); frames: how many to play.
# Runs in real time (60 Hz): 1500 frames take about 25 s.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?workdir}
start=${2:-6000}
frames=${3:-1500}
sim=${4:-40,30,10}
end=$((start + frames))
out=$work/net
mkdir -p "$out"
rom=$here/target/batman.bin
rt=("$here/recomp/build/batman-rt" "$rom" --mods robin-p2 --mods-dir "$here/mods")
port=$((7900 + RANDOM % 1000))

make -s -C "$here/recomp"
"${rt[@]}" --coop --input "$here/tests/inputs/session1.txt" --frames "$start" \
    --state-save-at "$start" "$out/start.st" 2>/dev/null
python3 "$here/tests/net_inputs.py" $((start + 1)) "$end" 1 > "$out/input.txt"

play() {   # play <name> <guest env...>
    local name=$1; shift
    "${rt[@]}" --host "$port" --state-load "$out/start.st" --input "$out/input.txt" --frames "$end" \
        --hash --net-sim "$sim" --net-log "$out/$name.host.log" --net-start "$out/$name.start.st" \
        > "$out/$name.host.out" 2> "$out/$name.host.err" &
    sleep 0.5
    env "$@" "${rt[@]}" --join "127.0.0.1:$port" --input "$out/input.txt" --frames "$end" \
        --hash --net-sim "$sim" --net-log "$out/$name.guest.log" \
        > "$out/$name.guest.out" 2> "$out/$name.guest.err"
    wait
    port=$((port + 1))
    grep -h "^net: [0-9]* rollbacks" "$out/$name.host.err" "$out/$name.guest.err" | sed "s/^net:/net: $name:/"
}

rc=0
play run
host=$(cat "$out/run.host.out"); guest=$(cat "$out/run.guest.out")
"${rt[@]}" --coop --state-load "$out/run.start.st" --input "$out/run.host.log" --frames "$end" --hash \
    > "$out/replay.out" 2>/dev/null
replay=$(cat "$out/replay.out")
echo "net: host   $host"; echo "net: guest  $guest"; echo "net: replay $replay"
[ "$host" = "$guest" ] && [ "$host" = "$replay" ] || { echo "net: the states differ"; rc=1; }
cmp -s "$out/run.host.log" "$out/run.guest.log" || { echo "net: the settled inputs differ"; rc=1; }
grep -q "0 desyncs" "$out/run.host.err" || { echo "net: a desync"; rc=1; }

play resync BATMAN_NET_CORRUPT=$((start + frames / 3))
host=$(cat "$out/resync.host.out"); guest=$(cat "$out/resync.guest.out")
grep -q "desync at frame" "$out/resync.host.err" || { echo "net: the knocked-off guest went unnoticed"; rc=1; }
[ "$host" = "$guest" ] && echo "net: resync: the knocked-off guest was found and resynced, same end state" \
    || { echo "net: resync: the states differ ($host / $guest)"; rc=1; }
[ $rc = 0 ] && echo "net: OK"
exit $rc
