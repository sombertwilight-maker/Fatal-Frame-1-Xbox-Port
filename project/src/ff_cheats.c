/* ff_cheats.c -- raw memory cheats, read from fatalframe_cheats.ini beside
 * the exe and toggled from the CHEATS tab of the F6 menu.
 *
 * Why not Action Replay codes as printed: a Datel Xbox code is an encrypted
 * pair of 32-bit words plus a per-game mastercode, and the mastercode exists
 * to tell the cartridge which instruction fetch to trap so it can inject
 * code at a safe moment. A static recompile has no instruction fetches to
 * trap -- the guest's bytes are never executed, they were translated to C --
 * so the mastercode has nothing to hook and any code that patches
 * instructions is inert. What does carry over is the half that matters:
 * writing a value into guest RAM at a guest address. Our guest RAM sits at
 * the same virtual addresses the Xbox used, so a raw "address = value" poke
 * behaves exactly as it did on hardware.
 *
 * File format: a [name] per cheat, then one write per line,
 *
 *     [Infinite film]
 *     b 0017A2B4 63          ; 8-bit:  write 0x63
 *     w 0017A2B8 270F        ; 16-bit
 *     d 0017A2BC 0000FFFF    ; 32-bit
 *
 * '#' or ';' starts a comment. A cheat that is On has every one of its
 * writes applied each frame, so the game cannot tick the value back down. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define MAX_CHEATS 24
#define MAX_WRITES 32
#define RAM_LO 0x00010000u
#define RAM_HI 0x04000000u

extern void *xbox_GetMemoryOffset(void);

typedef struct { uint32_t va, val; int size; } Write;
typedef struct {
    char   name[40];
    Write  w[MAX_WRITES];
    int    nw;
    int    on;
} Cheat;

static Cheat s_cheat[MAX_CHEATS];
static int   s_ncheat;
static int   s_loaded;

static void add_write(Cheat *c, int size, uint32_t va, uint32_t val)
{
    if (c->nw >= MAX_WRITES)
        return;
    if (va < RAM_LO || va >= RAM_HI)
        return;
    c->w[c->nw].size = size;
    c->w[c->nw].va = va;
    c->w[c->nw].val = val;
    c->nw++;
}

static void load(const char *dir)
{
    char path[MAX_PATH], line[256];
    FILE *f;
    Cheat *cur = NULL;

    s_loaded = 1;
    snprintf(path, sizeof path, "%s\\fatalframe_cheats.ini", dir ? dir : ".");
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[CHEAT] no fatalframe_cheats.ini (looked in %s)\n", path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *p = line, *e;
        while (*p == ' ' || *p == '\t') p++;
        e = p + strcspn(p, "#;\r\n");
        while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;
        if (!*p)
            continue;
        if (*p == '[') {
            char *close = strchr(p, ']');
            if (!close || s_ncheat >= MAX_CHEATS) {
                cur = NULL;
                continue;
            }
            *close = 0;
            cur = &s_cheat[s_ncheat++];
            memset(cur, 0, sizeof *cur);
            snprintf(cur->name, sizeof cur->name, "%s", p + 1);
            continue;
        }
        if (cur) {
            char sz = (char)(*p | 0x20);
            unsigned long va = 0, val = 0;
            int size = sz == 'b' ? 8 : sz == 'w' ? 16 : sz == 'd' ? 32 : 0;
            if (!size) {
                fprintf(stderr, "[CHEAT] %s: not b/w/d: %s\n", cur->name, p);
                continue;
            }
            if (sscanf(p + 1, "%lx %lx", &va, &val) == 2)
                add_write(cur, size, (uint32_t)va, (uint32_t)val);
            else
                fprintf(stderr, "[CHEAT] %s: cannot read: %s\n", cur->name, p);
        }
    }
    fclose(f);
    fprintf(stderr, "[CHEAT] %d cheat%s from fatalframe_cheats.ini\n",
            s_ncheat, s_ncheat == 1 ? "" : "s");
}

/* Cheat Engine attaches to fatalframe.exe and shows HOST addresses. Guest
 * RAM is one mapped window, so host = guest + offset, and the offset is the
 * only thing between a Cheat Engine find and a line in the cheat file. The
 * window is placed from a list of candidate bases, so the offset is NOT a
 * constant across machines -- it is printed here, and RECOMP_CE does the
 * subtraction so nobody has to. */
static void report_offset(void)
{
    uintptr_t off = (uintptr_t)xbox_GetMemoryOffset();
    const char *ce = getenv("RECOMP_CE");

    /* %I is an MSVC extension that mingw's ANSI stdio does not implement:
     * it swallowed the whole line and printed nothing at all. */
    fprintf(stderr, "[CHEAT] guest RAM is at host +0x%llX.\n",
            (unsigned long long)off);
    fprintf(stderr, "[CHEAT]   ini address = Cheat Engine address - 0x%llX\n",
            (unsigned long long)off);

    if (ce && *ce) {
        unsigned long long host = strtoull(ce, NULL, 16);
        if (host > (unsigned long long)off) {
            uint64_t va = (uint64_t)(host - off);
            if (va >= RAM_LO && va < RAM_HI) {
                const uint8_t *p = (const uint8_t *)xbox_GetMemoryOffset() + va;
                uint32_t d;
                uint16_t w;
                memcpy(&d, p, 4);
                memcpy(&w, p, 2);
                fprintf(stderr, "[CHEAT] RECOMP_CE %s -> guest %08X\n",
                        ce, (unsigned)va);
                fprintf(stderr, "[CHEAT]   b %08X %02X   |   w %08X %04X"
                                "   |   d %08X %08X\n",
                        (unsigned)va, p[0], (unsigned)va, w, (unsigned)va, d);
            } else {
                fprintf(stderr, "[CHEAT] RECOMP_CE %s -> guest %08X is outside"
                                " guest RAM\n", ce, (unsigned)va);
            }
        } else {
            fprintf(stderr, "[CHEAT] RECOMP_CE %s is below the window base\n", ce);
        }
    }
}

void ff_cheats_init(const char *dir)
{
    const char *on;
    if (!s_loaded)
        load(dir);
    report_offset();
    /* RECOMP_CHEATS_ON=0,3,4 -- the indices that were On when we last quit. */
    on = getenv("RECOMP_CHEATS_ON");
    if (on && *on) {
        const char *p = on;
        while (*p) {
            int i = atoi(p);
            if (i >= 0 && i < s_ncheat)
                s_cheat[i].on = 1;
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
    }
}

int ff_cheat_count(void) { return s_ncheat; }

const char *ff_cheat_name(int i)
{
    return (i >= 0 && i < s_ncheat) ? s_cheat[i].name : "";
}

int ff_cheat_on(int i)
{
    return (i >= 0 && i < s_ncheat) ? s_cheat[i].on : 0;
}

void ff_cheat_toggle(int i)
{
    if (i >= 0 && i < s_ncheat)
        s_cheat[i].on = !s_cheat[i].on;
}

/* The enabled set, for the ini writer. */
void ff_cheats_list(char *buf, int cap)
{
    int i, n = 0;
    buf[0] = 0;
    for (i = 0; i < s_ncheat; i++)
        if (s_cheat[i].on)
            n += snprintf(buf + n, (size_t)(cap - n), n ? ",%d" : "%d", i);
}

/* Called every frame: hold each enabled write down. */
void ff_cheats_apply(void)
{
    uint8_t *mem;
    int i, j;
    if (!s_ncheat)
        return;
    mem = (uint8_t *)xbox_GetMemoryOffset();
    if (!mem)
        return;
    for (i = 0; i < s_ncheat; i++) {
        if (!s_cheat[i].on)
            continue;
        for (j = 0; j < s_cheat[i].nw; j++) {
            Write *w = &s_cheat[i].w[j];
            volatile void *p = mem + w->va;
            switch (w->size) {
            case 8:  *(volatile uint8_t *)p  = (uint8_t)w->val;  break;
            case 16: *(volatile uint16_t *)p = (uint16_t)w->val; break;
            default: *(volatile uint32_t *)p = w->val;           break;
            }
        }
    }
}
