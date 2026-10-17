#!/usr/bin/env python3
"""Search a frame dump (`batman-rt --dump`, RAM 0x0000-0x27FF per vblank) for game variables.

    ramsearch.py stats  <frames.bin> <from> <to> [--max-distinct N] [--min-changes N] [--max-changes N]
        words whose values over vblanks from..to fit the filters (few distinct values = counters,
        flags, health, lives, level number), with their value histories summarised
    ramsearch.py split  <frames.bin> <a0> <a1> <b0> <b1>
        words constant over a0..a1 and constant over b0..b1, with a different value (level number,
        mode, stage id)
    ramsearch.py corr   <frames.bin> <from> <to> <input-script> [--bit right=1000,left=2000]
        words that rise while one button is held and fall while the other is (positions)
    ramsearch.py trace  <frames.bin> <from> <to> <addr> [addr ...]
        every change of those words, one line per change

Only RAM is searched (video RAM is the PPU's). Frames are matched by their label (vblank)."""
import struct
import sys

RAM_WORDS = 0x2800
VRAM_WORDS = 0x700
FRAME_BYTES = 12 + 2 * (RAM_WORDS + VRAM_WORDS)
CHUNK = 64


def frames(path, lo, hi):
    """(vblank, RAM bytes) for lo <= vblank <= hi"""
    with open(path, "rb") as f:
        # labels are ascending and nearly equal to the record index; seek close and scan
        f.seek(max(0, lo - 2) * FRAME_BYTES)
        while True:
            blob = f.read(FRAME_BYTES)
            if len(blob) < FRAME_BYTES:
                return
            fr = struct.unpack_from("<I", blob)[0]
            if fr > hi:
                return
            if fr >= lo:
                yield fr, blob[12:12 + 2 * RAM_WORDS]


def changes(path, lo, hi):
    """yields (vblank, ram_tuple_or_None, [(addr, old, new), ...]); first item carries the full RAM"""
    prev = None
    for fr, ram in frames(path, lo, hi):
        if prev is None:
            yield fr, struct.unpack(f"<{RAM_WORDS}H", ram), []
        else:
            ch = []
            for c in range(0, 2 * RAM_WORDS, 2 * CHUNK):
                if ram[c:c + 2 * CHUNK] != prev[c:c + 2 * CHUNK]:
                    new = struct.unpack_from(f"<{CHUNK}H", ram, c)
                    old = struct.unpack_from(f"<{CHUNK}H", prev, c)
                    base = c // 2
                    for i in range(CHUNK):
                        if new[i] != old[i]:
                            ch.append((base + i, old[i], new[i]))
            yield fr, None, ch
        prev = ram


def load_input(path):
    ev = []
    for line in open(path):
        parts = line.split()
        if len(parts) == 2 and not line.lstrip().startswith("#"):
            ev.append((int(parts[0]), int(parts[1], 16)))
    return ev


def cmd_stats(path, lo, hi, max_distinct=16, min_changes=1, max_changes=10**9):
    distinct = {}
    nchanges = [0] * RAM_WORDS
    first = None
    for fr, full, ch in changes(path, lo, hi):
        if full is not None:
            first = full
            distinct = {a: {full[a]} for a in range(RAM_WORDS)}
            continue
        for a, _, new in ch:
            nchanges[a] += 1
            d = distinct.get(a)
            if d is not None:
                d.add(new)
                if len(d) > max_distinct:
                    del distinct[a]
    for a in range(RAM_WORDS):
        d = distinct.get(a)
        if d is None or not min_changes <= nchanges[a] <= max_changes:
            continue
        vals = " ".join(f"{v:04x}" for v in sorted(d))
        print(f"{a:04X}  changes {nchanges[a]:6d}  start {first[a]:04x}  values {vals}")


def cmd_split(path, a0, a1, b0, b1):
    def constant(lo, hi):
        val, alive = None, None
        for fr, full, ch in changes(path, lo, hi):
            if full is not None:
                val, alive = full, set(range(RAM_WORDS))
            for a, _, _ in ch:
                alive.discard(a)
        return val, alive
    va, ca = constant(a0, a1)
    vb, cb = constant(b0, b1)
    for a in sorted(ca & cb):
        if va[a] != vb[a]:
            print(f"{a:04X}  {va[a]:04x} -> {vb[a]:04x}")


def cmd_corr(path, lo, hi, script, plus=0x1000, minus=0x2000, top=25):
    ev = load_input(script)
    held = 0
    k = 0
    while k < len(ev) and ev[k][0] <= lo:
        held = ev[k][1]
        k += 1
    score = [0] * RAM_WORDS
    moves = [0] * RAM_WORDS
    for fr, full, ch in changes(path, lo, hi):
        while k < len(ev) and ev[k][0] <= fr:
            held = ev[k][1]
            k += 1
        d = 1 if held & plus else -1 if held & minus else 0
        for a, old, new in ch:
            delta = (new - old + 0x8000) % 0x10000 - 0x8000  # signed 16-bit step
            if abs(delta) > 64:
                continue  # jumps (respawns, wraps) say nothing about direction
            moves[a] += 1
            if d:
                score[a] += d * (1 if delta > 0 else -1)
    best = sorted(range(RAM_WORDS), key=lambda a: -abs(score[a]))[:top]
    for a in best:
        print(f"{a:04X}  score {score[a]:+6d}  small moves {moves[a]}")


def cmd_trace(path, lo, hi, addrs):
    last = {}
    for fr, full, ch in changes(path, lo, hi):
        if full is not None:
            print(f"{fr}: " + "  ".join(f"{a:04X}={full[a]:04x}" for a in addrs))
            continue
        hit = [(a, new) for a, _, new in ch if a in addrs]
        if hit:
            print(f"{fr}: " + "  ".join(f"{a:04X}={v:04x}" for a, v in hit))


def opts(args, names):
    out = {}
    while args and args[-2:-1] and args[-2].startswith("--"):
        k, v = args[-2][2:].replace("-", "_"), args[-1]
        if k not in names:
            sys.exit(f"unknown option --{k}")
        out[k] = v
        args = args[:-2]
    return args, out


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    cmd = a[0]
    if cmd == "stats":
        a, o = opts(a, {"max_distinct", "min_changes", "max_changes"})
        cmd_stats(a[1], int(a[2]), int(a[3]), **{k: int(v) for k, v in o.items()})
    elif cmd == "split":
        cmd_split(a[1], *map(int, a[2:6]))
    elif cmd == "corr":
        a, o = opts(a, {"bit"})
        kw = {}
        if "bit" in o:
            m = dict(x.split("=") for x in o["bit"].split(","))
            kw = {"plus": int(m["right"], 16), "minus": int(m["left"], 16)}
        cmd_corr(a[1], int(a[2]), int(a[3]), a[4], **kw)
    elif cmd == "trace":
        cmd_trace(a[1], int(a[2]), int(a[3]), {int(x, 16) for x in a[4:]})
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
