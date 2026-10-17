#!/usr/bin/env python3
"""Compare a batman-rt run with a MAME reference capture (tools/mame/capture.lua).

  compare.py trace  <mame trace.tr> <ours.tr>      first diverging instruction
  compare.py frames <mame frames.bin> <ours.bin>   per-frame RAM / video RAM differences
  compare.py io     <mame io.log> <ours.log>       peripheral write and read sequences
  compare.py snaps  <mame dir> <ours dir>          pixel diff of fNNNNN.png snapshots
"""
import os
import struct
import sys
import zlib

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
    """(label, cycles, raw RAM + video RAM bytes) per record, in file order (ascending labels)"""
    with open(path, "rb") as f:
        while True:
            blob = f.read(FRAME_BYTES)
            if len(blob) < FRAME_BYTES:
                return
            fr, cyc = struct.unpack_from("<IQ", blob)
            yield fr, cyc, blob[12:]


def addr_of(i):
    return i if i < RAM_WORDS else 0x2900 + (i - RAM_WORDS)


def cmp_frames(ref, ours, show=8):
    """Streams both dumps and joins them on the frame label (both are in ascending order),
    so dumps of any length compare in constant memory. Frames only one side has are skipped."""
    bad = n = 0
    mine = frames(ours)
    cur = next(mine, None)
    for fa, _, wa in frames(ref):
        while cur is not None and cur[0] < fa:
            cur = next(mine, None)
        if cur is None:
            break
        if cur[0] != fa:
            continue
        wb = cur[2]
        n += 1
        if wa != wb:
            bad += 1
            if bad <= show:
                ua = struct.unpack(f"<{RAM_WORDS + VRAM_WORDS}H", wa)
                ub = struct.unpack(f"<{RAM_WORDS + VRAM_WORDS}H", wb)
                diff = [i for i in range(len(ua)) if ua[i] != ub[i]]
                desc = ", ".join(f"{addr_of(i):04X}:{ua[i]:04X}/{ub[i]:04X}" for i in diff[:10])
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


def read_png(path):
    """RGB(A) 8-bit non-interlaced PNG -> (w, h, list of rows of (r, g, b))."""
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, typ = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack_from(">IIBB", body)
            assert depth == 8 and ctype in (2, 6), f"{path}: unsupported PNG"
            bpp = 3 if ctype == 2 else 4
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1: line[i] = (line[i] + a) & 255
            elif ft == 2: line[i] = (line[i] + b) & 255
            elif ft == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(w)])
        prev = line
    return w, h, rows


def cmp_snaps(ref_dir, our_dir):
    bad = 0
    names = sorted(n for n in os.listdir(ref_dir) if n.startswith("f") and n.endswith(".png"))
    for name in names:
        ours = os.path.join(our_dir, name)
        if not os.path.exists(ours):
            print(f"{name}: missing in ours")
            bad += 1
            continue
        wa, ha, a = read_png(os.path.join(ref_dir, name))
        wb, hb, b = read_png(ours)
        if (wa, ha) != (wb, hb):
            print(f"{name}: size {wa}x{ha} vs {wb}x{hb}")
            bad += 1
            continue
        diff = [(x, y) for y in range(ha) for x in range(wa) if a[y][x] != b[y][x]]
        if diff:
            bad += 1
            x, y = diff[0]
            print(f"{name}: {len(diff)} pixels differ, first at ({x},{y}) mame {a[y][x]} ours {b[y][x]}")
    print(f"snaps: {len(names) - bad}/{len(names)} identical")
    return 1 if bad else 0


def main():
    cmds = {"trace": cmp_trace, "frames": cmp_frames, "io": cmp_io, "snaps": cmp_snaps}
    if len(sys.argv) != 4 or sys.argv[1] not in cmds:
        print(__doc__)
        return 2
    return cmds[sys.argv[1]](sys.argv[2], sys.argv[3])


if __name__ == "__main__":
    sys.exit(main())
