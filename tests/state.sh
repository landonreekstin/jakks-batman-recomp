#!/usr/bin/env bash
# Save-state round trip: a run that saves at vblank N, then a second process that loads the
# state and runs on, must produce exactly what one uninterrupted run produces.
#   tests/state.sh <workdir> [save-at] [frames]
# Env: INPUT=<script> as for check.sh. If <workdir>/mame/frames.bin exists (from check.sh with
# the same INPUT), the resumed frames are also compared with MAME.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?workdir}
at=${2:-3000}
frames=${3:-5000}
rom=$here/target/batman.bin
rt=$here/recomp/build/batman-rt
out=$work/state
mkdir -p "$out"

make -s -C "$here/recomp" OUT=build/batman-rt
inp=()
[ -n "${INPUT:-}" ] && inp=(--input "$INPUT")

"$rt" "$rom" "${inp[@]}" --frames "$frames" --dump "$out/straight.bin" --iolog "$out/straight.io"
"$rt" "$rom" "${inp[@]}" --frames "$at" --state-save-at "$at" "$out/at.state"
"$rt" "$rom" "${inp[@]}" --frames "$frames" --state-load "$out/at.state" \
    --dump "$out/resumed.bin" --iolog "$out/resumed.io"

rc=0
# frame records are 12 + 2*(0x2800+0x700) bytes; the straight dump also has frames 0..at
rec=$((12 + 2 * (0x2800 + 0x700)))
if cmp -s <(tail -c +$(((at + 1) * rec + 1)) "$out/straight.bin") "$out/resumed.bin"; then
    echo "state: frames $((at + 1))-$frames identical to the uninterrupted run"
else
    echo "state: resumed frames differ from the uninterrupted run"; rc=1
fi
# io labels are frame + 1, so accesses made after the save carry labels > at
if cmp -s <(awk -v at="$at" '$1 > at' "$out/straight.io") "$out/resumed.io"; then
    echo "state: $(wc -l < "$out/resumed.io") peripheral accesses identical"
else
    echo "state: peripheral accesses differ"; rc=1
fi
if [ -f "$work/mame/frames.bin" ]; then
    python3 "$here/tests/compare.py" frames "$work/mame/frames.bin" "$out/resumed.bin" || rc=1
fi
exit $rc
