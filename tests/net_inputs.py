#!/usr/bin/env python3
"""A two-player input script of random play, for tests/net.sh: each player changes direction and
buttons every 3-40 frames (no Menu, so nobody pauses).

    tests/net_inputs.py <first frame> <end frame> <seed> > script.txt"""
import random
import sys


def main():
    start, end, seed = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    r = random.Random(seed)
    moves = [0, 0x1000, 0x2000, 0x8000, 0x4000, 0x9000, 0x5000, 0xA000]
    acts = [0, 0, 0x0200, 0x0800, 0x0100, 0x0A00]
    cur, nxt = [0, 0], [start, start + 7]
    print("# vblank P1 P2 (tests/net_inputs.py %d %d %d)" % (start, end, seed))
    for f in range(start, end):
        changed = False
        for p in range(2):
            if f >= nxt[p]:
                cur[p] = r.choice(moves) | r.choice(acts)
                nxt[p] = f + r.randint(3, 40)
                changed = True
        if changed:
            print(f"{f} {cur[0]:04x} {cur[1]:04x}")


if __name__ == "__main__":
    main()
