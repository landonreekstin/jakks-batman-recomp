# Versus: a Smash-style fighting mode

`recomp/src/versus.c` (`versus.h`). It runs from `batman --versus` (SDL) and
`batman-rt --versus` (headless, for tests).

## Design

- **Native engine.** The game's CPU doesn't run during a match. The ROM supplies everything drawn:
  - the characters' sprite frames, with their hit and hurt boxes;
  - their palettes;
  - a scene's tilemaps.

  `versus_present` writes the video registers, tilemaps, palettes and sprite RAM the way Street
  Party sets them up, then calls `spg_render`, so the picture comes from the same PPU port as
  the game's.
  - The co-op approach (swapping a second player's state into the game's one player slot) doesn't
    stretch to 8 players or to Smash physics.
  - Why not: patching the brawler's damage and movement everywhere would be fragile.
- **Deterministic plain data.** `versus_t` holds the whole match: no pointers, integers only
  (1/256 px), and a xorshift RNG in the struct.
  - The same inputs give the same match, and a saved state continues exactly (`tests/versus.sh`).
  - That is what rollback netplay needs later.
- **Buttons.** Port A's layout plus block (`VS_*`): A jumps, B attacks, X is the special, and Up
  also jumps (tap jump). A controller has the Smash-like layout: south = attack, east = special,
  west/north = jump, shoulders/triggers = block.

## Proof of concept (done)

- **Stage: scene 8**, a one-screen room 384 px wide. The screen shows map x 32–351.
  - Page 2 is a steel floor (top y = 208) with a catwalk over it (x 96–288 on the map, top
    y = 112), and page 1 is the brick room behind.
  - The floor's outer two block columns are blanked, so the floor runs from screen x 32 to 288
    and has edges.
  - The catwalk is a pass-through platform: jump up through it, press Down to drop through.
  - The floor is solid: its sides are walls below its top.
  - Blast zones are 64 px past the sides and bottom, and 96 px above.
- **Characters** (`notes/characters.md`), assigned in turn to the slots. The frame ranges are in the
  `CHARS` table:

  | | moves | notes |
  |---|---|---|
  | Batman | 3-hit jab combo (anim 0–2 of the table at 0x5744: frames 42–59), flying kick in the air (30–34) | ROM attack boxes |
  | Joker | mallet smash (23–29), jump kick (4–6) | ROM attack boxes |
  | Bane | shoulder lunge (31–34) on the ground and in the air | the platform drawn into his frames is left out; the boxes are given in `versus.c`; heavy (weight 140) |
  | Firefly | flamethrower (10–13), flying kick (52–55), 3 air jumps | the boxes are given in `versus.c`; light (85) |
  | Penguin | umbrella poke (21–25), umbrella spin in the air (16–20) | ROM attack boxes; small, waddles (68–77) |
  | Mr. Freeze | arm thrust (5–11), freeze breath upwards in the air (21–26) | the boxes are given in `versus.c`; slow, heavy (125). His frames have no walk or jump |
  | Robin | Batman's moves on `mods/robin-p2`'s frames (upper ROM copy, bank 0x251C40) | only offered with `--mods robin-p2`; faster and lighter than Batman |

  The roster order is Batman, Joker, Bane, Firefly, Penguin, Mr. Freeze, then Robin when his mod is
  loaded (`versus_roster`). `versus_set_char` changes a slot's character before the fight.

- **Physics:**
  - walking;
  - a jump plus air jumps;
  - air drift, gravity, terminal speed, a fast fall (Down while falling).
- **Hits:**
  - each tick, the current frame's attack boxes are tested against the other fighters' body
    boxes. A frame without body boxes uses the character's default;
  - a hit adds the move's damage, then launches at its angle with speed
    `bkb + kbg * percent / weight` and hitstun `6 + speed / 48`, after a short hitlag on both;
  - an attack hits each fighter once.
- **Stocks:** 3 each by default (`--stocks N`).
  - After a KO the fighter is gone for 1 s, then hovers at the top centre, invulnerable, until
    a button or 2 s pass.
  - The last fighter with stocks wins.
  - A human's B, A or Menu after 4 s starts a rematch. CPU-only matches restart on their own.
- **CPUs:** every slot without a human:
  - they go for the nearest fighter and attack in reach;
  - they jump after fighters above them and drop off the catwalk after ones below;
  - when off the stage they head back to the middle and jump.
- **Character select** (roadmap 3), then the match.
- **HUD:** name, percent, stocks and CPU along the top, a P*n* tag over each fighter, a countdown,
  and the result. It is drawn with `osd.c` into the framebuffer after the PPU, so it isn't
  hardware-like yet.
- **Up to 8 fighters.** Sprite palette banks 8–15 are one per slot. Characters repeat with the
  same look until costumes exist (roadmap item 2).

## The title menu item (`recomp/src/vsmenu.c`)

- **How the game draws the menu:** `0x0098CF`.
  - `0x0098A4(table, n)` draws n records of (string, x, y) from a table, with x and y in 8 px
    cells, through `0x07E0D3`.
  - The table at ROM 0x4009 holds New Game (16, 19), Continue (16, 21), "Press A" (13, 26) and
    "No saved games" (13, 26).
  - It draws the first two, then "Press A". It shows "No saved games" instead when the cursor
    is on Continue and `0x0DD290` finds no save.
  - `0x009841` then draws the highlight box round item [cursor].
- **Input:** the menu's state machine, around `0x00A126`.
  - The cursor is the word at [bp]. Up moves it towards 0 and Down towards 1 (the limit is the
    compare at 0x00A146), with sound 0x3F, or sound 7 at an end.
  - A (at 0x00A15E) on 0 sets the menu state to 5 (difficulty). On 1 it sets 4 (save slots)
    if a save exists.
- **The change:**
  - a 5-record table at ROM 0xFC00 (free: segment 0 ends in zeros from 0xFBFF, and only 0xFBFF
    and the vectors at 0xFFF7–0xFFF8 are ever read), with Versus as item 2 at (16, 23);
  - the draw code points at it and draws 3 items, and Down goes on to 2;
  - a hook skips the save check on Versus, so "Press A" shows;
  - a hook on A over Versus sets `m->versus_request` and leaves the menu alone.
- **Front end:** the SDL front end snapshots the machine and opens the versus select screen.
  Special on that screen (C, or the east button) restores the snapshot, back at the title menu
  with the cursor on Versus.
- **When it is on:** only in native play. The patch goes into the ROM image after its content id
  is taken, so save states still load. Recordings and netplay leave the menu as it was, as does
  `--no-versus-menu`. `batman-rt --versus-menu` tests it.

## Stages

- **Data** (`STAGES` in `versus.c`): a scene, where the camera may go, blast zones, spawn and
  respawn points, platforms, page-2 blocks to take out, and the palette banks the scene leaves
  free (for projectiles).
  - Positions are map pixels of the scene.
  - A platform is pass-through or solid (a block 48 px deep, with walls at its sides and a
    ceiling), and each end may be a ledge.
- **The camera** follows the middle of the fighters in play, eased, inside its limits.
  - Each frame the 64×32-tile ring of both pages is refilled round it, the way the game builds
    columns (`notes/level-format.md`).
  - Page 1 scrolls at half speed and repeats its map.
  - Fighters off the screen get their tag at the screen's edge (`<P1`, `^P2`), since the PPU
    can't zoom out.
- **0 Catwalk** (scene 8): as before. The floor's ends are now ledges.
- **1 Clock Tower** (scene 3, *Clock's Ticking*): the top room of the tower with its side walls
  taken out, so page 1's skyline shows there.
  - Platforms: the floor (y 448, solid), a long stone platform (y 320), stone ledges (y 128) and
    wooden beams (y 224) on both sides, round the clock face.
  - Every outer end is a ledge.
  - 640 px wide, so the camera scrolls (x 320–640, y 0–240).
  - The floor-to-platform gap (128 px) is more than Batman's two jumps: the ledges and the
    grapnel are the way up.
- **Stage select:** on the select screen, P1's block cycles the stage. `--stage N` on both front
  ends.
- **Ledges.**
  - A falling fighter within 6–18 px outside a free ledge end, with feet 8–48 px below the top,
    catches it. One fighter per ledge.
  - The first catch since landing is intangible for 40 ticks. It gives back the jumps and ends
    helpless.
  - From the ledge: Up/Jump jumps; toward the stage (or waiting 5 s) climbs; B or X climbs with
    the jab; away or Down lets go (no catch again for 30 ticks).
  - A hit knocks the fighter off. CPUs climb after a moment.
- **The grapnel** (Batman's and Robin's X + up).
  - It fires at the nearest free ledge above and in front of the hand within 176 px. The line
    flies for 6 ticks (drawn into the frame, two pixels thick), then the fighter is pulled up it
    at 7 px a tick and hangs from that ledge.
  - With no ledge in reach it is the rising uppercut as before.
  - Off-stage CPUs use it as their recovery.
- **Checks:** `tests/inputs/versus-ledge.txt` (walk off the Catwalk's floor, catch the ledge,
  jump back up), `versus-grapnel.txt` (Clock Tower: the line, the hang at 376,356, the climb
  onto the platform), and a 4-CPU Clock Tower match that finishes.

## Moves

- **The scheme (Smash's), on the game's buttons:**
  - B: neutral jab; B + side, up, down: the tilts;
  - in the air: B is the neutral air, B + up and B + down the up and down airs;
  - X (special): neutral; X + side, up (the recovery), down.

  A move a character doesn't have falls back to its plain attack.
- **Jumpsquat.** Up is also jump (tap jump), so a ground jump leaves the ground 4 ticks after
  the press. B or X in those ticks does the up tilt or up special instead.
- **Moves are data** (`CHARS` in `versus.c`): ROM frames, the ROM's attack boxes (or one given
  here), damage, knockback, and angle (below 0 hits downwards: a meteor). They can also have:
  - a projectile and the frame it leaves on;
  - a speed held for the whole move (dashes, the glide);
  - a rise or a drop at the start;
  - low gravity;
  - helpless afterwards (a recovery: nothing but drifting, drawn darker, until landing).

  Landing ends a move started in the air.
- **Projectiles** (`PROJS`): frames of a bank entry, each in its own palette bank 2–7 (scene 8's
  background uses only 0 and 1).
  - A projectile flies, falls, or stays with its thrower. Thrown bombs go off where they land.
  - It hits once and vanishes; a column hits each fighter once. A shield blocks it.
  - Some are one at a time.

| | B side / up / down | airs (up, down) | X | X side | X up | X down |
|---|---|---|---|---|---|---|
| Batman | cape strike 60–66 / cape uppercut 84–89 / low sweep 92–96 | uppercut, spike 99–102 | Batarang (bank 0x0BD5) | glide kick 139–143 | rising uppercut | |
| Joker | mallet / mallet overhead / mallet low | overhead, mallet spike | gun (frame 74) | gas bomb (frame 73, an arc) | jump kick | |
| Bane | | | | shoulder charge | uppercut that flies him up | ground pound |
| Firefly | | | flame burst (27–32) | jet kick | jet up | flame column (34–38, downwards) |
| Penguin | | | bomb (89–96, an arc) | umbrella chain (to 128 px) | umbrella spin up | |
| Mr. Freeze | | | freeze breath | freeze shot (frozen 1 s, no damage) | ice: a hop, then a platform under him | |
| Robin | as Batman | as Batman | Batarang | dash kick | rising uppercut | |

- **Bane** is slow (walk 1.25 px a tick, attacks at 7–8 ticks a frame), hits for 12–16 %, is
  the heaviest (170), and takes 3/4 of a hit's damage (`armor`). His second jump is weak. His
  frames are still stand-ins (the stagger): new ones are to be drawn.
- **Mr. Freeze's ice:**
  - at the top of his X + up hop, a 64 px platform of his breath's cloud tiles appears under him;
  - only he can stand on it (and jump again from it); anyone else it touches takes 6 %;
  - it melts after a second, blinking at the end;
  - one per trip off the stage.
- **The freeze shot** (his breath's sparkle tiles) leaves its target frozen in place for 60
  ticks, drawn icy, with no damage. A hit breaks it, and a shield blocks it.
- **CPUs:**
  - they recover with X + up when off the stage with no jumps left;
  - they shoot neutral specials at fighters more than 70 px away;
  - they mix tilts and airs by where the target is.
- **Checks:** `tests/inputs/versus-specials.txt` (the Batarang 4 %, the glide kick 12 % in all,
  the recovery onto the catwalk) and `versus-freeze.txt` (frozen at 0 %, standing on the ice, the
  ice melting). `batman-rt --chars 5,1` picks characters by slot.

## Block

Block is a trigger or shoulder on a controller, V on the keyboard (Left Shift for `--kb2`'s P2).

- **The shield:** hold block on the ground.
  - It has 50 points. It drains 0.15 a tick while held (about 5.5 s from full) and comes back
    0.08 a tick when not held.
  - A hit on it does no percent. It costs 1.5× the move's damage in shield points, holds the
    shield for 4 + damage/2 ticks, and pushes the blocker back.
  - Shielding fighters are tinted blue-white, or red under a third.
- **Shield break:** the fighter pops up and is dizzy for 3 s, flashing yellow, and comes back
  with 3/8 of a shield.
- **From the shield:**
  - Left/Right roll 56 px, ending turned back;
  - Down spot-dodges;
  - Jump or Up jumps out.
- **In the air,** block is one air dodge per airtime.
- **Dodges** are intangible for ticks 3–15 and drawn see-through (the PPU's blend).
- **CPUs** shield attacks they see coming, and now and then roll away.
- **Checks:** `tests/inputs/versus-block.txt` (the mallet on a shield: 0 %, 7 points left
  after two hits, and the third breaks it) and `versus-roll.txt`.

## Save states and rewind

A match's whole state is its `versus_t`.
- F5/F8 save and load it, in `<save>.vs<slot>`.
- Backspace rewinds a frame per displayed frame, up to 60 s, from a ring of `versus_t`s.
- Neither touches the story game's own states.
- All of this is off online.

## Online (`recomp/src/vsnet.c`)

- **Run it:**
  - host: `batman target/batman.bin --versus --host [port]`;
  - guests: `batman target/batman.bin --versus --join <addr>[:port]`.

  Up to 8 machines, one player each. Each plays with the keyboard or a controller.
- **A star.** The host plays slot 1 and keeps the match.
  - A guest takes the first CPU slot. On the select screen it can also add a fighter.
  - The guests send their own inputs to the host, which relays every slot's inputs to each guest.
  - Packets repeat everything not yet acknowledged, so a lost packet costs nothing.
  - CPU slots need no network: the bots are part of the deterministic step.
- **Rollback.** Each machine runs ahead on predicted inputs (each slot's last one). When a real
  input differs, it goes back to that frame's snapshot (16 kept) and runs forward again.
- **Changes to the match.** A join, a leave or a desync makes the host send its state to every
  guest, and all go on from it.
  - A leaving player's slot goes back to a CPU.
  - Desyncs are found from a hash of a settled frame, once a second.
- **Wire.** It is separate from the co-op netplay (`net.c`), with its own magic and protocol
  number, so the two can't be mixed up. The defaults, the input delay (2) and the simulated
  network options are the same.
- **`tests/vsnet.sh <workdir> [frames] [lat,jitter,loss%]`:** a host and two guests on loopback,
  four fighters (one a CPU), random input, 40 ms ± 30 ms latency and 10% loss.
  - All three end in the same state.
  - The host's settled inputs (`--net-log`), replayed offline with the online CPU-slot mask
    (`--bots 4,5,6,7,8`), give that state too.
  - A guest knocked off on purpose is found and resynced.
  - Passed with about 85–130 rollbacks per machine over 900–1200 frames. Guests roll back further
    than the host: a guest's input reaches another guest through the host, so their latency is
    doubled.
- **Open:**
  - only tried on loopback;
  - the host's local second controller is ignored;
  - nothing pauses online.

## Checks (`tests/versus.sh`)

- A 4-CPU match run twice gives the same state hash, and so does one split by a save/load at
  frame 1777.
- `tests/inputs/versus-platform.txt`: P1 lands on the catwalk (y 112), then drops through to the
  floor (y 208).
- `tests/inputs/versus-combo.txt`: Batman's three hits do 3 + 3 + 6 %.
- An idle P1 against a CPU loses its 3 stocks, and the CPU wins with 3 KOs.
- A 4-CPU match finishes, over 20,000 frames.
- Story mode doesn't reach this code without `--versus`.

## Roadmap

1. ~~Proof of concept: one stage, 4 fighters, jab and aerial, pass-through platform, percent,
   basic knockback, stocks, CPUs.~~
2. **Characters:** ~~the Penguin, Mr. Freeze, Robin~~ (done).
   - **Bane** (new frames where the ROM has none):
     - a shoulder charge on X + side;
     - on X + up, a powerful uppercut that carries him up through the air;
     - balance: slower movement and slower attacks, but much more damage dealt and taken, and a
       weaker recovery when falling.
   - **Mr. Freeze:**
     - X + side shoots something that freezes the target for a second and does no damage;
     - X + up, if feasible, "draws ice": a short-lived ice platform that hurts the others but
       not him, and that he can jump off to recover.
   - **New characters:** Nightwing, Harley Quinn, Catwoman, Deathstroke, Killer Croc. None is in
     the ROM, so each needs frames made in the game's style (like `mods/robin`'s generator).
   - **Sound and music:** borrowed from the rest of the game. Still to do: find Bane's real
   fighting frames, and the enemies' own animation tables.
   - **Costumes** instead of palette swaps: slightly redrawn sprites of a character's other forms
     from other media, such as a Batman Beyond suit for Batman.
   - Build them like `mods/robin`'s generator: frames rebuilt into a copy of the character's frame
     slots in the upper ROM copy, in the game's art style.
   - Choose one per slot on character select. A repeated character gets the next costume by
     default.
3. **Menus.**
   - Done: the character select (`VS_SELECT`, the SDL front end's default). The fighters stand in
     their places; each human picks with Left/Right and confirms with B or A (X takes it back).
     P1's Up/Down add or remove CPU fighters (2–8), and the countdown starts when every human is
     ready. A rematch goes back to it. `batman-rt --select` and `tests/inputs/versus-select.txt`
     test it.
   - To do: choosing the CPUs' characters, a stage select, stocks, costumes, and a
     `batman versus` entry in the launcher (nixos-config, bump the pin).
4. **Versus from the story game's title menu** (done, `recomp/src/vsmenu.c`): a third item, "Versus",
   under "New Game" and "Continue" on the first menu. Below is the plan as it was; what was built is
   under "The title menu item". Picking it leaves the CPU's game and starts the versus mode
   (its menus, or a match). Finishing or quitting a match goes back to that title menu.
   - **Starting point.** The "New Game" string is at word 0x403C (`mods/example-text`).
   - **To find:** the menu's code, its item count, the cursor and the action for each item
     (`--watch` on the menu's RAM and the pad words while moving the cursor).
   - **The new item** needs its text, a cursor stop and a PC hook on its action. That hook hands
     control to `versus.c`, the same way co-op's hooks work.
   - **Coming back** restores a snapshot of the machine taken at the title menu.
   - **Unmodded runs.** Without the item, story mode must still match MAME, so the item could be
     on by default only in the native front end.
5. **Save states, rewind and netplay** for versus (done; see "Online" below). Still to do:
   several local players on one online machine, and spectators.
6. **Stage engine v2** (done; see "Stages" below). Still to do: more stages (scene 12's ledges and
   fire, the scrolling levels), moving platforms, hazards, zooming out (the PPU can't scale, so
   perhaps a second, smaller-tile view), and a grapnel for the others.
7. **Block** (done; see "Block" below). Still to do: grabs and throws, and shield tilting.
8. **Move sets** (done; see "Moves" below). Still to do: smash attacks (charged), back airs,
   grabs, and moves for the characters with few frames (Bane, Mr. Freeze).
9. **Blowback v2.** Started: the growth term is ×5 (`KB_GROWTH`), so that a strong hit KOs a
   middleweight from mid-stage at about 100–130 % on the Clock Tower. Still to do:
   - Smash's full knockback formula, per-move angles, DI, tumble, meteors, KO effects;
   - a balance table per character. Firefly's three air jumps and long flame win too often,
     and Bane's big body is hit too easily.
10. **Polish:**
   - sound: effects through the SPU from the ROM's samples, then music;
   - the HUD in PPU tiles (with the game's font and the lives heads);
   - CPU levels, a results screen;
   - a GBA port (the engine has no floats and no OS calls; drawing is the part to replace).

## Open

- Bane's frames have no attacks. His "lunge" is his stagger (31–34), with a box given in the
  code. His hurt pose is the electrocution (29–30).
- The blanked floor columns show the brick wall of page 1, not a pit. A real stage needs its own
  art there.
- The two side platforms the plan had don't fit scene 8's 32 px block grid next to the catwalk.
  They are left for stage v2.
- Joker's jab is the slow mallet. He has no quick jab in his frames.
- Knockback, damage and the CPUs are untuned.
