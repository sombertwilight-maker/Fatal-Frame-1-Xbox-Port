/* ff_tmdl.c -- load a model of our own, for our own render pass.
 *
 * The game's model format has ceilings the GPU does not: a vertex buffer
 * that cannot pass about 66 KB, index runs living inside fixed-size unit
 * records, one bone per rigid draw, material slots with no texture
 * coordinate at all. A costume that fits those limits is not the costume
 * anyone drew. So this reads a file of our own instead, and the renderer
 * draws it beside the game's own geometry using the bone matrices the game
 * has already posed.
 *
 * Files live in models/ beside the exe -- outside the game data, so a
 * player adds a costume without touching their disc image, and a bad file
 * cannot corrupt anything the game reads.
 *
 * Layout (little-endian):
 *   "TMDL" u32 version(2) u32 meshes u32 textures
 *   per mesh: u32 nverts, nindices, texture
 *             verts 40 bytes: f32 pos[3] nrm[3] uv[2], u16 boneA boneB,
 *                             f32 weightA
 *             indices: u32, a plain triangle list
 *   per texture: u32 w, h, then w*h*4 RGBA
 *
 * Positions are in model space and bones name the game's own skeleton, so
 * the renderer does the posing and nothing here bakes in a rest pose.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ff_tmdl.h"

#define TMDL_MAGIC  0x4C444D54u      /* "TMDL" */
#define TMDL_VER    2u
#define VERT_BYTES  40u
#define BONE_STRIDE 0xE0u   /* one entry of the engine's bone array */

static int rd(const uint8_t *b, size_t n, size_t *o, void *dst, size_t len)
{
    if (*o + len > n)
        return 0;
    memcpy(dst, b + *o, len);
    *o += len;
    return 1;
}

static uint32_t rd32(const uint8_t *b, size_t n, size_t *o, int *ok)
{
    uint32_t v = 0;
    if (!rd(b, n, o, &v, 4))
        *ok = 0;
    return v;
}

void ff_tmdl_free(FfModel *m)
{
    uint32_t i;
    if (!m)
        return;
    for (i = 0; i < m->n_mesh; i++) {
        free(m->mesh[i].verts);
        free(m->mesh[i].idx);
    }
    for (i = 0; i < m->n_tex; i++)
        free(m->tex[i].rgba);
    free(m->mesh);
    free(m->tex);
    memset(m, 0, sizeof *m);
}

int ff_tmdl_load(const char *path, FfModel *out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long size;
    size_t o = 0;
    uint32_t i, magic, ver;
    int ok = 1;

    memset(out, 0, sizeof *out);
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 16) {
        fclose(f);
        return 0;
    }
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);

    magic = rd32(buf, size, &o, &ok);
    ver = rd32(buf, size, &o, &ok);
    out->n_mesh = rd32(buf, size, &o, &ok);
    out->n_tex = rd32(buf, size, &o, &ok);
    if (!ok || magic != TMDL_MAGIC) {
        fprintf(stderr, "[TMDL] %s: not a model file\n", path);
        free(buf);
        return 0;
    }
    if (ver != TMDL_VER) {
        fprintf(stderr, "[TMDL] %s: version %u, this build reads %u\n",
                path, ver, TMDL_VER);
        free(buf);
        return 0;
    }
    if (out->n_mesh > 4096 || out->n_tex > 256) {
        fprintf(stderr, "[TMDL] %s: %u meshes, %u textures -- refusing\n",
                path, out->n_mesh, out->n_tex);
        free(buf);
        return 0;
    }

    out->mesh = (FfMesh *)calloc(out->n_mesh ? out->n_mesh : 1, sizeof(FfMesh));
    out->tex = (FfTexture *)calloc(out->n_tex ? out->n_tex : 1, sizeof(FfTexture));
    if (!out->mesh || !out->tex) {
        free(buf);
        ff_tmdl_free(out);
        return 0;
    }

    for (i = 0; i < out->n_mesh && ok; i++) {
        FfMesh *me = &out->mesh[i];
        me->n_vert = rd32(buf, size, &o, &ok);
        me->n_idx = rd32(buf, size, &o, &ok);
        me->texture = rd32(buf, size, &o, &ok);
        if (!ok || me->n_vert > 1u << 20 || me->n_idx > 1u << 22) {
            ok = 0;
            break;
        }
        me->verts = (FfVertex *)malloc(me->n_vert * sizeof(FfVertex));
        me->idx = (uint32_t *)malloc(me->n_idx * sizeof(uint32_t));
        if (!me->verts || !me->idx) {
            ok = 0;
            break;
        }
        /* FfVertex is laid out to match the file exactly, so the vertices
         * are one read rather than a loop of field copies. */
        ok = rd(buf, size, &o, me->verts, me->n_vert * VERT_BYTES)
             && rd(buf, size, &o, me->idx, me->n_idx * 4);
        out->tot_vert += me->n_vert;
        out->tot_idx += me->n_idx;
    }

    for (i = 0; i < out->n_tex && ok; i++) {
        FfTexture *t = &out->tex[i];
        size_t bytes;
        t->w = rd32(buf, size, &o, &ok);
        t->h = rd32(buf, size, &o, &ok);
        if (!ok || !t->w || !t->h || t->w > 4096 || t->h > 4096) {
            ok = 0;
            break;
        }
        bytes = (size_t)t->w * t->h * 4;
        t->rgba = (uint8_t *)malloc(bytes);
        if (!t->rgba) {
            ok = 0;
            break;
        }
        ok = rd(buf, size, &o, t->rgba, bytes);
    }

    free(buf);
    if (!ok) {
        fprintf(stderr, "[TMDL] %s: file ends early or is malformed\n", path);
        ff_tmdl_free(out);
        return 0;
    }
    fprintf(stderr, "[TMDL] %s: %u meshes, %u vertices, %u triangles,"
                    " %u textures\n",
            path, out->n_mesh, out->tot_vert, out->tot_idx / 3, out->n_tex);
    return 1;
}

/* models/<name>.tmdl beside the exe. */
int ff_tmdl_load_named(const char *name, FfModel *out)
{
    extern char g_ff_root[];
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s\\models\\%s.tmdl", g_ff_root, name);
    return ff_tmdl_load(path, out);
}

/* Loaded once at startup. The renderer asks for it by name later; for now
 * loading it proves the file is found, well formed and the right version. */
static FfModel s_model;
static int     s_have;

int ff_tmdl_boot(void)
{
    const char *name = getenv("RECOMP_TMDL");
    if (!name || !*name || strcmp(name, "0") == 0)
        name = "tormented";
    s_have = ff_tmdl_load_named(name, &s_model);
    if (!s_have)
        fprintf(stderr, "[TMDL] no models\\%s.tmdl -- the game's own model"
                        " is used\n", name);
    return s_have;
}

const FfModel *ff_tmdl_get(void)
{
    return s_have ? &s_model : NULL;
}

/* ---- which draws belong to the player ---------------------------------
 *
 * Reported from the pushbuffer executor, which is the only place that still
 * knows the guest address a draw reads its vertices from. For now this only
 * counts, so a frame can be checked against what the model actually has
 * before any geometry depends on the answer. RECOMP_TMDL_LOG=1 prints it. */
static int      s_dbg_on = -1;
static uint32_t s_seen_mine, s_seen_all, s_frames;
static uint32_t s_vert_mine;
static uint32_t s_vb_base;

uint32_t ff_model_vb_base(void) { return s_vb_base; }
uint32_t ff_model_frame_no(void) { return s_frames; }

/* Where the draws of a frame read their vertices from.
 *
 * The file-range test reported nothing, so rather than guess whether the
 * handle tag missed or the reads take another path, this records the
 * addresses themselves. The player's model is one allocation drawn many
 * times a frame, so it shows up as a tight cluster of addresses with a lot
 * of draws and a few thousand vertices between them -- which both tells us
 * the test is viable and hands us the range to use. */
#define SPOTS 24
typedef struct { uint32_t base, tex, draws, verts, mine; } Spot;
static Spot s_spot[SPOTS];
static uint32_t s_nspot;

void ff_model_draw_note(int mine, uint32_t vertex_addr, uint32_t tex_addr,
                        uint32_t count)
{
    uint32_t i, page;
    if (s_dbg_on < 0) {
        const char *e = getenv("RECOMP_TMDL_LOG");
        s_dbg_on = e && *e && strcmp(e, "0") != 0;
    }
    if (!s_dbg_on)
        return;
    s_seen_all++;
    if (mine) {
        s_seen_mine++;
        s_vert_mine += count;
        /* Draw 0 of the model sits at vb_rel 0 and is issued every frame,
         * so the lowest player address seen is the vertex buffer's base. */
        if (!s_vb_base || vertex_addr < s_vb_base)
            s_vb_base = vertex_addr;
    }
    if (!vertex_addr && !tex_addr)
        return;
    page = vertex_addr & ~0xFFFFu;          /* 64 KB buckets */
    for (i = 0; i < s_nspot; i++)
        if (s_spot[i].base == page) {
            s_spot[i].draws++;
            s_spot[i].verts += count;
            if (mine)
                s_spot[i].mine++;
            if (!s_spot[i].tex)
                s_spot[i].tex = tex_addr;
            return;
        }
    if (s_nspot < SPOTS) {
        s_spot[s_nspot].base = page;
        s_spot[s_nspot].tex = tex_addr;
        s_spot[s_nspot].draws = 1;
        s_spot[s_nspot].verts = count;
        s_spot[s_nspot].mine = mine ? 1 : 0;
        s_nspot++;
    }
}

/* ---- is this allocation the player's model? --------------------------
 *
 * The addresses the GPU reads vertices from sit above the 64 MB RAM window:
 * the title copies model data into GPU-visible memory rather than letting
 * the GPU read the loaded file in place. So the file's address is no use,
 * and a shader hash is no use either -- characters, ghosts and props share
 * vertex programs.
 *
 * What cannot drift is the geometry itself. The model file is on disk and
 * we can read it; whatever the title copies where, the bytes are the same.
 * So take a run of bytes from a draw's vertices and look for it in the
 * model's own vertex buffer. The answer is cached per 64 KB page, because
 * an allocation does not change identity between draws.
 */
#define PAGES 64
static struct { uint32_t base; int mine; } s_page[PAGES];
static uint32_t s_npage;
static uint8_t *s_ref;           /* the model's vertex buffer, from disk */
static uint32_t s_ref_len;

/* The loaded model file, for finding its posed bone array in guest RAM.
 *
 * The header is no good as an anchor: the title rewrites the offsets in it
 * into pointers once the file is in memory. Vertex bytes it never touches,
 * so a slice of the vertex buffer is what we hunt for, and the distance
 * from there back to the file's start is known from the header on disk. */
#define MAX_BONES 64
static int32_t  s_parent[MAX_BONES];   /* the bone tree, as it is on disk */
static int      s_hdr_have;
static uint32_t s_coordp, s_nbones;
static float    s_bind_inv[MAX_BONES][16];   /* inverse bind, per bone */
static float    s_bind_scale[MAX_BONES];     /* its uniform row scale */
static int      s_bind_have;
static uint32_t s_pal;           /* guest address of the posed bone array */
static int      s_pal_tried;

/* Keep the bone tree. Every other field of a bone changes as she moves --
 * the matrix most of all -- but a parent index is structure, copied in
 * with the file and never written again. Twenty-eight of them in a row at
 * a fixed stride is a pattern nothing else in memory wears, which makes it
 * the one dependable way to find the array once the title has it. */
void ff_model_set_header(const void *bones, uint32_t nbones)
{
    const uint8_t *p = (const uint8_t *)bones;
    uint32_t i;

    s_hdr_have = 0;
    s_coordp = 0;
    s_nbones = nbones;
    s_pal = 0;
    s_pal_tried = 0;
    if (!bones || !nbones || nbones > MAX_BONES)
        return;
    for (i = 0; i < nbones; i++)
        memcpy(&s_parent[i], p + i * BONE_STRIDE + 0xD0, 4);
    s_hdr_have = 1;
}

void ff_model_set_reference(const void *vb, uint32_t len)
{
    free(s_ref);
    s_ref = NULL;
    s_ref_len = 0;
    s_npage = 0;                 /* identities must be decided afresh */
    s_pal = 0;
    s_pal_tried = 0;
    if (!vb || !len)
        return;
    s_ref = (uint8_t *)malloc(len);
    if (!s_ref)
        return;
    memcpy(s_ref, vb, len);
    s_ref_len = len;
    fprintf(stderr, "[TMDL] reference geometry: %u bytes\n", len);
}

/* Comparing raw bytes was wrong: what the draw hands us has already been
 * gathered into the executor's own float layout, so it cannot match the
 * file's packed one even when the geometry is identical. Compare the
 * VALUES instead. A position is three consecutive floats, and they sit
 * consecutively in the file's vertex buffer too whatever the stride around
 * them, so scanning the reference as a float stream needs no knowledge of
 * the vertex format at all. */
static int position_in_reference(const float *p)
{
    const float *r = (const float *)s_ref;
    uint32_t n = s_ref_len / 4, i;
    if (!s_ref || n < 3)
        return 0;
    /* A position of exactly zero says nothing -- padding matches it. */
    if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f)
        return 0;
    for (i = 0; i + 3 <= n; i++)
        if (r[i] == p[0] && r[i + 1] == p[1] && r[i + 2] == p[2])
            return 1;
    return 0;
}

int ff_model_is_player(uint32_t vertex_addr, const void *host_vertices)
{
    uint32_t i, page = vertex_addr & ~0xFFFFu;
    if (!s_ref || !vertex_addr || !host_vertices)
        return 0;
    /* The buffer is 64KB-ish, so a page of it holds the tail of our model
     * and the head of whatever was allocated next. Once the base is known,
     * bound the answer by the buffer's real extent rather than the page --
     * without this the object sitting immediately after her is drawn as
     * hers, and suppressing the player would take it with her. */
    if (s_vb_base && (vertex_addr < s_vb_base
                      || vertex_addr >= s_vb_base + s_ref_len))
        return 0;
    for (i = 0; i < s_npage; i++)
        if (s_page[i].base == page)
            return s_page[i].mine;
    if (s_npage >= PAGES)
        return 0;
    s_page[s_npage].base = page;
    s_page[s_npage].mine = position_in_reference((const float *)host_vertices);
    if (s_page[s_npage].mine)
        fprintf(stderr, "[TMDL] player geometry lives at %08X\n", page);
    return s_page[s_npage++].mine;
}

void ff_model_frame_end(void)
{
    extern uint32_t g_ff_model_lo, g_ff_model_hi;
    if (s_dbg_on <= 0)
        return;
    if (++s_frames % 120)
        return;
    fprintf(stderr, "[TMDL] model file at %08X..%08X | per 120 frames:"
                    " %u of %u draws were the player, %u vertices\n",
            g_ff_model_lo, g_ff_model_hi, s_seen_mine, s_seen_all, s_vert_mine);
    {
        uint32_t i, j;
        for (i = 0; i < s_nspot; i++)       /* busiest first */
            for (j = i + 1; j < s_nspot; j++)
                if (s_spot[j].mine > s_spot[i].mine ||
                    (s_spot[j].mine == s_spot[i].mine &&
                     s_spot[j].draws > s_spot[i].draws)) {
                    Spot t = s_spot[i];
                    s_spot[i] = s_spot[j];
                    s_spot[j] = t;
                }
        for (i = 0; i < s_nspot && i < 8; i++)
            fprintf(stderr, "[TMDL]    verts at %08X  tex %08X  %5u draws"
                            "  %7u verts  %5u PLAYER\n",
                    s_spot[i].base, s_spot[i].tex, s_spot[i].draws,
                    s_spot[i].verts, s_spot[i].mine);
        s_nspot = 0;
    }
    s_seen_mine = s_seen_all = s_vert_mine = 0;
}

/* ---------------------------------------------------------------- *
 * Finding the bone matrix palette
 *
 * The skinned draws arrive with an identity matrix in c[96..98], so the
 * title skins them on the CPU: somewhere in guest RAM is an array of posed
 * bone matrices. The rigid draws hand us fifteen of its entries exactly, so
 * we can go looking for them. Two known matrices give both the base and the
 * stride; printing the floats around each hit shows the layout.
 * ---------------------------------------------------------------- */
#define SCAN_RAM_LO 0x00010000u
#define SCAN_RAM_HI 0x04000000u
#define SCAN_HITS   16

extern void *xbox_GetMemoryOffset(void);

static float s_known[2][12];
static int   s_known_have[2];
static int   s_scan_done;

static int near_eq(float a, float b)
{
    float d = a - b;
    if (a != a || b != b)                    /* NaN never matches */
        return 0;
    return d > -1e-3f && d < 1e-3f;
}

/* Which arrangement of floats around `f` spells out the matrix `want`,
 * whose rows we saw as (0,1,2 | 4,5,6 | 8,9,10) in the constant file?
 * The title may keep them row-major, column-major, 3x4 or 4x4, so rather
 * than guess we name the one that fits. */
static const char *layout_at(const float *f, const float *want)
{
    if (near_eq(f[1], want[1]) && near_eq(f[2], want[2])
     && near_eq(f[4], want[4]) && near_eq(f[8], want[8]))
        return "row-major 4x4";
    if (near_eq(f[1], want[1]) && near_eq(f[2], want[2])
     && near_eq(f[3], want[4]) && near_eq(f[6], want[8]))
        return "row-major 3x4 packed";
    if (near_eq(f[4], want[1]) && near_eq(f[8], want[2])
     && near_eq(f[1], want[4]) && near_eq(f[2], want[8]))
        return "column-major 4x4";
    if (near_eq(f[3], want[1]) && near_eq(f[6], want[2])
     && near_eq(f[1], want[4]) && near_eq(f[2], want[8]))
        return "column-major 3x4 packed";
    return NULL;
}

static uint32_t scan_for(const float *host, const float *want,
                         uint32_t *hits, char **lay, uint32_t max)
{
    uint32_t n = 0, i, words = (SCAN_RAM_HI - SCAN_RAM_LO) / 4;
    const float *p = host + SCAN_RAM_LO / 4;
    for (i = 0; i + 16 < words; i++) {
        const char *l;
        if (p[i] != want[0])                 /* exact on the anchor float */
            continue;
        l = layout_at(p + i, want);
        if (!l)
            continue;
        if (n < max) {
            hits[n] = SCAN_RAM_LO + i * 4;
            lay[n] = (char *)l;
        }
        n++;
    }
    return n;
}

static uint32_t scan_report(const float *host, const char *tag,
                            const float *want)
{
    uint32_t hits[SCAN_HITS], n, i, j;
    char *lay[SCAN_HITS];

    n = scan_for(host, want, hits, lay, SCAN_HITS);
    fprintf(stderr, "[MAT] %s: %u hit(s)\n", tag, n);
    for (i = 0; i < n && i < SCAN_HITS; i++) {
        const float *f = host + hits[i] / 4;
        fprintf(stderr, "[MAT]   at %08X  %s\n", hits[i], lay[i]);
        for (j = 0; j < 16; j += 4)
            fprintf(stderr, "[MAT]     +%-3u %12.4f %12.4f %12.4f %12.4f\n",
                    j * 4, f[j], f[j+1], f[j+2], f[j+3]);
    }
    return n == 1 ? hits[0] : 0;
}

/* Proof, not assumption: the matrix the title just handed the GPU for one
 * of her draws must BE one of the entries in the array we located. If no
 * entry matches, we are reading the wrong memory and posing from noise. */
void ff_model_verify_palette(const float *consts96)
{
    static int done;
    const float *base;
    uint32_t i;

    if (done || s_dbg_on <= 0 || !s_pal || !s_nbones)
        return;
    done = 1;
    base = (const float *)((const uint8_t *)xbox_GetMemoryOffset() + s_pal);
    for (i = 0; i < s_nbones; i++) {
        const float *f = base + i * (BONE_STRIDE / 4);
        /* the constant file is row-major 3x4, the array column-major 4x4 */
        float d0 = f[0]  - consts96[0], d1 = f[4]  - consts96[1];
        float d2 = f[12] - consts96[3], d3 = f[1]  - consts96[4];
        if (d0 > -1e-2f && d0 < 1e-2f && d1 > -1e-2f && d1 < 1e-2f
         && d2 > -1e-1f && d2 < 1e-1f && d3 > -1e-2f && d3 < 1e-2f) {
            fprintf(stderr, "[TMDL] pose confirmed: this draw is bone %u"
                            " of the array at %08X\n", i, s_pal);
            return;
        }
    }
    fprintf(stderr, "[TMDL] POSE IS WRONG: no bone at %08X matches the"
                    " matrix the title just used\n", s_pal);
}

void ff_model_note_matrix(uint32_t rel, const float *m)
{
    static int on = -1;
    const float *host;

    if (on < 0) {
        const char *e = getenv("RECOMP_TMDL_SCANMAT");
        on = e && e[0] && strcmp(e, "0") != 0;
    }
    if (!on || s_scan_done)
        return;
    if (rel == 0 && !s_known_have[0]) {
        memcpy(s_known[0], m, sizeof s_known[0]);
        s_known_have[0] = 1;
    } else if (rel == 1728 && !s_known_have[1]) {
        memcpy(s_known[1], m, sizeof s_known[1]);
        s_known_have[1] = 1;
    }
    if (!s_known_have[0] || !s_known_have[1])
        return;

    s_scan_done = 1;
    host = (const float *)xbox_GetMemoryOffset();
    fprintf(stderr, "[MAT] scanning guest RAM for the posed bone matrices\n");
    fprintf(stderr, "[MAT] bone A (draw 0) row0 %.4f %.4f %.4f\n",
            s_known[0][0], s_known[0][1], s_known[0][2]);
    fprintf(stderr, "[MAT] bone B (draw 2) row0 %.4f %.4f %.4f\n",
            s_known[1][0], s_known[1][1], s_known[1][2]);
    {
        uint32_t a = scan_report(host, "bone A", s_known[0]);
        uint32_t b = scan_report(host, "bone B", s_known[1]);
        /* draw 0 is bone 3 and draw 2 is bone 4 in this model, so the gap
         * between the two hits is one bone's worth of the palette. */
        if (a && b && b > a) {
            uint32_t stride = b - a;
            uint32_t base = a - 3 * stride;
            uint32_t i, j;
            fprintf(stderr, "[MAT] palette base %08X stride %u bytes\n",
                    base, stride);
            for (i = 0; i < 28; i++) {
                const float *f = host + (base + i * stride) / 4;
                fprintf(stderr, "[MAT] bone %2u @%08X\n", i, base + i * stride);
                for (j = 0; j < stride / 4 && j < 32; j += 4)
                    fprintf(stderr, "[MAT]   +%-3u %12.4f %12.4f %12.4f %12.4f\n",
                            j * 4, f[j], f[j+1], f[j+2], f[j+3]);
            }
        }
    }
    fprintf(stderr, "[MAT] scan complete\n");
}

/* ---------------------------------------------------------------- *
 * The posed bone array
 *
 * The title loads the .mpx into main RAM and poses the bones in place, in
 * the file's own bone array -- the array we parse offline sits at coordp
 * from the file's start, 0xE0 bytes an entry, the matrix first and column
 * major. So finding the loaded file finds the pose: search RAM for the
 * header we read off disk, which does not change as she moves, rather than
 * for a matrix, which does.
 * ---------------------------------------------------------------- */

/* A bone matrix has a shape: no projection column, a 1 in the corner, and
 * the same scale down all three rows. Checking every entry says whether we
 * are looking at the bone array or at whatever happens to lie nearby. */
static int palette_looks_right(uint32_t at)
{
    const float *base = (const float *)((const uint8_t *)xbox_GetMemoryOffset()
                                        + at);
    uint32_t i, r;

    for (i = 0; i < s_nbones; i++) {
        const float *f = base + i * (BONE_STRIDE / 4);
        float s0 = 0.0f;
        if (f[3] != 0.0f || f[7] != 0.0f || f[11] != 0.0f || f[15] != 1.0f)
            return 0;
        for (r = 0; r < 3; r++) {
            float a = f[r * 4], b = f[r * 4 + 1], c = f[r * 4 + 2];
            float len = (float)sqrt(a * a + b * b + c * c);
            if (len < 1e-4f || len > 1e5f)
                return 0;
            if (!r)
                s0 = len;
            else if (len < s0 * 0.98f || len > s0 * 1.02f)
                return 0;
        }
    }
    return 1;
}

/* Aligning the array to the bones
 *
 * One entry can be placed exactly -- the matrix the title just used for a
 * draw is in there verbatim -- but that does not say WHICH bone it is, and
 * the fields that would have said so are overwritten with animation data
 * by the time we look.
 *
 * The pose itself answers it. Every posed matrix is its bind matrix times
 * the same world scale, so posed_scale / bind_scale is one constant across
 * the whole skeleton. Line the array up wrongly and that breaks loudly,
 * because the root bone is the only one whose bind scale is 1 where every
 * other is 1.919 -- pair it with the wrong entry and the ratio is out by
 * nearly half. So we try each alignment and keep the one that holds.
 */
static const float *s_live;

void ff_model_note_live_matrix(const float *c96) { s_live = c96; }

static float posed_scale(const uint8_t *host, uint32_t at, uint32_t i)
{
    const float *f = (const float *)(host + at + i * BONE_STRIDE);
    return (float)sqrt(f[0] * f[0] + f[4] * f[4] + f[8] * f[8]);
}

static int alignment_holds(const uint8_t *host, uint32_t at)
{
    float lo = 0.0f, hi = 0.0f;
    uint32_t i;

    if (!palette_looks_right(at))
        return 0;
    for (i = 0; i < s_nbones; i++) {
        float r = posed_scale(host, at, i) / s_bind_scale[i];
        if (r < 1e-4f)
            return 0;
        if (!i || r < lo) lo = r;
        if (!i || r > hi) hi = r;
    }
    return hi < lo * 1.02f;
}

static uint32_t locate_palette(void)
{
    const uint8_t *host;
    const float *p;
    uint32_t words, i, hits = 0, found = 0, k, nfit = 0, fit = 0;

    if (s_pal || s_pal_tried || !s_hdr_have || !s_bind_have || !s_live)
        return s_pal;
    s_pal_tried = 1;
    host = (const uint8_t *)xbox_GetMemoryOffset();
    p = (const float *)host + SCAN_RAM_LO / 4;
    words = (SCAN_RAM_HI - SCAN_RAM_LO) / 4;

    for (i = 0; i + 16 < words; i++) {          /* column major in memory */
        if (p[i] != s_live[0] || p[i + 4] != s_live[1]
                              || p[i + 8] != s_live[2])
            continue;
        if (!hits)
            found = SCAN_RAM_LO + i * 4;
        hits++;
    }
    if (hits != 1) {
        fprintf(stderr, "[TMDL] cannot place a bone: %u match the matrix"
                        " in use\n", hits);
        return 0;
    }
    for (k = 0; k < s_nbones; k++) {
        uint32_t at = found - k * BONE_STRIDE;
        if (at < SCAN_RAM_LO || at + s_nbones * BONE_STRIDE >= SCAN_RAM_HI)
            continue;
        if (alignment_holds(host, at)) {
            fit = at;
            nfit++;
        }
    }
    if (nfit == 1) {
        s_pal = fit;
        fprintf(stderr, "[TMDL] posed bones at %08X (%u of them); the draw"
                        " we matched is bone %u\n",
                s_pal, s_nbones, (found - s_pal) / BONE_STRIDE);
    } else {
        fprintf(stderr, "[TMDL] bone at %08X but %u alignments fit -- no"
                        " pose\n", found, nfit);
    }
    return s_pal;
}

/* The bone's posed matrix as a row-major 3x4, the layout the vertex
 * program's constant file uses. Returns 0 if the pose is not available. */
int ff_model_bone_matrix(uint32_t bone, float out[12])
{
    const float *f;
    uint32_t r, c;

    if (!locate_palette() || bone >= s_nbones)
        return 0;
    f = (const float *)((const uint8_t *)xbox_GetMemoryOffset()
                        + s_pal + bone * BONE_STRIDE);
    for (r = 0; r < 3; r++)
        for (c = 0; c < 4; c++)
            out[r * 4 + c] = f[c * 4 + r];      /* column major -> row major */
    return 1;
}

uint32_t ff_model_bone_count(void) { return s_nbones; }

/* Is the player's own geometry to be drawn at all? When the replacement
 * model is showing, her draws are dropped and ours stand in for them. */
int ff_model_suppress(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_TORMENTED");
        on = e && e[0] && strcmp(e, "0") != 0;
    }
    return on;
}

/* ---------------------------------------------------------------- *
 * Posing the replacement model
 *
 * Both the file's bind matrices and the title's posed ones are 4x4 with
 * the translation in the last row -- row-vector convention, v' = v * M.
 * A vertex of ours is authored in the game's model space, so the transform
 * that carries it to where the title has put that bone is
 *
 *     skin[b] = inverse(bind[b]) * posed[b]
 *
 * and a two-bone vertex is the weighted sum of the two. The title does the
 * same thing for its own skinned pieces -- it skins on the CPU and hands
 * the GPU world-space vertices with an identity matrix -- so our draws go
 * down exactly the path its own do.
 * ---------------------------------------------------------------- */
static float s_skin[MAX_BONES][16];

/* Inverse of an affine 4x4 held row-vector style: the 3x3 is inverted and
 * the translation carried back through it. */
static int affine_inverse(const float *m, float *out)
{
    float a = m[0], b = m[1], c = m[2];
    float d = m[4], e = m[5], f = m[6];
    float g = m[8], h = m[9], i = m[10];
    float A =  (e * i - f * h), B = -(d * i - f * g), C =  (d * h - e * g);
    float det = a * A + b * B + c * C, id;

    if (det > -1e-12f && det < 1e-12f)
        return 0;
    id = 1.0f / det;
    out[0] = A * id;              out[1] = -(b * i - c * h) * id;
    out[2] = (b * f - c * e) * id; out[3] = 0.0f;
    out[4] = B * id;              out[5] =  (a * i - c * g) * id;
    out[6] = -(a * f - c * d) * id; out[7] = 0.0f;
    out[8] = C * id;              out[9] = -(a * h - b * g) * id;
    out[10] = (a * e - b * d) * id; out[11] = 0.0f;
    out[12] = -(m[12] * out[0] + m[13] * out[4] + m[14] * out[8]);
    out[13] = -(m[12] * out[1] + m[13] * out[5] + m[14] * out[9]);
    out[14] = -(m[12] * out[2] + m[13] * out[6] + m[14] * out[10]);
    out[15] = 1.0f;
    return 1;
}

static void mat_mul(const float *x, const float *y, float *out)
{
    int r, c;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            out[r * 4 + c] = x[r * 4 + 0] * y[c]
                           + x[r * 4 + 1] * y[4 + c]
                           + x[r * 4 + 2] * y[8 + c]
                           + x[r * 4 + 3] * y[12 + c];
}

/* The bind pose, straight off the bone array in the file we just read. */
void ff_model_set_bind(const void *bones, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)bones;
    uint32_t i;

    s_bind_have = 0;
    if (!bones || !n || n > MAX_BONES)
        return;
    for (i = 0; i < n; i++) {
        const float *b = (const float *)(p + i * BONE_STRIDE);
        float m[16];
        uint32_t r, c;

        /* The bind matrix carries a scale of its own -- 1.919 here -- but
         * the space the title poses from does not: its bone-local space
         * uses these axes normalized. Invert the raw matrix and every
         * offset from a joint comes out at half size, which leaves the
         * skeleton in the right place with the body shrunk onto it. So
         * normalize the axes first and keep the translation as it is. */
        memcpy(m, b, sizeof m);
        for (r = 0; r < 3; r++) {
            float len = (float)sqrt(m[r * 4] * m[r * 4]
                                  + m[r * 4 + 1] * m[r * 4 + 1]
                                  + m[r * 4 + 2] * m[r * 4 + 2]);
            if (len < 1e-6f)
                return;
            if (!r)
                s_bind_scale[i] = len;
            for (c = 0; c < 3; c++)
                m[r * 4 + c] /= len;
        }
        if (!affine_inverse(m, s_bind_inv[i]))
            return;
    }
    s_bind_have = 1;
}

/* Rebuild skin[] from the pose the title has just computed. */
static int build_skin(void)
{
    uint32_t i, n = s_nbones;
    float posed[16];

    if (!s_bind_have || !locate_palette() || n > MAX_BONES)
        return 0;
    for (i = 0; i < n; i++) {
        const float *f = (const float *)((const uint8_t *)xbox_GetMemoryOffset()
                                         + s_pal + i * BONE_STRIDE);
        memcpy(posed, f, sizeof posed);
        mat_mul(s_bind_inv[i], posed, s_skin[i]);
    }
    return 1;
}

/* ---------------------------------------------------------------- *
 * Handing the posed model to the draw path
 *
 * The executor wants vertices in its own gathered layout: sixteen
 * attributes of four floats each, of which the title's character program
 * reads v0 position, v1 normal and v2 texture coordinate. We fill that
 * once a frame and the draw site walks the meshes.
 * ---------------------------------------------------------------- */
#define VSH_ATTRS  16

static void upload_textures(void);

/* The model is loaded once at boot; this is the posed copy of it. */
static const FfModel *s_rep;
static float   *s_posed;              /* [tot_vert][VSH_ATTRS][4] */
static uint32_t *s_base;              /* first vertex of each mesh */
static int      s_posed_ok;

/* Meshes in texture order.
 *
 * The model arrives as 165 little meshes and drawing each one its own way
 * cost more than the rest of the frame put together: every draw re-uploads
 * its vertices and the back end re-hashes the whole texture to see whether
 * it changed. Ordering the meshes by texture puts everything that shares a
 * texture side by side, so a frame becomes one draw per texture over a
 * contiguous run of vertices -- eight instead of a hundred and sixty-five.
 */
static uint32_t *s_order;             /* mesh indices, grouped by texture */

#define MAX_GROUPS 32
static struct { uint32_t tex, first, count, vbase, vcount; }
    s_group[MAX_GROUPS];
static uint32_t s_ngroup, *s_gidx, s_nidx;

static int by_texture(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    uint32_t tx = s_rep->mesh[x].texture, ty = s_rep->mesh[y].texture;
    if (tx != ty)
        return tx < ty ? -1 : 1;
    return x < y ? -1 : (x > y);
}

static int replacement_ready(void)
{
    uint32_t i, run = 0;

    if (s_rep)
        return 1;
    s_rep = ff_tmdl_get();
    if (!s_rep || !s_rep->tot_vert)
        return 0;
    s_posed = (float *)calloc((size_t)s_rep->tot_vert * VSH_ATTRS * 4,
                              sizeof *s_posed);
    s_base = (uint32_t *)calloc(s_rep->n_mesh, sizeof *s_base);
    s_order = (uint32_t *)calloc(s_rep->n_mesh, sizeof *s_order);
    if (!s_posed || !s_base || !s_order) {
        free(s_posed); free(s_base); free(s_order);
        s_posed = NULL; s_base = NULL; s_order = NULL; s_rep = NULL;
        return 0;
    }
    for (i = 0; i < s_rep->n_mesh; i++)
        s_order[i] = i;
    qsort(s_order, s_rep->n_mesh, sizeof *s_order, by_texture);
    for (i = 0; i < s_rep->n_mesh; i++) {
        s_base[s_order[i]] = run;
        run += s_rep->mesh[s_order[i]].n_vert;
    }
    /* Fold each texture's meshes into one run of indices over the shared
     * vertex array, so the frame costs one draw per texture. */
    {
        uint32_t tot = 0, g = 0, k;
        for (i = 0; i < s_rep->n_mesh; i++)
            tot += s_rep->mesh[i].n_idx;
        s_gidx = (uint32_t *)calloc(tot ? tot : 1, sizeof *s_gidx);
        if (!s_gidx)
            return 0;
        for (i = 0; i < s_rep->n_mesh; ) {
            const uint32_t tex = s_rep->mesh[s_order[i]].texture;
            uint32_t first = s_nidx, vbase = s_base[s_order[i]], vcount = 0;
            /* Indices count from the group's own first vertex, so a draw
             * uploads only its slice. Handing every draw the whole model
             * meant eighteen megabytes of vertices a frame for a model
             * that is two. */
            while (i < s_rep->n_mesh
                   && s_rep->mesh[s_order[i]].texture == tex) {
                const FfMesh *me = &s_rep->mesh[s_order[i]];
                for (k = 0; k < me->n_idx; k++)
                    s_gidx[s_nidx++] = me->idx[k]
                                     + (s_base[s_order[i]] - vbase);
                vcount += me->n_vert;
                i++;
            }
            if (g < MAX_GROUPS) {
                s_group[g].tex = tex;
                s_group[g].first = first;
                s_group[g].count = s_nidx - first;
                s_group[g].vbase = vbase;
                s_group[g].vcount = vcount;
                g++;
            }
        }
        s_ngroup = g;
        fprintf(stderr, "[TMDL] %u meshes folded into %u draws\n",
                s_rep->n_mesh, s_ngroup);
    }
    fprintf(stderr, "[TMDL] replacement ready: %u meshes, %u vertices\n",
            s_rep->n_mesh, s_rep->tot_vert);
    upload_textures();
    return 1;
}

/* Skin every vertex into world space. Called once a frame, before the
 * first of our draws goes down. */
int ff_model_pose(const float *c96)
{
    uint32_t mi, vi, run = 0;
    float anchor[16], inv[16], b;

    s_posed_ok = 0;
    if (!replacement_ready() || !build_skin())
        return 0;

    /* Hand the vertex program vertices in the space it already expects.
     *
     * Overriding its matrix with identity was wrong, and not only for the
     * geometry: the same constants feed the lighting, so a degenerate
     * matrix there gives a black model, and a black model in a dark house
     * is an invisible one. The draw we stand in on carries its bone's
     * matrix; carry our world-space vertices back through it and the
     * program transforms and lights them exactly as it does her own.
     */
    if (!c96)
        return 0;
    for (b = 0, mi = 0; mi < 3; mi++) {          /* 3x4 rows -> row-vector */
        anchor[0 * 4 + mi] = c96[mi * 4 + 0];
        anchor[1 * 4 + mi] = c96[mi * 4 + 1];
        anchor[2 * 4 + mi] = c96[mi * 4 + 2];
        anchor[3 * 4 + mi] = c96[mi * 4 + 3];
    }
    anchor[3] = anchor[7] = anchor[11] = 0.0f;
    anchor[15] = 1.0f;
    (void)b;
    if (!affine_inverse(anchor, inv))
        return 0;
    for (mi = 0; mi < s_nbones; mi++) {
        float t[16];
        memcpy(t, s_skin[mi], sizeof t);
        mat_mul(t, inv, s_skin[mi]);
    }
    for (mi = 0; mi < s_rep->n_mesh; mi++) {
        const FfMesh *me = &s_rep->mesh[s_order[mi]];
        for (vi = 0; vi < me->n_vert; vi++, run++) {
            const FfVertex *v = &me->verts[vi];
            float *o = s_posed + (size_t)run * VSH_ATTRS * 4;
            const float *A = s_skin[v->bone_a < s_nbones ? v->bone_a : 0];
            const float *B = s_skin[v->bone_b < s_nbones ? v->bone_b : 0];
            float wa = v->weight_a, wb = 1.0f - wa;
            int k;

            for (k = 0; k < 3; k++) {
                o[k] = wa * (v->pos[0] * A[k] + v->pos[1] * A[4 + k]
                           + v->pos[2] * A[8 + k] + A[12 + k])
                     + wb * (v->pos[0] * B[k] + v->pos[1] * B[4 + k]
                           + v->pos[2] * B[8 + k] + B[12 + k]);
                o[4 + k] = wa * (v->nrm[0] * A[k] + v->nrm[1] * A[4 + k]
                               + v->nrm[2] * A[8 + k])
                         + wb * (v->nrm[0] * B[k] + v->nrm[1] * B[4 + k]
                               + v->nrm[2] * B[8 + k]);
            }
            o[3] = 1.0f;
            o[7] = 1.0f;
            o[8] = v->uv[0];
            o[9] = v->uv[1];
            o[10] = 0.0f;
            o[11] = 1.0f;
        }
    }
    {   /* Once: the pose we built, and what it did to the model, so the
         * same arithmetic can be reproduced and looked at off the machine. */
        static uint32_t told;
        if (s_dbg_on > 0 && told++ % 120 == 0) {
            uint32_t i;
            float lo[3], hi[3];
            for (i = 0; i < 3; i++) { lo[i] = 1e30f; hi[i] = -1e30f; }
            for (vi = 0; vi < s_rep->tot_vert; vi++) {
                const float *o = s_posed + (size_t)vi * VSH_ATTRS * 4;
                for (mi = 0; mi < 3; mi++) {
                    if (o[mi] < lo[mi]) lo[mi] = o[mi];
                    if (o[mi] > hi[mi]) hi[mi] = o[mi];
                }
            }
            fprintf(stderr, "[POSE] drawn bounds  x %.1f..%.1f  y %.1f..%.1f"
                            "  z %.1f..%.1f\n",
                    lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
            fprintf(stderr, "[POSE] anchor %.4f %.4f %.4f %.4f %.4f %.4f"
                            " %.4f %.4f %.4f %.4f %.4f %.4f\n",
                    c96[0], c96[1], c96[2], c96[3], c96[4], c96[5],
                    c96[6], c96[7], c96[8], c96[9], c96[10], c96[11]);
            /* The first few vertices, in and out, so the same arithmetic
             * can be run off the machine and compared number for number. */
            for (vi = 0; vi < 4 && vi < s_rep->mesh[0].n_vert; vi++) {
                const FfVertex *v = &s_rep->mesh[0].verts[vi];
                const float *o = s_posed + (size_t)vi * VSH_ATTRS * 4;
                fprintf(stderr, "[POSE] v%u in %.4f %.4f %.4f  bones %u %u"
                                " w %.4f  out %.3f %.3f %.3f\n",
                        vi, v->pos[0], v->pos[1], v->pos[2],
                        v->bone_a, v->bone_b, v->weight_a, o[0], o[1], o[2]);
            }
            for (i = 0; i < s_nbones; i++) {
                const float *f = (const float *)
                    ((const uint8_t *)xbox_GetMemoryOffset()
                     + s_pal + i * BONE_STRIDE);
                fprintf(stderr, "[POSE] bone %2u %.4f %.4f %.4f %.4f %.4f %.4f"
                                " %.4f %.4f %.4f %.4f %.4f %.4f\n", i,
                        f[0], f[1], f[2], f[4], f[5], f[6],
                        f[8], f[9], f[10], f[12], f[13], f[14]);
            }
        }
    }
    s_posed_ok = 1;
    return 1;
}

uint32_t ff_model_mesh_count(void)
{
    return s_posed_ok ? s_ngroup : 0;
}

/* One texture's worth of the model: the whole vertex array and the span of
 * indices that uses this texture. */
int ff_model_mesh(uint32_t i, const float **verts, uint32_t *nv,
                  const uint32_t **idx, uint32_t *ni, uint32_t *tex)
{
    if (!s_posed_ok || i >= s_ngroup)
        return 0;
    *verts = s_posed + (size_t)s_group[i].vbase * VSH_ATTRS * 4;
    *nv    = s_group[i].vcount;
    *idx   = s_gidx + s_group[i].first;
    *ni    = s_group[i].count;
    *tex   = s_group[i].tex;
    return 1;
}

/* ---------------------------------------------------------------- *
 * Our own textures, where the GPU can read them
 *
 * A texture reaches the back end as a guest address, so ours have to live
 * in guest memory like the title's own. The contiguous allocator gives us
 * a window of our own; the images go in as LIN_A8R8G8B8, which is the
 * same four bytes the TMDL carries in the other order.
 *
 * This is not only about looks. Standing in on one of her draws meant
 * taking its texture and its combiner program as well, and those decide
 * the colour of every pixel we draw -- which is how the model came out
 * white one build and invisible the next. With our own textures bound and
 * the combiners off, what we draw stops depending on whose draw we
 * borrowed.
 * ---------------------------------------------------------------- */
#define MAX_TEX 16

extern uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t align);

static uint32_t s_tex_va[MAX_TEX], s_tex_w[MAX_TEX], s_tex_h[MAX_TEX];
static int      s_tex_done;

static void upload_textures(void)
{
    uint32_t i, total = 0, off = 0, va;
    uint8_t *base;

    if (s_tex_done || !s_rep)
        return;
    s_tex_done = 1;
    for (i = 0; i < s_rep->n_tex && i < MAX_TEX; i++)
        total += s_rep->tex[i].w * s_rep->tex[i].h * 4;
    if (!total)
        return;
    va = xbox_ContiguousAlloc(total, 4096);
    if (!va) {
        fprintf(stderr, "[TMDL] no room for %u bytes of textures\n", total);
        return;
    }
    base = (uint8_t *)xbox_GetMemoryOffset() + va;
    for (i = 0; i < s_rep->n_tex && i < MAX_TEX; i++) {
        const FfTexture *t = &s_rep->tex[i];
        uint32_t n = t->w * t->h, k;
        uint8_t *d = base + off;

        for (k = 0; k < n; k++) {               /* RGBA -> B G R A */
            d[k * 4 + 0] = t->rgba[k * 4 + 2];
            d[k * 4 + 1] = t->rgba[k * 4 + 1];
            d[k * 4 + 2] = t->rgba[k * 4 + 0];
            d[k * 4 + 3] = t->rgba[k * 4 + 3];
        }
        s_tex_va[i] = va + off;
        s_tex_w[i] = t->w;
        s_tex_h[i] = t->h;
        off += n * 4;
    }
    fprintf(stderr, "[TMDL] %u textures at %08X, %u bytes\n",
            s_rep->n_tex, va, total);
}

int ff_model_texture(uint32_t i, uint32_t *va, uint32_t *w, uint32_t *h)
{
    if (i >= MAX_TEX || !s_tex_va[i])
        return 0;
    *va = s_tex_va[i];
    *w = s_tex_w[i];
    *h = s_tex_h[i];
    return 1;
}

/* The first posed vertex, in the executor's own gathered layout, so the
 * vertex program can be run over it and its landing place compared with
 * one of hers. */
int ff_model_first_vertex(float out[16][4])
{
    if (!s_posed_ok || !s_rep || !s_rep->tot_vert)
        return 0;
    memcpy(out, s_posed, 16 * 4 * sizeof(float));
    return 1;
}

/* Which of her draws do we stand in on?
 *
 * Ours inherit the GPU state and the lighting constants of whichever draw
 * we hook. Her first draw of the frame turned out to be an eye -- a few
 * dozen vertices with the state eyes want -- and that drawn across a whole
 * body puts nothing on the screen. Her biggest draw is a main body piece,
 * plain and opaque, so that is the one to stand in on. Which draw that is
 * is learned from the model rather than written down, so it holds for any
 * costume. */
static uint32_t s_anchor_rel, s_anchor_nu;

int ff_model_is_anchor(uint32_t rel, uint32_t nu)
{
    if (nu > s_anchor_nu) {
        s_anchor_nu = nu;
        s_anchor_rel = rel;
        return 1;
    }
    return s_anchor_nu && rel == s_anchor_rel;
}
