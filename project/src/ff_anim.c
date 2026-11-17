/* ff_anim.c -- find the guest value that drives a motion (the ladder climb's
 * clock, a timer, a counter) by watching guest RAM while the motion plays.
 *
 * RECOMP_ANIMSCAN=1 arms it; it costs nothing when unset. RECOMP_ANIMSCAN=<n>
 * records for n seconds instead of the default 30.
 *
 * One key. Press F8 as the motion starts; it records, then prints what it
 * found. Press F8 again to stop early.
 *
 * What it looks for: a value that, sampled every frame, steps FORWARD by the
 * SAME amount over and over. That is what a motion clock does for as long as
 * the motion runs, and it is what nothing else in the frame does -- a camera
 * wobble or a position holds a steady rate for three or four samples at most.
 * The report ranks by the longest steady run each address managed, so the
 * clock comes out at the top and the list never has to reach one entry.
 *
 * Two phases inside the window, so that scoring stays cheap: for the first
 * 1.5 s everything in RAM is a suspect and the pass is a plain full-memory
 * diff; after that only the addresses that moved are sampled, at frame rate.
 * That is why F8 goes down as the motion begins rather than before it. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RAM_LO    0x00010000u
#define RAM_HI    0x04000000u
#define MAXCAND   (1u << 21)         /* 2M addresses */
#define POOL_MS   1500u              /* phase 1: collect suspects */
#define SAMPLE_MS 15u                /* phase 2: about frame rate */
#define TOP       32

extern void *xbox_GetMemoryOffset(void);

typedef struct {
    uint32_t va, prev;
    float    mean;                   /* running mean step while it holds */
    uint32_t ticks, best;            /* current / longest steady run */
    uint32_t moves, resets;          /* samples that changed / went backwards */
} Cand;

enum { IDLE, POOLING, SCORING };

static uint8_t *s_base;
static Cand    *s_cand;
static uint32_t s_ncand;
static int      s_state;
static DWORD    s_t0, s_last;
static uint32_t s_samples;
static DWORD    s_done_at, s_poke_t;
static uint32_t s_total_ms = 30000;

#define MAXRGN 512
static struct { uint32_t lo, hi; } s_rgn[MAXRGN];
static int s_nrgn;

static const volatile uint32_t *host_of(uint32_t va)
{
    return (const volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va);
}

/* Guest RAM is not committed end to end; a blind walk would fault. */
static void map_regions(void)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = RAM_LO;
    s_nrgn = 0;
    while (va < RAM_HI && s_nrgn < MAXRGN) {
        MEMORY_BASIC_INFORMATION mbi;
        uint32_t end;
        if (!VirtualQuery(mem + va, &mbi, sizeof mbi))
            break;
        end = va + (uint32_t)(mbi.RegionSize > RAM_HI - va
                              ? RAM_HI - va : mbi.RegionSize);
        if (mbi.State == MEM_COMMIT
            && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            if (s_nrgn && s_rgn[s_nrgn - 1].hi == va)
                s_rgn[s_nrgn - 1].hi = end;
            else if (s_nrgn < MAXRGN) {
                s_rgn[s_nrgn].lo = va;
                s_rgn[s_nrgn].hi = end;
                s_nrgn++;
            }
        }
        va = end;
    }
}

static int looks_float(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return isfinite(f) && (f == 0.0f || (fabsf(f) > 1.0e-6f && fabsf(f) < 1.0e6f));
}

static void start(void)
{
    int r;
    if (!s_base)
        s_base = (uint8_t *)calloc(1, RAM_HI - RAM_LO);
    if (!s_cand)
        s_cand = (Cand *)calloc(MAXCAND, sizeof *s_cand);
    if (!s_base || !s_cand) {
        fprintf(stderr, "[ANIM] out of memory\n");
        return;
    }
    map_regions();
    for (r = 0; r < s_nrgn; r++)
        memcpy(s_base + (s_rgn[r].lo - RAM_LO),
               (const void *)host_of(s_rgn[r].lo), s_rgn[r].hi - s_rgn[r].lo);
    s_ncand = 0;
    s_samples = 0;
    s_state = POOLING;
    s_t0 = s_last = GetTickCount();
    fprintf(stderr, "[ANIM] recording %u s across %d region%s -- do the motion"
                    " now (F8 again to stop early)\n",
            s_total_ms / 1000, s_nrgn, s_nrgn == 1 ? "" : "s");
}

/* Phase 1 -> 2: everything that moved at all becomes a suspect. */
static void build_pool(void)
{
    const uint32_t *old = (const uint32_t *)s_base;
    uint32_t i = 0, va;
    int r;
    for (r = 0; r < s_nrgn && i < MAXCAND; r++)
        for (va = s_rgn[r].lo; va < s_rgn[r].hi && i < MAXCAND; va += 4) {
            uint32_t n = *host_of(va);
            if (n == old[(va - RAM_LO) >> 2])
                continue;
            s_cand[i].va = va;
            s_cand[i].prev = n;
            s_cand[i].mean = 0;
            s_cand[i].ticks = s_cand[i].best = 0;
            s_cand[i].moves = s_cand[i].resets = 0;
            i++;
        }
    s_ncand = i;
    s_state = SCORING;
    fprintf(stderr, "[ANIM] watching %u address%s%s\n", s_ncand,
            s_ncand == 1 ? "" : "es",
            s_ncand >= MAXCAND ? " (capped)" : "");
}

static void sample_once(void)
{
    uint32_t i;
    s_samples++;
    for (i = 0; i < s_ncand; i++) {
        Cand *c = &s_cand[i];
        uint32_t n = *host_of(c->va);
        float d;
        if (n == c->prev)
            continue;                /* a 30 Hz clock read at 60 Hz */
        c->moves++;
        if (looks_float(n) && looks_float(c->prev)) {
            float a, b;
            memcpy(&a, &c->prev, 4);
            memcpy(&b, &n, 4);
            d = b - a;
        } else {
            d = (float)((double)n - (double)c->prev);
        }
        c->prev = n;
        if (d <= 0.0f) {             /* backwards: the run ends */
            c->ticks = 0;
            c->resets++;
            continue;
        }
        if (c->ticks && fabsf(d - c->mean) <= 0.25f * fabsf(c->mean) + 1.0e-6f) {
            c->mean = c->mean * 0.9f + d * 0.1f;
            c->ticks++;
        } else {
            c->mean = d;             /* a new, so far steady, rate */
            c->ticks = 1;
        }
        if (c->ticks > c->best)
            c->best = c->ticks;
    }
}

static int cmp_best(const void *a, const void *b)
{
    uint32_t x = ((const Cand *)a)->best, y = ((const Cand *)b)->best;
    return x > y ? -1 : x < y ? 1 : 0;
}

/* A global clock -- the frame counter, an audio position, a wall timer --
 * advances steadily for the WHOLE window and never goes backwards, so it
 * wins a plain "longest run" ranking every time. A motion clock looks the
 * opposite way round: it runs while the motion plays, then stops or snaps
 * back when it ends. So the list only shows addresses that went backwards at
 * least once and that were moving for less than four fifths of the window,
 * and the always-on ones are counted off to the side. */
static void finish(void)
{
    uint32_t i, shown = 0, always = 0, never = 0;
    s_state = IDLE;
    s_done_at = GetTickCount();
    if (!s_ncand) {
        fprintf(stderr, "[ANIM] nothing moved -- was the game paused?\n");
        return;
    }
    for (i = 0; i < s_ncand; i++) {
        if (!s_cand[i].resets)
            never++;
        if (s_samples && s_cand[i].moves * 5u >= s_samples * 4u)
            always++;
    }
    qsort(s_cand, s_ncand, sizeof *s_cand, cmp_best);
    fprintf(stderr, "[ANIM] %u watched over %u samples;"
                    " %u never reset, %u ran the whole time -- both skipped\n",
            s_ncand, s_samples, never, always);
    fprintf(stderr, "[ANIM] starts and stops, longest steady run first:\n");
    for (i = 0; i < s_ncand && shown < TOP; i++) {
        Cand *c = &s_cand[i];
        uint32_t now;
        float f;
        if (c->best < 8 || !c->resets)
            continue;
        if (s_samples && c->moves * 5u >= s_samples * 4u)
            continue;
        now = *host_of(c->va);
        memcpy(&f, &now, 4);
        fprintf(stderr, "[ANIM]   %08X  run %-5u step %-12g moving %2u%%"
                        "  resets %-4u now %-13g (u32 %u)\n",
                c->va, c->best, (double)c->mean,
                s_samples ? c->moves * 100u / s_samples : 0u,
                c->resets, (double)f, now);
        shown++;
    }
    if (!shown)
        fprintf(stderr, "[ANIM]   nothing started and stopped -- try again and"
                        " make sure the motion ENDS inside the window\n");
    fprintf(stderr, "[ANIM] done -- F8 to record again\n");
}

/* ---- poke: test a candidate by hand ------------------------------------
 *
 * RECOMP_ANIMPOKE=<hex guest va>[:<multiplier>], e.g. 0038B920:2 -- every
 * sample, whatever the game just added to that dword gets multiplied. If the
 * address really is the motion's clock the motion runs that much faster, and
 * that is the whole experiment: one line in the ini, a ladder, and an answer.
 * The scaled value is what the next step is measured from, so the speed-up
 * compounds the way the game's own advance would.
 *
 * This writes into the running game's memory, so a wrong address misbehaves
 * or crashes. Delete the line to undo it; nothing is persisted. */
static uint32_t s_poke_va;
static float    s_poke_mult = 2.0f;
static uint32_t s_poke_prev;
static int      s_poke_live;

static void poke_once(void)
{
    volatile uint32_t *p = (volatile uint32_t *)host_of(s_poke_va);
    uint32_t n = *p;

    if (s_poke_live == 1) {                  /* first look: just remember */
        s_poke_prev = n;
        s_poke_live = 2;
        return;
    }
    if (n == s_poke_prev)
        return;
    if (looks_float(n) && looks_float(s_poke_prev)) {
        float a, b, d;
        memcpy(&a, &s_poke_prev, 4);
        memcpy(&b, &n, 4);
        d = b - a;
        if (d > 0.0f) {
            float v = a + d * s_poke_mult;
            memcpy(&n, &v, 4);
            *p = n;
        }
    } else if (n > s_poke_prev) {
        double d = (double)n - (double)s_poke_prev;
        uint32_t v = s_poke_prev + (uint32_t)(d * (double)s_poke_mult);
        *p = v;
        n = v;
    }
    s_poke_prev = n;
}

/* One line for the on-screen overlay, so a press is visibly doing something. */
int ff_anim_status(char *buf, int cap)
{
    DWORD now = GetTickCount();
    if (s_state == POOLING) {
        snprintf(buf, (size_t)cap, "ANIM arming...");
        return 1;
    }
    if (s_state == SCORING) {
        uint32_t left = (now - s_t0 >= s_total_ms)
                        ? 0 : (s_total_ms - (now - s_t0)) / 1000u;
        snprintf(buf, (size_t)cap, "ANIM REC %us  %u", left, s_ncand);
        return 1;
    }
    if (s_done_at && now - s_done_at < 8000u) {
        snprintf(buf, (size_t)cap, "ANIM done - see log");
        return 1;
    }
    if (s_poke_live) {
        snprintf(buf, (size_t)cap, "POKE %08X x%.2g",
                 s_poke_va, (double)s_poke_mult);
        return 1;
    }
    return 0;
}

void ff_anim_poll(void)
{
    static int on = -1;
    DWORD now;

    if (on < 0) {
        const char *e = getenv("RECOMP_ANIMSCAN");
        on = e && *e && strcmp(e, "0") != 0;
        if (on) {
            int secs = atoi(e);
            if (secs > 1 && secs <= 600)
                s_total_ms = (uint32_t)secs * 1000u;
            fprintf(stderr, "[ANIM] armed: press F8 as the motion starts"
                            " (records %u s)\n", s_total_ms / 1000);
        }
        e = getenv("RECOMP_ANIMPOKE");
        /* Not the on/off test used above: a guest address legitimately
         * STARTS with '0' (0038B920), so only the literal string "0" is off. */
        if (e && *e && strcmp(e, "0") != 0) {
            char *endp = NULL;
            unsigned long va = strtoul(e, &endp, 16);
            if (va >= RAM_LO && va < RAM_HI && !(va & 3u)) {
                s_poke_va = (uint32_t)va;
                if (endp && *endp == ':') {
                    double m = atof(endp + 1);
                    if (m > 0.0 && m < 100.0)
                        s_poke_mult = (float)m;
                }
                s_poke_live = 1;
                on = 1;
                fprintf(stderr, "[ANIM] poking %08X: every step the game takes"
                                " is multiplied by %.3g\n",
                        s_poke_va, (double)s_poke_mult);
            } else {
                fprintf(stderr, "[ANIM] RECOMP_ANIMPOKE=%s is not a usable"
                                " guest address\n", e);
            }
        }
    }
    if (!on)
        return;

    now = GetTickCount();
    if (s_poke_live && now - s_poke_t >= SAMPLE_MS) {
        s_poke_t = now;
        poke_once();
    }
    if (GetAsyncKeyState(VK_F8) & 1) {
        if (s_state == IDLE)
            start();
        else if (s_state == POOLING)
            fprintf(stderr, "[ANIM] stopped before it had suspects\n"), s_state = IDLE;
        else
            finish();
        return;
    }
    if (s_state == POOLING) {
        if (now - s_t0 >= POOL_MS)
            build_pool();
        return;
    }
    if (s_state == SCORING) {
        if (now - s_last >= SAMPLE_MS) {
            s_last = now;
            sample_once();
        }
        if (now - s_t0 >= s_total_ms)
            finish();
    }
}
