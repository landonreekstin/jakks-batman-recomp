#!/usr/bin/env python3
"""An input script of random play, for tests/net.sh (two players) and tests/vsnet.sh (up to 8):
each player changes direction and buttons every 3-40 frames (no Menu, so nobody pauses).

    tests/net_inputs.py <first frame> <end frame> <seed> [players] > script.txt"""
import random
import sys


def main():
    start, end, seed = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    np = int(sys.argv[4]) if len(sys.argv) > 4 else 2
    r = random.Random(seed)
    moves = [0, 0x1000, 0x2000, 0x8000, 0x4000, 0x9000, 0x5000, 0xA000]
    acts = [0, 0, 0x0200, 0x0800, 0x0100, 0x0A00]
    cur, nxt = [0] * np, [start + 7 * p for p in range(np)]
    if np == 2:
        print("# vblank P1 P2 (tests/net_inputs.py %d %d %d)" % (start, end, seed))
    else:
        print("# vblank P1..P%d (tests/net_inputs.py %d %d %d %d)" % (np, start, end, seed, np))
    for f in range(start, end):
        changed = False
        for p in range(np):
            if f >= nxt[p]:
                cur[p] = r.choice(moves) | r.choice(acts)
                nxt[p] = f + r.randint(3, 40)
                changed = True
        if changed:
            print(f"{f} " + " ".join(f"{c:04x}" for c in cur))


if __name__ == "__main__":
    main()
