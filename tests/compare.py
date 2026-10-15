#!/usr/bin/env python3
"""Compare a batman-rt run with a MAME reference capture (tools/mame/capture.lua).

  compare.py trace  <mame trace.tr> <ours.tr>      first diverging instruction
  compare.py frames <mame frames.bin> <ours.bin>   per-frame RAM / video RAM differences
  compare.py io     <mame io.log> <ours.log>       peripheral write and read sequences
"""
import struct
import sys

RAM_WORDS = 0x2800
VRAM_WORDS = 0x700
FRAME_BYTES = 12 + 2 * (RAM_WORDS + VRAM_WORDS)


def trace_lines(path):
    with open(path) as f:
        for line in f:
            # MAME: "R1=.. SR=.. 005DCF: disasm"; ours: "R1=.. SR=.. 005DCF".
            # MAME also logs "(interrupted at 009438, IRQ 1)" markers, skipped here.
            if line.startswith("R1="):
                yield line.split(":")[0].strip()


def cmp_trace(ref, ours):
    a, b = trace_lines(ref), trace_lines(ours)
    first = next(a)
    skipped = 0
    for line in b:  # MAME's trace starts a few instructions late; align on its first line
        if line == first:
            break
        skipped += 1
    else:
        print("trace: could not align")
        return 1
    n = 1
    prev = first
    for x, y in zip(a, b):
        if x != y:
            print(f"trace: diverged after {n} matching instructions (ours skipped {skipped})")
            print(f"  previous: {prev}")
            print(f"  mame:     {x}")
            print(f"  ours:     {y}")
            return 1
        prev = x
        n += 1
    print(f"trace: {n} instructions match")
    return 0


def frames(path):
    with open(path, "rb") as f:
        while True:
            blob = f.read(FRAME_BYTES)
            if len(blob) < FRAME_BYTES:
                return
            fr, cyc = struct.unpack_from("<IQ", blob)
            words = struct.unpack_from(f"<{RAM_WORDS + VRAM_WORDS}H", blob, 12)
            yield fr, cyc, words


def addr_of(i):
    return i if i < RAM_WORDS else 0x2900 + (i - RAM_WORDS)


def cmp_frames(ref, ours, show=8):
    bad = 0
    n = 0
    mine = {fr: (cyc, words) for fr, cyc, words in frames(ours)}
    for fa, ca, wa in frames(ref):
        if fa not in mine:
            continue
        cb, wb = mine[fa]
        n += 1
        diff = [i for i in range(len(wa)) if wa[i] != wb[i]]
        if diff:
            bad += 1
            if bad <= show:
                desc = ", ".join(f"{addr_of(i):04X}:{wa[i]:04X}/{wb[i]:04X}" for i in diff[:10])
                print(f"frame {fa}: {len(diff)} words differ (mame/ours) {desc}{' ...' if len(diff) > 10 else ''}")
    print(f"frames: {n - bad}/{n} identical")
    return 1 if bad else 0


def io_records(path, kind):
    with open(path) as f:
        for line in f:
            fr, rw, addr, data = line.split()
            if rw == kind:
                yield int(fr), addr, data


def cmp_io(ref, ours):
    rc = 0
    for kind in ("W", "R"):
        n = 0
        for x, y in zip(io_records(ref, kind), io_records(ours, kind)):
            if x != y:
                print(f"io {kind}: diverged after {n} records: mame {x} ours {y}")
                rc = 1
                break
            n += 1
        else:
            print(f"io {kind}: {n} records match")
    return rc


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("trace", "frames", "io"):
        print(__doc__)
        return 2
    return {"trace": cmp_trace, "frames": cmp_frames, "io": cmp_io}[sys.argv[1]](sys.argv[2], sys.argv[3])


if __name__ == "__main__":
    sys.exit(main())
