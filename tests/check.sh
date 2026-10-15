#!/usr/bin/env bash
# Run MAME and batman-rt on the same frames and compare them.
#   tests/check.sh <workdir> [frames] [--trace]
# --trace also diffs every instruction (MAME runs with -debug; keep frames small, ~93k instr/frame).
# The MAME capture is cached in <workdir>/mame; delete it to recapture.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?workdir}
frames=${2:-600}
trace=${3:-}
rom=$here/target/batman.bin

mkdir -p "$work/roms/jak_batm" "$work/mame/nvram" "$work/ours"
ln -sf "$rom" "$work/roms/jak_batm/batman.bin"

if [ ! -f "$work/mame/frames.bin" ]; then
    debug=()
    [ -n "$trace" ] && debug=(-debug -debugger none)
    # LD_LIBRARY_PATH from the devshell breaks MAME's libasound (glibc mismatch)
    env -u LD_LIBRARY_PATH CAP_DIR="$work/mame" CAP_FRAMES="$frames" ${trace:+CAP_TRACE=1} \
        mame jak_batm -rompath "$work/roms" -video none -sound none -nothrottle \
        -cfg_directory "$work/mame/cfg" -nvram_directory "$work/mame/nvram" "${debug[@]}" \
        -autoboot_script "$here/tools/mame/capture.lua" >/dev/null
fi

make -s -C "$here/recomp" OUT=build/batman-rt
args=(--frames "$frames" --iolog "$work/ours/io.log" --dump "$work/ours/frames.bin")
[ -n "${REPLAY:-}" ] && args+=(--replay "$work/mame/io.log")
[ -n "$trace" ] && args+=(--trace "$work/ours/trace.tr")
"$here/recomp/build/batman-rt" "$rom" "${args[@]}" || true

rc=0
[ -n "$trace" ] && { python3 "$here/tests/compare.py" trace "$work/mame/trace.tr" "$work/ours/trace.tr" || rc=1; }
python3 "$here/tests/compare.py" frames "$work/mame/frames.bin" "$work/ours/frames.bin" || rc=1
python3 "$here/tests/compare.py" io "$work/mame/io.log" "$work/ours/io.log" || rc=1
exit $rc
