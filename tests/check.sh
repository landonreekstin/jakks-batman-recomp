#!/usr/bin/env bash
# Run MAME and batman-rt on the same frames and compare them.
#   tests/check.sh <workdir> [frames] [--trace]
# --trace also diffs every instruction (MAME runs with -debug; keep frames small, ~93k instr/frame).
# The MAME capture is cached in <workdir>/mame; delete it to recapture.
# Env: INPUT=<script> (tests/inputs/*.txt), SNAPS=<a,b,c> vblanks to compare as images,
#      REPLAY=1 feed MAME's peripheral reads back in.
#      MODS=a,b  run batman-rt with those mods and MAME on the ROM file with them baked in
#      (data mods only: plugins and pokes have no MAME equivalent).
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?workdir}
frames=${2:-600}
trace=${3:-}
rom=$here/target/batman.bin

mkdir -p "$work/roms/jak_batm" "$work/mame/nvram" "$work/ours"
make -s -C "$here/recomp" OUT=build/batman-rt
if [ -n "${MODS:-}" ]; then
    "$here/recomp/build/batman-rt" "$rom" --mods "$MODS" --mods-dir "$here/mods" --bake "$work/baked.bin" --frames 0
    rom=$work/baked.bin
fi
ln -sf "$rom" "$work/roms/jak_batm/batman.bin"

if [ ! -f "$work/mame/frames.bin" ]; then
    debug=()
    [ -n "$trace" ] && debug=(-debug -debugger none)
    # LD_LIBRARY_PATH from the devshell breaks MAME's libasound (glibc mismatch)
    env -u LD_LIBRARY_PATH CAP_DIR="$work/mame" CAP_FRAMES="$frames" ${trace:+CAP_TRACE=1} \
        ${INPUT:+CAP_INPUT=$INPUT} ${SNAPS:+CAP_SNAPS=$SNAPS} \
        mame jak_batm -rompath "$work/roms" -video none -sound none -nothrottle \
        -cfg_directory "$work/mame/cfg" -nvram_directory "$work/mame/nvram" "${debug[@]}" \
        -snapshot_directory "$work/mame/snap" \
        -autoboot_script "$here/tools/mame/capture.lua" >/dev/null
fi

args=(--frames "$frames" --iolog "$work/ours/io.log" --dump "$work/ours/frames.bin")
[ -n "${REPLAY:-}" ] && args+=(--replay "$work/mame/io.log")
[ -n "${MODS:-}" ] && args+=(--mods "$MODS" --mods-dir "$here/mods")
[ -n "$trace" ] && args+=(--trace "$work/ours/trace.tr")
[ -n "${INPUT:-}" ] && args+=(--input "$INPUT")
[ -n "${SNAPS:-}" ] && args+=(--snap "$work/ours" --snap-frames "$SNAPS")
"$here/recomp/build/batman-rt" "$here/target/batman.bin" "${args[@]}" || true

rc=0
[ -n "$trace" ] && { python3 "$here/tests/compare.py" trace "$work/mame/trace.tr" "$work/ours/trace.tr" || rc=1; }
python3 "$here/tests/compare.py" frames "$work/mame/frames.bin" "$work/ours/frames.bin" || rc=1
python3 "$here/tests/compare.py" io "$work/mame/io.log" "$work/ours/io.log" || rc=1
[ -n "${SNAPS:-}" ] && { python3 "$here/tests/compare.py" snaps "$work/mame/snap" "$work/ours" || rc=1; }
exit $rc
