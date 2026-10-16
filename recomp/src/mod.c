/* Runtime mods: see mod.h and mod_api.h. */
#include "mod.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_api.h"
#include "osd.h"
#include "rom.h"
#include "sha256.h"
#include "state.h"

#define MAX_MODS 32
#define MAX_POKES 256
#define MAX_HOOKS 256
#define MAX_FRAME_FNS 64

static struct {
    char dir[1024];
    char name[128];
} mods[MAX_MODS];
static int n_mods;

static struct { uint32_t addr; uint16_t value; int always; } pokes[MAX_POKES];
static int n_pokes;

static struct { uint32_t addr; batman_hook_fn fn; void *user; } hooks[MAX_HOOKS];
static int n_hooks;
static uint8_t *hook_map;

static struct { batman_frame_fn fn; void *user; } frame_fns[MAX_FRAME_FNS];
static int n_frame_fns;

static sha256_t manifest_hash; /* every manifest's text, in load order */
static int loading_mod;        /* the mod whose plugin is being initialised */

int mods_count(void) { return n_mods; }

/* ---- plugin API ----------------------------------------------------------------------- */

static uint16_t api_peek(spg_t *m, uint32_t addr) { return spg_peek(m, addr); }

static void api_poke(spg_t *m, uint32_t addr, uint16_t v)
{
    addr &= 0x3fffff;
    if (addr < 0x2800) m->ram[addr] = v;
    else if (addr >= 0x2900 && addr < 0x3000) m->vram[addr - 0x2900] = v;
    else fprintf(stderr, "mod: poke %06x ignored (only RAM and video RAM)\n", addr);
}

static void api_bus_write(spg_t *m, uint32_t addr, uint16_t v) { m->cpu.write(m->cpu.ctx, addr, v); }
static uint16_t *api_regs(spg_t *m) { return m->cpu.r; }
static uint32_t api_pc(spg_t *m) { return unsp_lpc(&m->cpu); }
static uint32_t api_frame(spg_t *m) { return m->frame; }
static uint32_t *api_framebuffer(spg_t *m) { return m->framebuffer; }
static void api_text(spg_t *m, int x, int y, const char *s) { osd_text(m->framebuffer, PPU_W, PPU_H, x, y, 1, s); }

static void api_retf(spg_t *m)
{
    uint16_t *r = m->cpu.r;
    r[REG_SR] = m->cpu.read(m->cpu.ctx, ++r[REG_SP]);
    r[REG_PC] = m->cpu.read(m->cpu.ctx, ++r[REG_SP]);
    m->cpu.cycles += 8;
}

static void api_hook(uint32_t addr, batman_hook_fn fn, void *user)
{
    addr &= 0x3fffff;
    if (n_hooks == MAX_HOOKS) { fprintf(stderr, "mod: too many hooks\n"); return; }
    if (!hook_map) hook_map = calloc(ROM_SPACE_WORDS / 8, 1);
    hooks[n_hooks].addr = addr;
    hooks[n_hooks].fn = fn;
    hooks[n_hooks++].user = user;
    hook_map[addr >> 3] |= (uint8_t)(1 << (addr & 7));
}

static void api_on_frame(batman_frame_fn fn, void *user)
{
    if (n_frame_fns == MAX_FRAME_FNS) { fprintf(stderr, "mod: too many frame callbacks\n"); return; }
    frame_fns[n_frame_fns].fn = fn;
    frame_fns[n_frame_fns++].user = user;
}

static void api_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", mods[loading_mod].name);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int dispatch_hook(spg_t *m, uint32_t pc)
{
    int moved = 0;
    for (int i = 0; i < n_hooks && !moved; i++)
        if (hooks[i].addr == pc) moved = hooks[i].fn(m, hooks[i].user);
    return moved;
}

/* ---- manifests ------------------------------------------------------------------------ */

static int parse_num(const char *s, uint32_t *out)
{
    char *end;
    const unsigned long v = strtoul(s, &end, 16);
    if (!*s || *end) return 0;
    *out = (uint32_t)v;
    return 1;
}

typedef struct {
    uint16_t *rom;
    uint8_t *owner; /* per ROM word: index + 1 of the mod that last wrote it */
    int overlaps;
} patch_ctx_t;

static int put_word(patch_ctx_t *pc, int mod, uint32_t addr, uint16_t v)
{
    if (addr >= ROM_SPACE_WORDS) {
        fprintf(stderr, "%s: ROM address %06x out of range\n", mods[mod].name, addr);
        return 0;
    }
    if (pc->owner[addr] && pc->owner[addr] != mod + 1 && pc->overlaps++ < 8)
        fprintf(stderr, "mods: %s overwrites %s at %06x\n", mods[mod].name, mods[pc->owner[addr] - 1].name, addr);
    pc->owner[addr] = (uint8_t)(mod + 1);
    pc->rom[addr] = v;
    return 1;
}

static int load_manifest(int mod, patch_ctx_t *pc, const uint8_t rom_sha[32], const batman_api_t *api_tmpl)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s/mod.txt", mods[mod].dir);
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 0; }
    char line[4096];
    int lineno = 0, have_rom = 0, ok = 1;
    while (ok && fgets(line, sizeof(line), f)) {
        lineno++;
        sha256_update(&manifest_hash, line, strlen(line));
        /* '#' starts a comment, except inside a quoted string */
        int inq = 0;
        for (char *h = line; *h; h++) {
            if (*h == '"') inq = !inq;
            else if (*h == '#' && !inq) { *h = 0; break; }
        }
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p) continue;
        char *key = strsep(&p, " \t\r\n");
        while (p && isspace((unsigned char)*p)) p++;
        char *rest = p ? p : "";
        rest[strcspn(rest, "\r\n")] = 0;
        char tok[8][256];
        int nt = 0;
        { char tmp[4096]; snprintf(tmp, sizeof(tmp), "%s", rest); char *t = tmp, *w;
          while (nt < 8 && (w = strsep(&t, " \t"))) if (*w) snprintf(tok[nt++], sizeof(tok[0]), "%s", w); }
        uint32_t addr = 0, val;

        if (!strcmp(key, "name")) {
            snprintf(mods[mod].name, sizeof(mods[mod].name), "%s", rest);
        } else if (!strcmp(key, "rom")) {
            char want[65];
            sha256_hex(rom_sha, want);
            if (strcasecmp(tok[0], want)) {
                fprintf(stderr, "%s: made for ROM %s, not this one (%s)\n", path, tok[0], want);
                ok = 0;
            }
            have_rom = 1;
        } else if (!strcmp(key, "words") && nt >= 2 && parse_num(tok[0], &addr)) {
            for (int i = 1; i < nt && ok; i++) {
                if (!parse_num(tok[i], &val) || val > 0xffff) { fprintf(stderr, "%s:%d: bad word %s\n", path, lineno, tok[i]); ok = 0; }
                else ok = put_word(pc, mod, addr + i - 1, (uint16_t)val);
            }
        } else if (!strcmp(key, "text") && nt >= 2 && parse_num(tok[0], &addr)) {
            char *q0 = strchr(rest, '"'), *q1 = q0 ? strrchr(q0 + 1, '"') : NULL;
            if (!q1) { fprintf(stderr, "%s:%d: text needs a quoted string\n", path, lineno); ok = 0; }
            for (char *c = q0 + 1; ok && c < q1; c++) ok = put_word(pc, mod, addr++, (unsigned char)*c);
        } else if (!strcmp(key, "patch") && nt == 2 && parse_num(tok[0], &addr)) {
            char fp[1400];
            snprintf(fp, sizeof(fp), "%s/%s", mods[mod].dir, tok[1]);
            FILE *pf = fopen(fp, "rb");
            if (!pf) { perror(fp); ok = 0; }
            int lo, hi;
            while (ok && (lo = fgetc(pf)) != EOF && (hi = fgetc(pf)) != EOF) ok = put_word(pc, mod, addr++, (uint16_t)(lo | hi << 8));
            if (pf) fclose(pf);
        } else if (!strcmp(key, "poke") && nt >= 2 && parse_num(tok[0], &addr) && parse_num(tok[1], &val)) {
            if (addr >= 0x2800 || val > 0xffff) { fprintf(stderr, "%s:%d: poke must be RAM (< 2800) and a 16-bit value\n", path, lineno); ok = 0; }
            else if (n_pokes == MAX_POKES) { fprintf(stderr, "%s:%d: too many pokes\n", path, lineno); ok = 0; }
            else {
                pokes[n_pokes].addr = addr;
                pokes[n_pokes].value = (uint16_t)val;
                pokes[n_pokes++].always = nt >= 3 && !strcmp(tok[2], "always");
            }
        } else if (!strcmp(key, "plugin") && nt == 1) {
            char fp[1400];
            snprintf(fp, sizeof(fp), "%s/%s", mods[mod].dir, tok[0]);
            void *so = dlopen(fp, RTLD_NOW | RTLD_LOCAL);
            int (*init)(const batman_api_t *, void **) = so ? (int (*)(const batman_api_t *, void **))dlsym(so, "batman_mod_init") : NULL;
            if (!init) { fprintf(stderr, "%s: %s\n", fp, so ? "no batman_mod_init" : dlerror()); ok = 0; }
            else {
                /* plugins are identified by their bytes too */
                FILE *sf = fopen(fp, "rb");
                char buf[65536];
                size_t n;
                while (sf && (n = fread(buf, 1, sizeof(buf), sf))) sha256_update(&manifest_hash, buf, n);
                if (sf) fclose(sf);
                batman_api_t *api = malloc(sizeof(*api));
                *api = *api_tmpl;
                api->mod_dir = mods[mod].dir;
                void *user = NULL;
                loading_mod = mod;
                if (init(api, &user)) { fprintf(stderr, "%s: init failed\n", fp); ok = 0; }
            }
        } else {
            fprintf(stderr, "%s:%d: can't parse \"%s %s\"\n", path, lineno, key, rest);
            ok = 0;
        }
    }
    fclose(f);
    if (ok && !have_rom) { fprintf(stderr, "%s: missing the rom <sha256> line\n", path); ok = 0; }
    return ok;
}

int mods_enabled_list(const char *mods_dir, char *out, size_t outlen)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s/enabled.txt", mods_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    out[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        p[strcspn(p, " \t\r\n#")] = 0;
        if (!*p || *p == '#') continue;
        if (out[0]) strncat(out, ",", outlen - strlen(out) - 1);
        strncat(out, p, outlen - strlen(out) - 1);
    }
    fclose(f);
    return out[0] != 0;
}

int mods_load(const char *list, const char *mods_dir, uint16_t *rom, const uint8_t rom_file_sha256[32])
{
    static const batman_api_t api_tmpl = {
        .version = BATMAN_MOD_API_VERSION,
        .peek = api_peek, .poke = api_poke, .bus_write = api_bus_write,
        .regs = api_regs, .pc = api_pc, .retf = api_retf,
        .hook = api_hook, .on_frame = api_on_frame,
        .frame = api_frame, .framebuffer = api_framebuffer, .text = api_text, .log = api_log,
    };
    sha256_init(&manifest_hash);
    patch_ctx_t pc = { rom, calloc(ROM_SPACE_WORDS, 1), 0 };
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", list);
    char *p = buf, *name;
    int ok = 1;
    while (ok && (name = strsep(&p, ","))) {
        if (!*name) continue;
        if (n_mods == MAX_MODS) { fprintf(stderr, "mods: too many\n"); ok = 0; break; }
        if (strchr(name, '/')) snprintf(mods[n_mods].dir, sizeof(mods[0].dir), "%s", name);
        else snprintf(mods[n_mods].dir, sizeof(mods[0].dir), "%s/%s", mods_dir, name);
        const char *base = strrchr(mods[n_mods].dir, '/');
        snprintf(mods[n_mods].name, sizeof(mods[0].name), "%s", base ? base + 1 : mods[n_mods].dir);
        ok = load_manifest(n_mods, &pc, rom_file_sha256, &api_tmpl);
        if (ok) fprintf(stderr, "mod: %s\n", mods[n_mods].name);
        n_mods++;
    }
    if (pc.overlaps > 8) fprintf(stderr, "mods: %d overlapping words in all\n", pc.overlaps);
    free(pc.owner);
    return ok;
}

void mods_content_id(const uint16_t *rom, uint8_t out[32])
{
    spg_rom_hash(rom, out);
    if (!n_mods) return;
    sha256_t s = manifest_hash; /* copy: the running hash stays usable */
    uint8_t man[32];
    sha256_final(&s, man);
    sha256_t c;
    sha256_init(&c);
    sha256_update(&c, out, 32);
    sha256_update(&c, man, 32);
    sha256_final(&c, out);
}

void mods_attach(spg_t *m)
{
    if (!n_hooks) return;
    m->hook_map = hook_map;
    m->pc_hook = dispatch_hook;
}

void mods_reset(spg_t *m)
{
    for (int i = 0; i < n_pokes; i++) m->ram[pokes[i].addr] = pokes[i].value;
}

void mods_frame(spg_t *m)
{
    for (int i = 0; i < n_pokes; i++)
        if (pokes[i].always) m->ram[pokes[i].addr] = pokes[i].value;
    for (int i = 0; i < n_frame_fns; i++) frame_fns[i].fn(m, frame_fns[i].user);
}
