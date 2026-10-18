#!/usr/bin/env python3
"""Generates mods/robin-p2/mod.txt: Robin as co-op player 2 (run with --coop).

    mods/robin-p2/generate.py target/batman.bin > mods/robin-p2/mod.txt   (make -C recomp does this)

The same Robin as mods/robin (see its generate.py), but written into the upper ROM copy, which
P2's update reads its sprites from (recomp/src/coop.c), with new tile numbers so Batman (P1) keeps
his. Native only: MAME has no co-op."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "robin"))
import generate  # noqa: E402

if __name__ == "__main__":
    generate.main(p2=True)
