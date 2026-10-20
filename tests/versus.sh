#!/usr/bin/env bash
# Versus mode (recomp/src/versus.c) checks, headless:
#   - determinism: a 4-CPU match run twice, and split by a save/load, ends in the same state;
#   - platforms: jump up through the catwalk, land on it, drop through it (inputs/versus-platform.txt);
#   - combo: Batman's three hits do 3 + 3 + 6 percent (inputs/versus-combo.txt);
#   - stocks: an idle P1 against a CPU loses its 3 stocks and the CPU wins;
#   - a 4-CPU match finishes (one winner, nobody stuck);
#   - block: a shield takes Joker's mallet with no damage, and breaks when held too long
#     (inputs/versus-block.txt); a roll from the shield (inputs/versus-roll.txt);
#   - moves: Batman's Batarang, glide and recovery (inputs/versus-specials.txt);
#   - ledges: walking off the floor catches its ledge, Up jumps back up (inputs/versus-ledge.txt);
#     on the Clock Tower the grapnel pulls Batman up to a ledge, and he climbs on (versus-grapnel.txt);
#   - Mr. Freeze: the freeze shot freezes with no damage, his ice holds him up, then melts
#     (inputs/versus-freeze.txt);
#   - character select: P1 picks Bane and adds a fifth fighter, and the match waits for Start
#     (inputs/versus-select.txt);
#   - the title menu's Versus item: Down twice and A pick it (inputs/versus-menu.txt), and the
#     game without the item is untouched (same state hash as a run without --versus-menu up to
#     the title menu).
#   tests/versus.sh [workdir]   (default: a temporary directory)
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=${1:-$(mktemp -d)}
mkdir -p "$work"
rom=$here/target/batman.bin
rt=$here/recomp/build/batman-rt
make -s -C "$here/recomp" OUT=build/batman-rt
fail=0
check() {   # name, then a command whose success is the check
    local name=$1; shift
    if "$@"; then echo "ok    $name"; else echo "FAIL  $name"; fail=1; fi
}
vs() { "$rt" "$rom" --versus "$@" 2>&1; }

a=$(vs --frames 3600 --hash | grep '^versus [0-9a-f]')
b=$(vs --frames 3600 --hash | grep '^versus [0-9a-f]')
vs --frames 1777 --state-save-at 1777 "$work/vs.state" >/dev/null
c=$(vs --frames 3600 --state-load "$work/vs.state" --hash | grep '^versus [0-9a-f]')
echo "  $a / $b / $c"
check "same match twice" test "$a" = "$b"
check "save at 1777, load, run on" test "$a" = "$c"

p=(--players 2 --bots none --input "$here/tests/inputs/versus-platform.txt")
check "lands on the catwalk" grep -q 'P1 .* at 136,112 on 2' <(vs "${p[@]}" --frames 260)
check "drops through to the floor" grep -q 'P1 .* at 136,208 on 1' <(vs "${p[@]}" --frames 360)

check "three-hit combo" grep -q 'P2 JOKER .* damage  12%' \
    <(vs --players 2 --bots none --input "$here/tests/inputs/versus-combo.txt" --frames 300)

out=$(vs --players 2 --bots 2 --frames 20000)
echo "$out" | sed 's/^/  /'
check "idle P1 loses its stocks to the CPU" grep -q 'winner 2' <<<"$out"
check "the CPU took all three" grep -q 'P2 .* KOs 3' <<<"$out"

out=$(vs --players 4 --frames 20000)
echo "$out" | sed 's/^/  /'
check "a 4-CPU match finishes" grep -qE 'matches [1-9]' <<<"$out"

b=(--players 2 --bots none --input "$here/tests/inputs/versus-block.txt")
check "block: no damage through the shield" grep -q 'P1 BATMAN .* damage   0% .* state 5 shield 7$' <(vs "${b[@]}" --frames 260)
check "block: the third hit breaks it" grep -q 'P1 BATMAN .* damage   0% .* state 7 ' <(vs "${b[@]}" --frames 300)
check "block: a roll" grep -q 'P1 BATMAN .* at 192,208 on 1 state 0' \
    <(vs --players 2 --bots none --input "$here/tests/inputs/versus-roll.txt" --frames 230)

sp=(--players 2 --bots none --input "$here/tests/inputs/versus-specials.txt")
check "specials: the Batarang hits from afar" grep -q 'P2 JOKER .* damage   4%' <(vs "${sp[@]}" --frames 200)
check "specials: the glide kick" grep -q 'P2 JOKER .* damage  12%' <(vs "${sp[@]}" --frames 280)
check "specials: the recovery reaches the catwalk" grep -q 'P1 BATMAN .* on 2 state 0' <(vs "${sp[@]}" --frames 340)

l=(--players 2 --bots none --input "$here/tests/inputs/versus-ledge.txt")
check "ledge: caught on the way down" grep -q 'P1 BATMAN .* at 56,244 on 0 state 8' <(vs "${l[@]}" --frames 230)
check "ledge: jumped back onto the floor" grep -q 'P1 BATMAN .* on 1 state 0' <(vs "${l[@]}" --frames 340)
g=(--stage 1 --players 2 --bots none --input "$here/tests/inputs/versus-grapnel.txt")
check "grapnel: on the line" grep -q 'P1 BATMAN .* state 9 ' <(vs "${g[@]}" --frames 214)
check "grapnel: hanging from the ledge" grep -q 'P1 BATMAN .* at 376,356 on 0 state 8' <(vs "${g[@]}" --frames 250)
check "grapnel: climbed onto the platform" grep -q 'P1 BATMAN .* at [0-9]*,320 on 2 state 0' <(vs "${g[@]}" --frames 280)
out=$(vs --stage 1 --players 4 --frames 20000)
echo "$out" | sed 's/^/  /'
check "a 4-CPU match on the Clock Tower finishes" grep -qE 'matches [1-9]' <<<"$out"

fz=(--players 2 --bots none --chars 5,1 --input "$here/tests/inputs/versus-freeze.txt")
check "freeze: frozen, no damage" grep -q 'P2 JOKER .* damage   0% .* state 10 ' <(vs "${fz[@]}" --frames 200)
check "freeze: standing on his ice" grep -q 'P1 MR.FREEZE .* at 145,150 on 100 ' <(vs "${fz[@]}" --frames 300)
check "freeze: the ice melts" grep -q 'P1 MR.FREEZE .* at 145,208 on 1 ' <(vs "${fz[@]}" --frames 380)

out=$(vs --select --players 4 --bots 2,3,4 --input "$here/tests/inputs/versus-select.txt" --frames 120)
check "select: P1 picked Bane" grep -q 'P1 BANE .* human stocks 3' <<<"$out"
check "select: a fifth fighter was added" grep -q 'P5 PENGUIN .* cpu' <<<"$out"
check "select: picked, it waits for Start" grep -q 'phase 3,' \
    <(vs --select --players 4 --bots 2,3,4 --input "$here/tests/inputs/versus-select.txt" --frames 95)
check "select: Start begins the countdown" grep -q 'phase 0,' <<<"$out"

check "title menu: Versus is picked" grep -q 'versus requested at vblank 1301' \
    <("$rt" "$rom" --versus-menu --input "$here/tests/inputs/versus-menu.txt" --frames 1400 2>&1)
a=$("$rt" "$rom" --frames 500 --hash 2>/dev/null)
b=$("$rt" "$rom" --frames 500 --hash --versus-menu 2>/dev/null)
check "boot to the title unchanged by the menu item" test "$a" = "$b"

exit $fail
