# Co-op: what the game does, and how two players fit in

Word addresses. Found with `batman-rt --trace` (call trees built from a one-frame trace taken
after a `--state-load`), `--watch` (with `fn:ADDR`, which logs only accesses made inside calls of
one function) and RAM dumps from session1.

## The frame loop (brawler, Street Party)

`main` loops at 0x0DC114:

| call | what |
|---|---|
| 0x005CDE | wait for vblank |
| 0x0DC745 | copy the sprite list to sprite RAM (0x0DC5EF) |
| 0x00C15F(1), 0x00C15F(0) | scroll the two background layers |
| 0x0DA03D | |
| 0x0DD77F | **read the controller**: [0x080A] = port A & 0xFFF0 (held), [0x080B] = held & ~previous (pressed) |
| 0x00F743 | |
| 0x07E8A5 | **update every object** |
| 0x07E804 (every 16th frame), 0x0DD16D, 0x0DA172, 0x00B6EA, 0x0DC64D, 0x00EEAB, 0x00F155 ... | |

## Objects

**The layout depends on the level.** In Street Party the player is at 0x017C and the HUD objects at
0x01C8 and 0x0214, with the pool from 0x0260. In the detective level *Search for a Smile* (Joker's
first mission) everything is one slot earlier: the player is at 0x0130, the HUD at 0x017C and 0x01C8.
The game points to them from 0x270A (player), 0x2708 (health/lives HUD) and 0x2705 (special HUD),
each to the object + 0x20. The addresses below are Street Party's.


- **The object list.** 0x07E8A5 walks a linked list from [[0x26FA] + 2].
  - Each node is 76 words.
  - Word 2 is the next node, and words 0–3 are list links.
  - Word 6 points to a far-pointer pair {segment, offset} in ROM, which is the update function.
  - The update is called with the node's address as its only argument. The call is made by
    pushing a return address, then `pop sr, pc` from that pair.
- **Street Party at vblank 4450:**

  | node | update | what |
  |---|---|---|
  | 0x0130 | 0x00E40A | |
  | 0x063C | 0x065913 | an enemy |
  | 0x017C | 0x008BF7 | **the player** |
  | 0x0688 | 0x0068BD | an enemy |
  | 0x00E4 | 0x0DA35B | |
  | 0x01C8 | 0x0DD3D6 | |
  | 0x0214 | 0x07FF39 | |
  | 0x0260 | 0x0DD2CB | |

## The player update, 0x008BF7

Writes measured over session1's Street Party (vblanks 3775–48600, `--watch w:0000-27ff,fn:008bf7`):

- **Its own node, 0x017C–0x01C7.**
  - Position at 0x019C: X as 16.16 (0x019C integer, 0x019D fraction), Y at 0x019E/0x019F.
  - 0x0191/0x0192 are copies made every frame by 0x007FF6. Enemies read these.
  - Animation is at 0x0184–0x018F.
- **The special meter**, 0x0244/0x0245.
- **Enemy nodes** (0x0260–0x07FB, 76-word slots): its attacks hit them.
- **The sprite buffer**, 0x20C3+ (4 words a sprite), with its pointer at 0x20A0. The player
  draws itself during its update.
- **Effect slots**, 0x2465–0x2543 (14 words each).
- **The camera targets** in the layer structs: 0x25EC (page 1, half speed) and 0x2606 (page 2),
  through 0x00C12F(layer, x).
  - These come from the **camera follow, 0x00C695(x, y, ...)**, which aims at x − 160 and eases.
  - The update calls it at 0x0090A1 with [0x019C], [0x019E]. It is also called from 0x0087B7
    and 0x008828.
- **Input:** [0x080A] (3425 writes, presses it consumes).
- **Scratch and stack:** 0x26E7–0x26FE, 0x2790+.

It doesn't write health (0x01E9) or lives (0x2099); enemies and the HUD objects do.

## Collisions and damage

- **The collision pass, 0x0DD16D.** It runs after the object loop and is skipped while 0x00F12F
  says the game is busy. For each object A in the update list it calls 0x0DD0AD(A).
- **How 0x0DD0AD tests a pair.** It walks the objects after A in a chain through word 0, sorted
  by word 4 (left x). For each pair (A, B):
  1. **Near:** stop if B is past A's width (word 0x19 − word 0x17).
  2. **Categories:** skip unless the words 0x1C (what it is) and 0x1D (what it reacts to)
     intersect.
  3. **Y:** words 0x16 + 0x18, within word 0x1A.
  4. **Box test:** 0x00B83D(A, B) on their frames' boxes.
  5. **Handlers:** A's handler (far pointer in word 7) as (A, B), then B's as (B, A), each only
     if it reacts.
- **The player's handler is 0x0090B5.** Being hit ends in 0x0DD458(−2), the health change:
  - it adds the change to [[0x2708] + 1] = 0x01E9, scaled by difficulty when negative;
  - below 0 it calls 0x0DD43C, which takes a life (0x2099) and sets 0x2706 = 0x0A (dying), or
    calls Game Over (0x07FAE1).
- **0x2706 is a game state:** 0x0A while the player is dying, then 4 (restart at the checkpoint,
  0x0DB81F), then 0.
- **0x0070D9 is the other direction:** an enemy taking damage from a player attack. It is called
  from the enemy's own update, and sets [[0x270A] + 0x28] = 0x01C4, the player's hit-confirm flag.
- **The HUD objects:**
  - 0x01C8 (update 0x0DD3D6, which draws the health bar and the lives heads; health is its word
    0x21 = 0x01E9);
  - 0x0214 (update 0x07FF39, the special meter; 0x0244 is its word 0x30).
- **The sprite list being built:** count at 0x20A0, entries of 4 words (tile, x, y, attr) from
  0x20A3. Coordinates are centred, and y is up.
- **Sprite palettes:** the table at RAM 0x2552 maps a palette id to a sprite attribute; 0xFFFF
  means bank 8 + (id & 3). The id is in the character header in the sprite bank, word 1:
  Batman 3 → 0x0E51 (bank 14). Bank 9 is unused in Street Party.
- **Character header:** word 1 of the player's header (index [0x018B] = 8 → bank offset 0x2EFF) is
  the palette id. The frame list follows at +0x0C. Drawing (0x00BC8A) reads through 0x0DC492 with
  the bank base 05:1C40 in [0x26EE:0x26EF].

## Doors (detective levels)

- A door is an object (update 0x065229) whose collision handler, 0x065314(door, player), does two
  things:
  - it **opens** the door on X held (0x080A & 0x0100) while the player is in its box, on the ground
    ([+0x22] = [+0x29]) and [+0x36] = 0;
  - **once it is open** ([0x2709] = 1) it starts the walk on the door's direction, door word 0x21:
    0 = Up pressed, 1 = Right held, 2 = Down pressed, 3 = Left held (0x080B/0x080A). The walk is
    0x07E98E(player, 0x5D80): the player's word 6 becomes 0x5D80 → 0x06528A, the door walk.
- The walk (about 13 frames in *Search for a Smile*) moves the player into the next room (0x0DB51D,
  0x00C63E) and sets word 6 back to 0x5D82 → 0x008BF7.
- Both read the buttons from the global pad words, not from the player.

## Searching (detective levels)

- A searchable object (the desk in Cell Block A) has word 6 = 0x5DA1 → update 0x07F37C at all times,
  and handler 0x5D9D → 0x07F4DF.
- **The handler:**
  - it sets the touching player's word 0x41 to 9 (the prompt, "A LARGE DESK"; the player update
    clears 0x41 every frame, and doors set it too);
  - on X pressed (0x080B) it starts the search.
- **The update** reads X held (0x080A, at 0x07F52E). It sets word 0x2F nonzero (9) while searching,
  with a progress bar ("SEARCHING THE DESK") and a sound (0x53).
- When the bar is full, the object goes away and the message shows with [0x2706] = 5 (busy). A
  button closes it, and [0x2706] goes back to 0.


- **Finding the objects.** The player is the object at [0x270A] − 0x20, and only in a level where
  [0x2708] − 0x20 runs the brawler HUD (0x0DD3D6) and [0x2705] − 0x20 the special meter (0x07FF39).
  Health is the HUD's word 0x21 and special its words 0x30–0x31. Elsewhere (menus, Fast Lane, whose
  HUD is 0x0DD610 and which has no player pointer) co-op does nothing. A pass of the object loop
  (0x07E8A5) without the player ends P2's level. Pausing keeps the player in the loop.
  - The first version took the objects by their Street Party addresses. In the detective level it
    redirected the return of the object at 0x0214 (a level object, 0x0DB492, there) and crashed.

All of it runs from PC hooks, with trampolines at 0x3FFF00+ in the upper ROM copy, which the game
never runs. All of the state is in `spg_t.coop` and in save states.

- **The object loop's dispatch (0x07E8BA)** sees every update call:
  - **The player:** P1's call returns to a trampoline. There P2's words go into RAM (0x0180–0x01C7,
    0x01E9, 0x0244/0x0245) with P2's buttons in 0x080A/0x080B. P2's own update (its word 6) is then
    called, and P1's words come back afterwards.
  - **Pool objects:** each targets the nearer player, with 32 px of hysteresis. For P2, P2's words
    are in RAM during that object's update.
  - **The HUD objects** run again with P2's words and lives, and their new sprites move to the
    bottom of the screen.
- **Collisions.** After P1's collision pass, P2's words go into RAM and every object is tested
  against the player as 0x0DD0AD tests a pair. The chain links are not touched: a first version
  that patched them hung the game when a handler removed an object.
  - **The chain is sorted by x (word 4), and the player's update keeps it sorted** (0x07E686: word 0
    next, word 1 previous, sentinels at the ends). P2's update ran it with P2's x, so P1's node sat
    where P2 was and the game's pass missed what was next to P1: P1 couldn't open the door it stood
    at in Cell Block A, or touch the locked door. After P2's update the node goes back to P1's place.
  - **P2's pass is skipped while the game is busy** (paused or showing a message, 0x00F12F: [0x2706]
    1 or 5), as the game's own pass is, and stops when a handler makes it busy. A door's handler runs
    for each player touching it. When P1 held X at the locked door, its handler opened the message
    (0x0DA5BB mode 3, [0x2706] = 5); P2's run of the same handler then put the door's label in its
    place (0x0DA5BB mode 0). The game stayed busy with no message to close, so both players were
    stuck while the music played. While a message is up, [[0x2675]] is its text id (0xFFFF: none).
- **Deaths.** 0x0DD43C with P2's words in RAM takes one of P2's lives: P2 is out for 3 s, then
  comes back beside P1 with full health, or stays out with none left. P1's deaths are the game's,
  and the checkpoint restart brings P2 along.
- **Doors.** P2's collision pass has P2's buttons in 0x080A/0x080B, so P2 can open a door and walk in.
  Whoever starts the walk, P1 walks through: if P2's word 6 becomes 0x5D80, P1 takes over P2's object
  words (keeping P1's own health and special). P2 is out of play during the walk and rejoins on P1's spot
  (in front of the door; an offset could put P2 off the map), with P1's state from the new room and P2's own health and special.
- **Searching: one search at a time.**
  - **Who searches.** The searcher is the player the searchable object follows when its word 0x2F
    turns on, since its update reads that player's buttons. Pool objects that follow P2 run with
    P2's buttons in the pad words.
  - **While it runs:**
    - every searchable object follows the searcher;
    - searchable objects are hidden from the other player's collision pass (categories zeroed
      around it), so the other player can't cancel the search or start a second one;
    - otherwise the other player moves and fights.
  - **During the message,** the other player gets no buttons, and only the searcher's buttons
    close it. If the searcher is P2, the game's pad words get P2's buttons outside P1's update.
- **Spawning.** P2 starts a level and respawns on P1's spot. An offset could put P2 off the map.
- **Camera and leash.** The camera follow (0x00C695) aims at the midpoint; P2's call returns at
  once. The leash keeps the players at most 272 px apart.
- **Look.** While P2's words are in RAM, sprite-bank reads (0x0DC492 at 05:1C40) go to 25:1C40,
  the upper copy, where `mods/robin-p2` puts Robin's frames. His tiles are at tile 0xC000+.
  Palette id 3 draws with bank 9, filled from the upper copy's palette every frame.

## Open

- **P1 dying while P2 is alive** still restarts both at the checkpoint; the plan was for P1 to
  respawn in place. P1 out of lives is Game Over even if P2 has lives.
- **P2's HUD** shows Batman's lives heads.
- **P2 dies at once:** no knockdown animation, and no blinking on respawn.
- **Robin's melee range and Birdarang:** these are code patches, so P2 has Batman's.
- **Doors** are checked in the first rooms of *Search for a Smile* (Cell Block A, the locked door
  and its message). Rooms entered in other ways (Street Party's sections, scripted scene changes)
  haven't been checked with P2.
- **Only P1's buttons close a message P2 didn't search for** (a locked door's): the message object
  (0x00E4, update 0x0DA35B) reads the game's pad words, which hold P1's.
- **"P1 couldn't attack sometimes"** (reported from online play) is not reproduced. In Street Party
  B starts an attack as often in co-op as alone.
- **Pool objects** run with P2's words in RAM when they target P2, including non-enemies (level
  objects such as 0x0DB492). Harmless so far, but a level object that moves the player would move P2.
- **Fast Lane (driving):** P2 is not there yet.
- **Netplay** works (`notes/netplay.md`), on loopback and once over the VPN.
