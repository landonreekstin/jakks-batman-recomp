/* "Versus" as a third item on the title menu, under New Game and Continue (vsmenu.h).
 *
 * The title menu (notes/versus.md):
 *   - 0x0098CF draws it. 0x0098A4(table, n) draws n items of a table of (string, x, y) records
 *     (x and y in 8 px cells) through 0x07E0D3. The table at 0x4009 holds New Game, Continue,
 *     "Press A" and "No saved games"; the first two are drawn, then "Press A", or "No saved games"
 *     when the cursor is on Continue and 0x0DD290 finds no saved game. Then 0x009841 draws the
 *     highlight box round item [cursor] of the same table.
 *   - The input, in the menu's state machine around 0x00A126: the cursor is the word at [bp].
 *     Up takes it down to 0, Down up to 0 + 1 (the compare at 0x00A146), with sound 0x3F, or
 *     sound 7 at the ends. A (0x00A15E) on 0 starts a new game (state 5), on 1 continues (state 4)
 *     if there is a saved game.
 *
 * The change: a new table at 0xFC00 (free ROM at the end of segment 0, never read; the strings
 * must be under 0x10000) with Versus as item 2 at cell (16, 23), "Press A" and "No saved games"
 * moved to 3 and 4; the draw code points at it and draws 3 items; Down goes up to 2. Two hooks:
 * the save check is skipped on Versus (so it says "Press A"), and A on Versus asks the front end
 * to start the versus mode instead of anything the game would do. */
#include "vsmenu.h"

#include "mod.h"
#include "spg.h"

#define TABLE 0xFC00
#define TEXT 0xFC10
#define OLD_TABLE 0x4009
#define SAVE_CHECK 0x0098E3  /* call 0dd290, with r1 = the cursor */
#define SAVE_SKIP 0x0098E9   /* where "Press A" is drawn */
#define A_PRESSED 0x00A166   /* A pressed on the title menu: r1 = [bp], the cursor */
#define MENU_DONE 0x00A473   /* the state machine's common exit */
#define ITEM_VERSUS 2

void vsmenu_patch(uint16_t *rom)
{
    static const uint16_t table[15] = {
        0x403C, 0x10, 0x13,   /* New Game */
        0x4033, 0x10, 0x15,   /* Continue */
        TEXT,   0x10, 0x17,   /* Versus */
        0x4024, 0x0D, 0x1A,   /* "Press A" */
        0x4015, 0x0D, 0x1A,   /* "No saved games" */
    };
    static const char text[] = "Versus";
    if (rom[OLD_TABLE] != 0x403C || rom[0x98D8] != OLD_TABLE) return;   /* not this ROM's menu */
    for (int i = 0; i < 15; i++) rom[TABLE + i] = table[i];
    for (int i = 0; i < (int)sizeof(text); i++) rom[TEXT + i] = (uint16_t)(unsigned char)text[i];
    rom[0x98D5] = 0x9443;   /* r2 = 03: draw three items */
    rom[0x98D8] = TABLE;
    rom[0x98EC] = TABLE;
    rom[0x98EE] = 9;        /* "Press A" is item 3 */
    rom[0x98F4] = TABLE;
    rom[0x98F6] = 12;       /* "No saved games" is item 4 */
    rom[0x9901] = TABLE;    /* the highlight box */
    rom[0xA146] = 0x4241;   /* cmp r1, 01: Down goes on to item 2 */
}

static int on_save_check(spg_t *m, void *user)
{
    (void)user;
    if (m->cpu.r[REG_R1] != ITEM_VERSUS) return 0;
    m->cpu.r[REG_PC] = SAVE_SKIP & 0xFFFF;
    return 1;
}

static int on_a_pressed(spg_t *m, void *user)
{
    (void)user;
    if (m->ram[m->cpu.r[REG_BP]] != ITEM_VERSUS) return 0;
    m->versus_request = 1;
    m->cpu.r[REG_PC] = MENU_DONE & 0xFFFF;
    return 1;
}

void vsmenu_attach(spg_t *m)
{
    (void)m;
    mods_hook(SAVE_CHECK, on_save_check, NULL);
    mods_hook(A_PRESSED, on_a_pressed, NULL);
}
