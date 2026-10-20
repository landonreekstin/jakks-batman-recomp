#!/usr/bin/env bash
# Versus netplay over loopback (recomp/src/vsnet.h): a host and two guests batman-rt play random
# input for slots 1-3 of a 4-fighter match (slot 4 a CPU) through a simulated network, then
#   - all three must end in the same match state;
#   - the host's settled inputs (--net-log), replayed offline, give that state too (the CPU slot
#     mask is the online one: slots 4-8);
#   - a guest knocked off on purpose (BATMAN_NET_CORRUPT) must be found out and resynced, and
#     everyone still ends the same.
#   tests/vsnet.sh <workdir> [frames] [lat,jitter,loss%]
# Runs in real time (60 Hz).
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?workdir}
frames=${2:-1500}
sim=${3:-40,30,10}
rom=$here/target/batman.bin
rt=$here/recomp/build/batman-rt
mkdir -p "$work"
make -s -C "$here/recomp" OUT=build/batman-rt
python3 "$here/tests/net_inputs.py" 0 "$frames" 23 4 > "$work/in.txt"
port=$((7900 + RANDOM % 90))

play() {   # name [env for guest 2]
    local name=$1 corrupt=${2:-}
    local a=(--versus --players 4 --input "$work/in.txt" --frames "$frames" --net-sim "$sim")
    "$rt" "$rom" "${a[@]}" --host "$port" --net-players 3 --net-log "$work/$name.log" \
        > "$work/$name.host" 2> "$work/$name.host.err" &
    sleep 0.3
    "$rt" "$rom" "${a[@]}" --join "127.0.0.1:$port" > "$work/$name.g1" 2> "$work/$name.g1.err" &
    sleep 0.3
    env ${corrupt:+BATMAN_NET_CORRUPT=$corrupt} "$rt" "$rom" "${a[@]}" --join "127.0.0.1:$port" \
        > "$work/$name.g2" 2> "$work/$name.g2.err" &
    wait
    port=$((port + 1))
    for f in host g1 g2; do sed 's/^/  /' "$work/$name.$f"; grep 'rollbacks' "$work/$name.$f.err" | sed 's/^/    /'; done
    [ "$(cut -d' ' -f2 "$work/$name".{host,g1,g2} | sort -u | wc -l)" = 1 ]
}

rc=0
if play run; then echo "vsnet: run: all three end the same: OK"; else echo "vsnet: run: FAIL"; rc=1; fi
replay=$("$rt" "$rom" --versus --players 4 --bots 4,5,6,7,8 --input "$work/run.log" --frames "$frames" --hash 2>/dev/null | cut -d' ' -f2)
if [ "$replay" = "$(cut -d' ' -f2 "$work/run.host")" ]; then echo "vsnet: replay of the settled inputs: same state: OK"
else echo "vsnet: replay: FAIL ($replay)"; rc=1; fi
if play resync $((frames / 2)) && grep -q 'desynced' "$work/resync.host.err"; then
    echo "vsnet: resync: the knocked-off guest was found and resynced, same end state: OK"
else echo "vsnet: resync: FAIL"; rc=1; fi
exit $rc
