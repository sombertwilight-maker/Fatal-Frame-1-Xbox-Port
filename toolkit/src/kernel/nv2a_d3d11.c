/*
 * NV2A on the host GPU: a Direct3D 11 back end for the pushbuffer executor.
 *
 * The executor (nv2a_pb_exec.c) decodes the title's pushbuffer, runs its
 * vertex programs, clips at the near plane, and hands the triangles here with
 * everything the software rasteriser would have used per pixel
 * (nv2a_backend.h, Nv2aGpuBackend). This file draws them with D3D11 instead:
 *
 *   - the register-combiner program (and the texture stage modes ahead of it)
 *     becomes an HLSL pixel shader, generated to match nv2a_combiner.c's
 *     nv2a_rc_eval term for term and compiled once per distinct program;
 *   - colour surfaces become render targets and zeta surfaces depth/stencil
 *     buffers, both keyed by guest address and kept on the GPU;
 *   - textures are decoded from guest memory with the executor's own sampler
 *     (every format it reads, palettes and DXT included) and re-checked once a
 *     frame by a hash of their bytes; a texture whose address is a render
 *     target -- render-to-texture -- binds that render target directly;
 *   - at each flip, the surface being presented is copied back into guest
 *     memory, where the framebuffer window (and everything else that reads
 *     the frame) finds it as before.
 *
 * Everything runs on the executor's thread, which is the only one touching
 * the immediate context.
 *
 * RECOMP_GPU=0 keeps the software rasteriser. RECOMP_GPU_STATS=1 prints a
 * line of counters every few seconds.
 */
#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <objbase.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "nv2a_vsh_interp.h"
#include "video/gfx_config.h"

/* This file is the D3D11 backend and does not include kernel.h; the one
 * kernel function its diagnostics call is declared here, matching the
 * prototype in src/kernel/kernel.h. Calling it implicitly built with older
 * compilers and is an error from GCC 14 on. */
extern unsigned long xbox_GuestCpuOwner(void);  /* holder's tid, 0 when free */

extern ptrdiff_t xbox_GetMemoryOffset(void);

static ID3D11Device        *D;
static ID3D11DeviceContext *C;
static uint32_t s_frame;
static int s_stats = -1;

static struct {
    uint32_t draws, verts, clears, tex_uploads, tex_hits_rt, shaders, readbacks;
    uint32_t vsh_draws, vprogs, const_uploads;
    uint64_t tex_bytes;
} S;

static uint8_t *gmem(void) { return (uint8_t *)xbox_GetMemoryOffset(); }

/* ---- hashing ------------------------------------------------------------ */

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t seed)
{
    /* Four independent 64-bit lanes (xxh64's core loop, simplified): fast
     * enough to re-check every texture once a frame. */
    const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full;
    uint64_t a = seed + P1, b = seed ^ P2, c = seed, d = seed - P1;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        a = (a ^ (w[0] * P2)); a = (a << 31 | a >> 33) * P1;
        b = (b ^ (w[1] * P2)); b = (b << 31 | b >> 33) * P1;
        c = (c ^ (w[2] * P2)); c = (c << 31 | c >> 33) * P1;
        d = (d ^ (w[3] * P2)); d = (d << 31 | d >> 33) * P1;
    }
    a ^= (b << 7 | b >> 57) ^ (c << 12 | c >> 52) ^ (d << 18 | d >> 46);
    for (; i < n; i++)
        a = (a ^ p[i]) * P1;
    a ^= n;
    a ^= a >> 33; a *= P2; a ^= a >> 29;
    return a;
}

static uint64_t hash_u32s(const uint32_t *w, size_t n)
{
    return hash_bytes((const uint8_t *)w, n * 4, 0x51EDu);
}

/* ---- shaders ------------------------------------------------------------ */

static ID3DBlob *compile(const char *src, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "nv2a", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] shader compile failed (%08lX): %s\n",
                (unsigned long)hr, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (getenv("RECOMP_GPU_SHADER_DUMP"))
            fprintf(stderr, "%s\n", src);
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

/* Vertex: everything after the executor's transform. Position is in surface
 * pixels; the shader maps it to clip space keeping w, so the GPU interpolates
 * perspective-correct as the software path does. */
static const char s_vs_src[] =
    "cbuffer V : register(b0) { float4 vp; float4 vx; };\n"
    "struct VI { float4 p : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
    "  float fog : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
    "  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
    "struct PI { float4 p : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
    "  noperspective float fog : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
    "  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
    "PI vs(VI i) { PI o; float w = i.p.w;\n"
    "  float x = i.p.x * vp.x - 1.0, y = 1.0 - i.p.y * vp.y, z = i.p.z * vp.z;\n"
    "  x *= (vx.z != 0 && abs(w - 1.0) > 1e-4) ? vx.z : 1.0;\n"
    "  o.p = float4(x * w, y * w, z * w, w);\n"
    "  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n"
    "  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3; return o; }\n";

/* Clears: a viewport-covering triangle writing a colour and a depth. */
static const char s_clear_src[] =
    "cbuffer K : register(b0) { float4 col; float4 dep; };\n"
    "float4 vs(uint i : SV_VertexID) : SV_Position {\n"
    "  float2 q = float2((i << 1) & 2, i & 2);\n"
    "  return float4(q * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "void ps(float4 p : SV_Position, out float4 c : SV_Target, out float d : SV_Depth)\n"
    "  { c = col; d = dep.x; }\n";

static ID3D11VertexShader *s_vs, *s_clear_vs;
static ID3D11PixelShader  *s_clear_ps;
static ID3D11InputLayout  *s_layout;
static ID3D11Buffer       *s_vcb, *s_pcb, *s_ccb, *s_vb;
static UINT                s_vb_cap, s_vb_pos;    /* vertices */
static ID3D11RasterizerState *s_rs;

/* The pixel shader's constants. */
typedef struct {
    float f0[8][4], f1[8][4];         /* SET_COMBINER_FACTOR0/1 per stage */
    float fc0[4], fc1[4];             /* SET_SPECULAR_FOG_FACTOR 0/1      */
    float fogc[4];                    /* fog colour                       */
    float ts[4][4];                   /* per stage: uv scale xy           */
    int   aref[4];                    /* alpha reference                  */
} PsConst;

/* What a pixel shader depends on; the generated source is a function of
 * this alone. */
typedef struct {
    uint32_t use_rc, textured;
    uint32_t color_icw[8], alpha_icw[8], color_ocw[8], alpha_ocw[8];
    uint32_t final0, final1, control, stage_program, clip_plane_mode;
    uint32_t kind[4];        /* 0 unbound (reads white), 1 2D, 2 cube, 3 shadow map */
    uint32_t shadow_func;    /* SET_SHADOW_COMPARE_FUNC, for kind 3 */
    uint32_t swap_rb[4];     /* texture reads a render target in the other order */
    uint32_t force_a1[4];    /* alphaless format (X8R8G8B8): reads alpha 1, not
                              * the target's leftover alpha -- the bloom pass
                              * alpha-tests what it reads back and vanished */
    uint32_t alpha_test, alpha_func;
    uint32_t dbg;            /* RECOMP_GPU_DEBUG experiments */
} PsKey;

typedef struct { uint64_t h; PsKey k; ID3D11PixelShader *ps; } PsEnt;
static PsEnt *s_ps;
static int s_nps, s_cap_ps;

/* -- HLSL generation, following nv2a_rc_eval (nv2a_combiner.c) -- */

typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        b->cap = b->cap * 2 + (k > 0 ? (size_t)k : 256);
        b->p = (char *)realloc(b->p, b->cap);
    }
}

static const char *REG[16] = {
    "rz", "c0", "c1", "fogr", "v0", "v1", "r6", "r7",
    "t0", "t1", "t2", "t3", "r0", "r1", "sm", "ef"
};

/* map_in: PS_INPUTMAPPING applied to expression x. */
static void emit_map(Buf *b, const char *x, uint32_t byte)
{
    switch ((byte >> 5) & 7) {
    case 0: bput(b, "max(%s,0)", x); break;
    case 1: bput(b, "(1-saturate(%s))", x); break;
    case 2: bput(b, "(2*max(%s,0)-1)", x); break;
    case 3: bput(b, "(-2*max(%s,0)+1)", x); break;
    case 4: bput(b, "(max(%s,0)-0.5)", x); break;
    case 5: bput(b, "(-max(%s,0)+0.5)", x); break;
    case 6: bput(b, "(%s)", x); break;
    default: bput(b, "(-(%s))", x); break;
    }
}

static void emit_rgb(Buf *b, uint32_t byte)
{
    char x[16];
    snprintf(x, sizeof x, "%s.%s", REG[byte & 0xF], (byte & 0x10) ? "aaa" : "rgb");
    emit_map(b, x, byte);
}

static void emit_alpha(Buf *b, uint32_t byte)
{
    char x[16];
    snprintf(x, sizeof x, "%s.%s", REG[byte & 0xF], (byte & 0x10) ? "a" : "b");
    emit_map(b, x, byte);
}

static void emit_mapout(Buf *b, const char *x, uint32_t mapping)
{
    switch (mapping) {
    case 0x08: bput(b, "clamp((%s)-0.5,-1,1)", x); break;
    case 0x10: bput(b, "clamp((%s)*2,-1,1)", x); break;
    case 0x18: bput(b, "clamp(((%s)-0.5)*2,-1,1)", x); break;
    case 0x20: bput(b, "clamp((%s)*4,-1,1)", x); break;
    case 0x30: bput(b, "clamp((%s)*0.5,-1,1)", x); break;
    default:   bput(b, "clamp(%s,-1,1)", x); break;
    }
}

static const char *cmp_op(uint32_t f)
{
    switch (f) {
    case 0x201: return "<";  case 0x202: return "==";
    case 0x203: return "<="; case 0x204: return ">";
    case 0x205: return "!="; case 0x206: return ">=";
    default:    return NULL;
    }
}

static char *gen_ps(const PsKey *k)
{
    Buf b = { (char *)malloc(16384), 0, 16384 };
    int st;

    bput(&b,
         "cbuffer P : register(b0) { float4 F0[8]; float4 F1[8]; float4 FC0; float4 FC1;\n"
         "  float4 FOGC; float4 TS[4]; int4 AREF; };\n"
         );
    for (st = 0; st < 4; st++)
        bput(&b, "%s S%d : register(s%d);\n",
             k->kind[st] == 3 ? "SamplerComparisonState" : "SamplerState", st, st);
    for (st = 0; st < 4; st++)
        bput(&b, "%s T%d : register(t%d);\n",
             k->kind[st] == 2 ? "TextureCube" : "Texture2D", st, st);
    bput(&b,
         "struct PI { float4 p : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
         "  noperspective float fog : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
         "  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
         "float4 ps(PI i) : SV_Target {\n"
         "  float4 o;\n");

    if (!k->use_rc) {
        /* Before the title programs the combiners: stage-0 texel * diffuse,
         * texture coordinates taken as they are (raster_xf_triangle). */
        bput(&b, "  o = i.d0;\n");
        if (k->textured) {
            if (k->kind[0] == 1)
                bput(&b, "  { float4 t = T0.Sample(S0, i.t0.xy * TS[0].xy);%s%s o *= t; }\n",
                     k->swap_rb[0] ? " t.rgb = t.bgr;" : "",
                     k->force_a1[0] ? " t.a = 1;" : "");
        }
        bput(&b, "  o = saturate(o);\n");
    } else {
        uint32_t n = k->control & 0xFF, flags = k->control >> 8, s;
        float dummy = 0; (void)dummy;
        if (n > 8) n = 8;
        bput(&b,
             "  float4 rz = 0, r6 = 0, r7 = 0, r1 = 0, sm = 0, ef = 0, c0, c1;\n"
             "  float4 v0 = i.d0, v1 = i.d1, fogr = float4(FOGC.rgb, i.fog);\n"
             "  float4 t0 = float4(0,0,0,1), t1 = t0, t2 = t0, t3 = t0, shdbg = 0;\n");
        /* Texture stages, by SHADER_STAGE_PROGRAM mode (rc_stage_fetch). */
        for (st = 0; st < 4; st++) {
            uint32_t mode = (k->stage_program >> (st * 5)) & 0x1F;
            const char *tc = st == 0 ? "i.t0" : st == 1 ? "i.t1" : st == 2 ? "i.t2" : "i.t3";
            switch (mode) {
            case 0:
                break;
            case 1: case 2:
                if (!k->kind[st]) { bput(&b, "  t%d = 1;\n", st); break; }
                if (k->kind[st] == 3) {
                    /* Shadow map: the stage's r/q against the stored depth,
                     * r in the depth buffer's own units (TS.z scales them to
                     * 0..1); 1 where the test passes. Bilinear comparison
                     * filtering is the NV2A's own percentage-closer filter. */
                    static int force = -2;
                    if (force == -2) {
                        const char *e = getenv("RECOMP_SHADOW_FORCE");
                        force = e && *e ? atoi(e) : -1;
                    }
                    if (force >= 0)
                        bput(&b, "  t%d = %d;\n", st, force);
                    else if (k->shadow_func == 0)
                        bput(&b, "  t%d = 0;\n", st);
                    else if (k->shadow_func == 7)
                        bput(&b, "  t%d = 1;\n", st);
                    else {
                        /* As xemu compares (pgraph/glsl/psh.c): the stored
                         * depth in the buffer's own units against r/q clamped
                         * to that range, stored <op> r, with the function in
                         * OpenGL's order (6 is GEQUAL: lit where nothing
                         * nearer the light was drawn). Four texels weighted
                         * bilinearly, as a percentage-closer filter.
                         * RECOMP_SHADOW_SWAP=1 swaps the operands. */
                        static const char *ops[8] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
                        static int swap = -1;
                        static double rscale = -1.0;
                        if (swap < 0) {
                            const char *e = getenv("RECOMP_SHADOW_SWAP");
                            swap = e && *e == '1';
                        }
                        if (rscale < 0) {
                            /* Diagnostic scale on r only (not the x/y
                             * lookup); 1.0 is correct -- earlier 2x readings
                             * came from a shadow map corrupted by negative-w
                             * clipping, fixed in the vertex epilogue. */
                            const char *e = getenv("RECOMP_SHADOW_RSCALE");
                            rscale = e && *e ? atof(e) : 1.0;  /* the F9
                                     * pairing probe shows the title renders
                                     * receivers into the map so reference ==
                                     * stored exactly; no scale is right */
                            if (rscale <= 0) rscale = 1.0;
                        }
                        {
                            /* The title asks for +200 Z16 units of polygon
                             * offset on the shadow pass plus a slope term;
                             * applied here on the reference so the compare
                             * carries it even where the rasterizer's bias
                             * cannot (RECOMP_SHADOW_RBIAS, Z16 units). */
                            static double rbias = -1.0;
                            if (rbias < 0) {
                                const char *e2 = getenv("RECOMP_SHADOW_RBIAS");
                                rbias = e2 && *e2 ? atof(e2) : 1000.0;
                                if (rbias < 0) rbias = 0;
                            }
                        bput(&b, "  { float q = %s.w != 0 ? %s.w : 1; float zm = 1.0 / TS[%d].z;\n"
                                 "    float r = clamp(%s.z / q * %f - %f, 0, zm); float2 dim; T%d.GetDimensions(dim.x, dim.y);\n"
                                 "    float2 p = %s.xy / q * TS[%d].xy * dim - 0.5, f = frac(p);\n"
                                 "    int2 i0 = clamp(int2(floor(p)), int2(0,0), int2(dim) - 1);\n"
                                 "    int2 i1 = clamp(int2(floor(p)) + 1, int2(0,0), int2(dim) - 1);\n"
                                 "    float4 d = float4(T%d.Load(int3(i0.x, i0.y, 0)).r, T%d.Load(int3(i1.x, i0.y, 0)).r,\n"
                                 "                      T%d.Load(int3(i0.x, i1.y, 0)).r, T%d.Load(int3(i1.x, i1.y, 0)).r) * zm;\n",
                             tc, tc, st, tc, (float)rscale, (float)rbias, st, tc, st, st, st, st, st);
                        }
                        /* The surface stores whole depth units (Z16 on the
                         * console; 24 bits here) and the hardware compares
                         * in them: a surface tested against its own depth
                         * lands within a unit of it, and comparing a 24-bit
                         * fraction against an unrounded r turns that into a
                         * fine lit/unlit moire. Whole units on both sides,
                         * r rounded down, as the stored value was rounded.
                         * RECOMP_SHADOW_QUANT=0 compares the raw values. */
                        {
                            static int quant = -1;
                            if (quant < 0) {
                                const char *e = getenv("RECOMP_SHADOW_QUANT");
                                quant = e && *e ? atoi(e) : 1;
                            }
                            if (quant == 1)
                                bput(&b, "    d = round(d); r = floor(r);\n");
                            else if (quant >= 2)
                                bput(&b, "    d = round(d); r = floor(r) - %d;\n", quant - 1);
                        }
                        if (!swap)
                            bput(&b, "    float4 c = float4(d.x %s r, d.y %s r, d.z %s r, d.w %s r);\n",
                                 ops[k->shadow_func], ops[k->shadow_func], ops[k->shadow_func], ops[k->shadow_func]);
                        else
                            bput(&b, "    float4 c = float4(r %s d.x, r %s d.y, r %s d.z, r %s d.w);\n",
                                 ops[k->shadow_func], ops[k->shadow_func], ops[k->shadow_func], ops[k->shadow_func]);
                        bput(&b, "    shdbg = float4(r, d.x, %s.z / q, %s.w);\n", tc, tc);
                        bput(&b, "    t%d = lerp(lerp(c.x, c.y, f.x), lerp(c.z, c.w, f.x), f.y); }\n", st);
                    }
                    break;
                }
                bput(&b, "  { float q = %s.w != 0 ? %s.w : 1;\n", tc, tc);
                if (k->kind[st] == 2)
                    bput(&b, "    t%d = T%d.Sample(S%d, float3(%s.xy / q, 1));", st, st, st, tc);
                else
                    bput(&b, "    t%d = T%d.Sample(S%d, %s.xy / q * TS[%d].xy);", st, st, st, tc, st);
                bput(&b, "%s%s }\n", k->swap_rb[st] ? (st == 0 ? " t0.rgb = t0.bgr;" :
                                                     st == 1 ? " t1.rgb = t1.bgr;" :
                                                     st == 2 ? " t2.rgb = t2.bgr;" : " t3.rgb = t3.bgr;") : "",
                     k->force_a1[st] ? (st == 0 ? " t0.a = 1;" : st == 1 ? " t1.a = 1;" :
                                        st == 2 ? " t2.a = 1;" : " t3.a = 1;") : "");
                break;
            case 3:
                if (!k->kind[st]) { bput(&b, "  t%d = 1;\n", st); break; }
                if (k->kind[st] == 2)
                    bput(&b, "  t%d = T%d.Sample(S%d, %s.xyz);\n", st, st, st, tc);
                else
                    bput(&b, "  t%d = T%d.Sample(S%d, %s.xy * TS[%d].xy);\n", st, st, st, tc, st);
                break;
            case 4:
                bput(&b, "  t%d = saturate(%s);\n", st, tc);
                break;
            case 5: {
                int j;
                bput(&b, "  t%d = 0;\n", st);
                for (j = 0; j < 4; j++) {
                    int ge = (k->clip_plane_mode >> (st * 4 + j)) & 1;
                    bput(&b, "  if (%s[%d] %s 0) discard;\n", tc, j, ge ? ">=" : "<");
                }
                break;
            }
            default:
                bput(&b, "  t%d = 0;\n", st);
                break;
            }
        }
        if (k->dbg & 1)
            for (st = 0; st < 4; st++)
                if (!((k->stage_program >> (st * 5)) & 0x1F))
                    bput(&b, "  t%d = 0;\n", st);
        bput(&b, "  float4 r0 = 0; r0.a = %s;\n", (k->stage_program & 0x1F) ? "t0.a" :
             (k->dbg & 2) ? "0" : "1");

        for (s = 0; s < n; s++) {
            uint32_t icw = k->color_icw[s], ocw = k->color_ocw[s];
            uint32_t aicw = k->alpha_icw[s], aocw = k->alpha_ocw[s];
            uint32_t fl = ocw >> 12, afl = aocw >> 12, d;
            bput(&b, "  { c0 = F0[%u]; c1 = F1[%u];\n",
                 (flags & 0x010) ? s : 0, (flags & 0x100) ? s : 0);
            if (flags & 1)
                bput(&b, "    bool mux = r0.a >= 0.5;\n");
            else
                bput(&b, "    bool mux = (((int)(r0.a * 255.0)) & 1) != 0;\n");
            bput(&b, "    float3 A = "); emit_rgb(&b, icw >> 24);
            bput(&b, ", B = ");          emit_rgb(&b, icw >> 16);
            bput(&b, ", C = ");          emit_rgb(&b, icw >> 8);
            bput(&b, ", D = ");          emit_rgb(&b, icw);
            bput(&b, ";\n    float aA = "); emit_alpha(&b, aicw >> 24);
            bput(&b, ", aB = ");            emit_alpha(&b, aicw >> 16);
            bput(&b, ", aC = ");            emit_alpha(&b, aicw >> 8);
            bput(&b, ", aD = ");            emit_alpha(&b, aicw);
            bput(&b, ";\n");
            bput(&b, "    float3 pab = %s, pcd = %s;\n",
                 (fl & 2) ? "dot(A,B).xxx" : "A*B", (fl & 1) ? "dot(C,D).xxx" : "C*D");
            bput(&b, "    float3 psum = %s;\n", (fl & 4) ? "mux ? pcd : pab" : "pab + pcd");
            bput(&b, "    float3 ab = "); emit_mapout(&b, "pab", fl & 0x38);
            bput(&b, ", cd = ");          emit_mapout(&b, "pcd", fl & 0x38);
            bput(&b, ", ms = ");          emit_mapout(&b, "psum", fl & 0x38);
            bput(&b, ";\n    float apab = aA*aB, apcd = aC*aD, apsum = %s;\n",
                 (afl & 4) ? "mux ? apcd : apab" : "apab + apcd");
            bput(&b, "    float aab = "); emit_mapout(&b, "apab", afl & 0x38);
            bput(&b, ", acd = ");         emit_mapout(&b, "apcd", afl & 0x38);
            bput(&b, ", ams = ");         emit_mapout(&b, "apsum", afl & 0x38);
            bput(&b, ";\n");
            /* Writes, in nv2a_rc_eval's order; register 0 stays zero. */
            if ((d = (ocw >> 4) & 0xF) != 0) {
                bput(&b, "    %s.rgb = ab;", REG[d]);
                if (fl & 0x80) bput(&b, " %s.a = ab.b;", REG[d]);
                bput(&b, "\n");
            }
            if ((d = ocw & 0xF) != 0) {
                bput(&b, "    %s.rgb = cd;", REG[d]);
                if (fl & 0x40) bput(&b, " %s.a = cd.b;", REG[d]);
                bput(&b, "\n");
            }
            if ((d = (ocw >> 8) & 0xF) != 0)
                bput(&b, "    %s.rgb = ms;\n", REG[d]);
            if ((d = (aocw >> 4) & 0xF) != 0) bput(&b, "    %s.a = aab;\n", REG[d]);
            if ((d = aocw & 0xF) != 0)        bput(&b, "    %s.a = acd;\n", REG[d]);
            if ((d = (aocw >> 8) & 0xF) != 0) bput(&b, "    %s.a = ams;\n", REG[d]);
            bput(&b, "  }\n");
        }

        if (k->final0 || k->final1) {
            uint32_t f0 = k->final0, f1 = k->final1, ff = f1 & 0xFF;
            bput(&b, "  c0 = FC0; c1 = FC1;\n");
            bput(&b, "  { float3 sa = %s, sb = %s; sm = float4(%s, 0); }\n",
                 (ff & 0x40) ? "1 - v1.rgb" : "v1.rgb",
                 (ff & 0x20) ? "1 - r0.rgb" : "r0.rgb",
                 (ff & 0x80) ? "saturate(sa + sb)" : "sa + sb");
            bput(&b, "  { float3 E = "); emit_rgb(&b, f1 >> 24);
            bput(&b, ", F = ");          emit_rgb(&b, f1 >> 16);
            bput(&b, "; ef = float4(E * F, 0); }\n");
            bput(&b, "  { float3 A = "); emit_rgb(&b, f0 >> 24);
            bput(&b, ", B = ");          emit_rgb(&b, f0 >> 16);
            bput(&b, ", C = ");          emit_rgb(&b, f0 >> 8);
            bput(&b, ", D = ");          emit_rgb(&b, f0);
            bput(&b, ";\n    o.rgb = D + C * (1 - A) + B * A; o.a = ");
            emit_alpha(&b, f1 >> 8);
            bput(&b, "; }\n");
        } else {
            bput(&b, "  o = r0;\n");
        }
        bput(&b, "  o = saturate(o);\n");
        if (k->kind[2] == 3 || k->kind[1] == 3) {
            static int dl = -2;
            if (dl == -2) { const char *e = getenv("RECOMP_DBG_LIGHT"); dl = e ? atoi(e) : 0; }
            if (dl == 1) bput(&b, "  o = float4(1,0,0,1);\n");
            if (dl == 2) bput(&b, "  o = float4(t1.aaa, 1);\n");
            if (dl == 3) bput(&b, "  o = float4(v0.rgb, 1);\n");
            if (dl == 4) bput(&b, "  o = float4(v0.aaa, 1);\n");
            if (dl == 5) bput(&b, "  o = float4(t2.aaa, 1);\n");
            if (dl == 6) bput(&b, "  o = float4(i.fog.xxx, 1);\n");
            if (dl == 9) bput(&b, "  o = float4(saturate((shdbg.x - shdbg.y) / 64 + 0.5), saturate(shdbg.y / 65535), saturate(shdbg.x / 65535), 1);\n");
            if (dl == 10) bput(&b, "  { float q2 = i.t2.w != 0 ? i.t2.w : 1; o = float4(saturate(i.t2.xy / q2 / 256), saturate(i.t2.z / q2 / 65535), 1); }\n");
            if (dl == 11) bput(&b, "  { float q2 = i.t2.w != 0 ? i.t2.w : 1; float rr = i.t2.z / q2; o = float4(saturate(rr / 262144), saturate(rr / 131072), saturate(rr / 524288), 1); }\n");
            /* 12: the law of the mismatch. shdbg = (r as compared, stored d,
             * raw tc.z/q, tc.w); r carries RECOMP_SHADOW_RSCALE, so x2 undoes
             * the current 0.5 and rat is the RAW reference over stored.
             * R: rat/4. G: frac(rat), bands that let the value be read off.
             * B: stored depth. A flat color across a wall receding in depth
             * means a pure constant; a drift with distance means the depth
             * laws differ (one side divided by w, or offset). */
            if (dl == 12) bput(&b, "  { float rat = shdbg.y > 0 ? shdbg.x * 2 / shdbg.y : 0; o = float4(saturate(rat * 0.25), frac(rat), saturate(shdbg.y / 65535), 1); }\n");
            /* 13: the same ratio divided by the texcoord's q. Flat R at 1
             * with B rising along the beam means rat tracks q, confirming a
             * missing or extra w-divide between the two depths. */
            if (dl == 13) bput(&b, "  { float rat = shdbg.y > 0 ? shdbg.x * 2 / shdbg.y : 0; float qq = shdbg.w != 0 ? shdbg.w : 1; o = float4(saturate(rat / qq), frac(rat / qq * 4), saturate(qq / 8), 1); }\n");
            if (dl == 7) bput(&b, "  o = float4(frac(i.t2.xyz / (i.t2.w != 0 ? i.t2.w : 1) * float3(TS[2].xy, TS[2].z)), 1);\n");
            /* 14: the compare's verdict per pixel: green where the shadow
             * test passes (lit), red where it fails, blue the reference. */
            if (dl == 14) bput(&b, "  o = float4(1 - t2.x, t2.x, saturate(shdbg.x / 65535), 1);\n");
            /* 15: the margin. shdbg.y is the sampled stored depth, shdbg.x
             * the compared reference. Green: passes by 0..4096 units (full
             * green = 4096 or more). Red: fails by 0..4096. Blue: frac of
             * margin/512, fine bands to read the value. */
            if (dl == 15) bput(&b, "  { float mg = shdbg.y - shdbg.x; o = float4(saturate(-mg / 4096), saturate(mg / 4096), frac(abs(mg) / 512), 1); }\n");
            if (dl == 8) bput(&b, "  o = float4(T2.SampleLevel(S2l, i.t2.xy / (i.t2.w != 0 ? i.t2.w : 1) * TS[2].xy, 0).rrr, 1);\n");
            /* 16: the three gates of the lit beam at once -- R the cookie
             * (t0 luminance), G the attenuation (t1 alpha), B the shadow
             * compare (t2). Where the beam should be but is black, the dark
             * channel names the culprit. */
            if (dl == 16) bput(&b, "  o = float4(dot(t0.rgb, 0.3333), t1.a, t2.x, 1);\n");
        }
    }
    if (k->alpha_test) {
        static int noat = -1;
        if (noat < 0) {
            const char *e = getenv("RECOMP_BEAM_NOAT");
            noat = e && *e == '1';
        }
        if (noat && k->kind[2] == 3) {
            /* Diagnostic: the beam pass survives without its alpha test. */
        } else {
            const char *op = cmp_op(k->alpha_func);
            if (k->alpha_func == 0x200)
                bput(&b, "  discard;\n");
            else if (op)
                bput(&b, "  if (!((int)(o.a * 255.0 + 0.5) %s AREF.x)) discard;\n", op);
        }
    }
    bput(&b, "  return o;\n}\n");
    return b.p;
}

static ID3D11PixelShader *get_ps(const PsKey *k)
{
    uint64_t h = hash_bytes((const uint8_t *)k, sizeof *k, 7);
    int i;
    char *src;
    ID3DBlob *blob;
    ID3D11PixelShader *ps = NULL;

    for (i = 0; i < s_nps; i++)
        if (s_ps[i].h == h && !memcmp(&s_ps[i].k, k, sizeof *k))
            return s_ps[i].ps;
    src = gen_ps(k);
    if (getenv("RECOMP_GPU_PS_DUMP"))
        fprintf(stderr, "[GPU-D3D11] pixel shader %d: rc %u stages %05X kinds %u%u%u%u control %08X"
                " final %08X %08X\n%s\n", s_nps, k->use_rc, k->stage_program, k->kind[0], k->kind[1],
                k->kind[2], k->kind[3], k->control, k->final0, k->final1, src);
    blob = compile(src, "ps", "ps_4_0");
    if (blob) {
        ID3D11Device_CreatePixelShader(D, ID3D10Blob_GetBufferPointer(blob),
                                       ID3D10Blob_GetBufferSize(blob), NULL, &ps);
        ID3D10Blob_Release(blob);
    }
    free(src);
    if (s_nps == s_cap_ps) {
        s_cap_ps = s_cap_ps ? s_cap_ps * 2 : 64;
        s_ps = (PsEnt *)realloc(s_ps, (size_t)s_cap_ps * sizeof *s_ps);
    }
    s_ps[s_nps].h = h;
    s_ps[s_nps].k = *k;
    s_ps[s_nps].ps = ps;            /* NULL on failure: remembered, not retried */
    s_nps++;
    S.shaders++;
    return ps;
}

/* ---- render targets and depth buffers ----------------------------------- */

/* w, h: the target in the title's pixels; pw, ph: as allocated, times the
 * internal-resolution scale. Everything the title sees (texture coordinates,
 * the vertex mapping) uses w, h; the viewport, scissor, depth buffer and
 * readback use pw, ph. */
typedef struct {
    uint32_t va, w, h, swizzled, pw, ph;
    ID3D11Texture2D *t;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    uint32_t used;
    uint32_t pitch, bpp, dirty;        /* guest layout; drawn since last readback */
    uint32_t gh;                       /* rows the title used since the last readback */
    uint32_t shown_pitch;              /* pitch it was last flipped to the screen with */
    ID3D11Texture2D *stage;            /* 1x staging copy for readback */
    uint64_t gm_hash;                  /* sparse hash of the guest memory under
                                        * the surface when it was last written:
                                        * changed memory means the title reused
                                        * the address for something else */
} RtEnt;

static uint32_t s_scale = 1;             /* internal resolution, applied */

typedef struct {
    uint32_t va, w, h;
    ID3D11Texture2D *t;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;     /* depth as a texture (shadow maps) */
    uint32_t used;
} DsEnt;

#define MAX_RT 64
static RtEnt s_rt[MAX_RT];
static int s_clear_log;    /* mirrors s_dbg.log for code above its declaration */
static int flash_on2(void); /* flash_on, for code above its declaration */
int g_gpu_frame_log;       /* the same, for other files (kernel_hal's vblank marker) */
static DsEnt s_ds[16];

static int make_rt(RtEnt *e, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = w * s_scale; td.Height = h * s_scale; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(D, &td, NULL, &e->t)))
        return 0;
    ID3D11Device_CreateRenderTargetView(D, (ID3D11Resource *)e->t, NULL, &e->rtv);
    ID3D11Device_CreateShaderResourceView(D, (ID3D11Resource *)e->t, NULL, &e->srv);
    e->w = w; e->h = h;
    e->pw = w * s_scale; e->ph = h * s_scale;
    return 1;
}

static void k_reset(void);
/* The byte range a surface's guest-memory hash covers: one rule for every
 * site, or the same bytes hash differently and the compare is noise. */
static uint32_t rt_gm_range(const RtEnt *e)
{
    uint32_t bytes = e->pitch ? e->pitch * e->h : e->w * e->h * 4;
    return bytes > 0x400000u ? 0x400000u : bytes;
}

/* 64 samples across the guest bytes under a surface. */
static uint64_t rt_gm_hash(uint32_t va, uint32_t bytes)
{
    const uint8_t *m = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint64_t h = 1469598103934665603ull;
    uint32_t k, step = bytes > 256 ? bytes / 64 : 4;
    for (k = 0; k + 4 <= bytes; k += step) {
        h ^= *(const uint32_t *)(m + k);
        h *= 1099511628211ull;
    }
    return h;
}

/* Lifecycle trace of one surface (the glow feedback buffer), capped. */
static uint32_t s_trace_va = 0x807F4000u;
static int s_trace_n;
enum { RT_TRACE_CAP = 200 };
#define RT_TRACE(va, ...) do { \
        if ((va) == s_trace_va && s_trace_n < RT_TRACE_CAP) { \
            s_trace_n++; \
            fprintf(stderr, "[RTLIFE] frame %u: ", s_frame); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
        } \
    } while (0)

static void free_rt(RtEnt *e)
{
    k_reset();
    if (e->stage) ID3D11Texture2D_Release(e->stage);
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->rtv) ID3D11RenderTargetView_Release(e->rtv);
    if (e->t) ID3D11Texture2D_Release(e->t);
    memset(e, 0, sizeof *e);
}

/* A new render target starts as what guest memory holds there, so a surface
 * the CPU prepared (or the software path drew into before) carries over. */
static void rt_seed_from_guest(RtEnt *e, const Nv2aGpuState *st)
{
    uint32_t w = e->w, h = e->h, y, x;
    uint32_t *px;
    const uint8_t *src = gmem() + st->color_va;
    if (st->swizzled || !st->pitch || (st->bpp != 4 && st->bpp != 2))
        return;
    if (w * st->bpp > st->pitch)
        w = st->pitch / st->bpp;
    px = (uint32_t *)malloc((size_t)e->w * e->h * 4);
    if (!px)
        return;
    memset(px, 0, (size_t)e->w * e->h * 4);
    for (y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * st->pitch;
        if (st->bpp == 4)
            memcpy(px + (size_t)y * e->w, row, (size_t)w * 4);
        else
            for (x = 0; x < w; x++) {
                uint32_t t = ((const uint16_t *)row)[x];
                px[(size_t)y * e->w + x] = 0xFF000000u | ((t & 0xF800u) << 8)
                    | ((t & 0x07E0u) << 5) | ((t & 0x001Fu) << 3);
            }
    }
    if (s_scale > 1) {
        /* Each texel repeated scale x scale times. */
        uint32_t *big = (uint32_t *)malloc((size_t)e->pw * e->ph * 4), sx;
        if (big) {
            for (y = 0; y < e->ph; y++)
                for (sx = 0; sx < e->pw; sx++)
                    big[(size_t)y * e->pw + sx] = px[(size_t)(y / s_scale) * e->w + sx / s_scale];
            ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)e->t, 0, NULL, big,
                                                  e->pw * 4, 0);
            free(big);
        }
    } else {
        ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)e->t, 0, NULL, px,
                                              e->w * 4, 0);
    }
    free(px);
}

static RtEnt *find_rt(uint32_t va)
{
    int i;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].t && s_rt[i].va == va)
            return &s_rt[i];
    return NULL;
}

static RtEnt *get_rt(const Nv2aGpuState *st)
{
    uint32_t w, h;
    RtEnt *e = find_rt(st->color_va);
    int i, oldest = 0;

    if (st->swizzled) {
        w = st->swz_w; h = st->swz_h;
    } else {
        w = st->bpp ? st->pitch / st->bpp : 0;
        h = st->clip_y + st->clip_h;
        if (w < st->clip_x + st->clip_w) w = st->clip_x + st->clip_w;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return NULL;
    if (e) {
        if (e->w >= w && e->h >= h && e->swizzled == st->swizzled) {
            e->used = s_frame;
            if (e->pitch != st->pitch || e->bpp != st->bpp)
                e->gh = 0;                 /* a different surface now */
            e->pitch = st->pitch; e->bpp = st->bpp;
            if (h > e->gh) e->gh = h;
            return e;
        }
        /* Grown (a taller clip than before): a bigger target, old pixels
         * carried over. */
        {
            RtEnt n;
            memset(&n, 0, sizeof n);
            if (w < e->w) w = e->w;
            if (h < e->h) h = e->h;
            if (!make_rt(&n, w, h))
                return e;
            if (e->swizzled == st->swizzled) {
                D3D11_BOX box = { 0, 0, 0, e->pw, e->ph, 1 };
                ID3D11DeviceContext_CopySubresourceRegion(C, (ID3D11Resource *)n.t, 0, 0, 0, 0,
                                                          (ID3D11Resource *)e->t, 0, &box);
            }
            free_rt(e);
            *e = n;
            e->va = st->color_va;
            e->swizzled = st->swizzled;
            e->used = s_frame;
            e->pitch = st->pitch; e->bpp = st->bpp;
            e->gh = h;
            return e;
        }
    }
    for (i = 0; i < MAX_RT; i++) {
        if (!s_rt[i].t) { oldest = i; break; }
        if (s_rt[i].used < s_rt[oldest].used) oldest = i;
    }
    e = &s_rt[oldest];
    {
        static int nlog;
        if (nlog < 300 && s_frame > 300) {
            nlog++;
            fprintf(stderr, "[RT] frame %u: new target %08X %ux%u%s; evicts %08X (last used frame %u)\n",
                    s_frame, st->color_va, w, h, st->swizzled ? " swizzled" : "",
                    e->t ? e->va : 0, e->t ? e->used : 0);
        }
    }
    if (e->t)
        free_rt(e);
    if (!make_rt(e, w, h))
        return NULL;
    e->va = st->color_va;
    e->swizzled = st->swizzled;
    e->used = s_frame;
    e->pitch = st->pitch; e->bpp = st->bpp;
    e->gh = h;
    rt_seed_from_guest(e, st);
    return e;
}

static DsEnt *find_ds(uint32_t va)
{
    int i;
    for (i = 0; i < 16; i++)
        if (s_ds[i].t && s_ds[i].va == va)
            return &s_ds[i];
    return NULL;
}

static DsEnt *get_ds(uint32_t va, uint32_t w, uint32_t h)
{
    int i, slot = -1, oldest = 0;
    D3D11_TEXTURE2D_DESC td;

    for (i = 0; i < 16; i++) {
        if (s_ds[i].t && s_ds[i].va == va) {
            if (s_ds[i].w >= w && s_ds[i].h >= h) {
                s_ds[i].used = s_frame;
                return &s_ds[i];
            }
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (i = 0; i < 16; i++) {
            if (!s_ds[i].t) { oldest = i; break; }
            if (s_ds[i].used < s_ds[oldest].used) oldest = i;
        }
        slot = oldest;
    }
    if (s_ds[slot].t) {
        if (w < s_ds[slot].w) w = s_ds[slot].w;
        if (h < s_ds[slot].h) h = s_ds[slot].h;
        k_reset();
        ID3D11DepthStencilView_Release(s_ds[slot].dsv);
        if (s_ds[slot].srv) ID3D11ShaderResourceView_Release(s_ds[slot].srv);
        ID3D11Texture2D_Release(s_ds[slot].t);
    }
    memset(&s_ds[slot], 0, sizeof s_ds[slot]);
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(D, &td, NULL, &s_ds[slot].t)))
        return NULL;
    {
        /* Typeless, so the depth can also be read as a texture: Fatal
         * Frame's flashlight is a shadow-mapped spot light, its shadow map a
         * depth-only pass sampled with SET_SHADOW_COMPARE_FUNC. */
        D3D11_DEPTH_STENCIL_VIEW_DESC dv;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv;
        memset(&dv, 0, sizeof dv);
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ID3D11Device_CreateDepthStencilView(D, (ID3D11Resource *)s_ds[slot].t, &dv, &s_ds[slot].dsv);
        memset(&sv, 0, sizeof sv);
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(D, (ID3D11Resource *)s_ds[slot].t, &sv,
                                                         &s_ds[slot].srv)))
            s_ds[slot].srv = NULL;
    }
    s_ds[slot].va = va; s_ds[slot].w = w; s_ds[slot].h = h; s_ds[slot].used = s_frame;
    /* Fresh depth: cleared to the far plane and stencil 0, as a title that
     * never clears would find the CPU path's buffer. */
    ID3D11DeviceContext_ClearDepthStencilView(C, s_ds[slot].dsv,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    return &s_ds[slot];
}

/* ---- textures ----------------------------------------------------------- */

typedef struct {
    uint32_t va, fmt, w, h, pitch, palette, cube, bytes;
    uint64_t hash, shash;             /* whole texture; sampled (tex_sparse_hash) */
    uint32_t checked, used;
    uint32_t tw, th;                  /* as created: a replacement may differ */
    ID3D11Texture2D *t;
    ID3D11ShaderResourceView *srv;
} TexEnt;

#define TEX_SLOTS 1024
static TexEnt s_tex[TEX_SLOTS];
static uint32_t *s_decode;
static size_t s_decode_cap;

static int tex_same(const TexEnt *e, const Nv2aGpuTexture *t)
{
    return e->t && e->va == t->offset && e->fmt == t->color && e->w == t->width
        && e->h == t->height && e->pitch == t->pitch && e->palette == t->palette
        && e->cube == t->cube;
}

static uint64_t tex_content_hash(const Nv2aGpuTexture *t)
{
    uint64_t h = hash_bytes(gmem() + t->offset, t->bytes, t->color);
    if (t->palette && t->color == 0x0B)
        h ^= hash_bytes(gmem() + t->palette, 1024, 0xC1u);
    return h;
}

/* ---- texture dump and replacement (F6 menu) -------------------------------
 *
 * Every texture the title uploads has a name: the hash of its bytes (and
 * palette), its size and its NV2A format -- the same name wherever in memory
 * and whenever it is loaded. Dumping writes each one once to
 * textures\dump\<name>.png; replacing uses textures\replace\<name>.png
 * instead, at any resolution (coordinates are normalised, so a 4x larger
 * replacement simply has 4x the detail). PNG through WIC, which every Windows
 * has. */
static IWICImagingFactory *s_wic;

static int wic_ready(void)
{
    static int tried;
    static const GUID clsid = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
    static const GUID iid = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
    if (!tried) {
        tried = 1;
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &iid, (void **)&s_wic)))
            s_wic = NULL;
        if (!s_wic)
            fprintf(stderr, "[GPU-D3D11] WIC unavailable: no texture dump/replace\n");
    }
    return s_wic != NULL;
}

static const GUID k_png = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID k_bgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

static int png_write_rows(const wchar_t *path, const uint32_t *px, uint32_t w, uint32_t h);
static int png_write(const wchar_t *path, const uint32_t *px, uint32_t w, uint32_t h)
{
    /* This title stores texture rows bottom to top: flipped, the dump opens
     * the right way up, and png_read flips a replacement back. */
    uint32_t *f = (uint32_t *)malloc((size_t)w * h * 4), y;
    int ok;
    if (!f) return 0;
    for (y = 0; y < h; y++)
        memcpy(f + (size_t)y * w, px + (size_t)(h - 1 - y) * w, (size_t)w * 4);
    ok = png_write_rows(path, f, w, h);
    free(f);
    return ok;
}

static int png_write_rows(const wchar_t *path, const uint32_t *px, uint32_t w, uint32_t h)
{
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    WICPixelFormatGUID fmt = k_bgra;
    int ok = 0;

    if (!wic_ready()) return 0;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(s_wic, &st))
        && SUCCEEDED(IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE))
        && SUCCEEDED(IWICImagingFactory_CreateEncoder(s_wic, &k_png, NULL, &enc))
        && SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache))
        && SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props))
        && SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, props))
        && SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, w, h))
        && SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt))
        && SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, h, w * 4, w * h * 4, (BYTE *)px))
        && SUCCEEDED(IWICBitmapFrameEncode_Commit(fr))
        && SUCCEEDED(IWICBitmapEncoder_Commit(enc)))
        ok = 1;
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (props) IPropertyBag2_Release(props);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    return ok;
}

static uint32_t *png_read(const wchar_t *path, uint32_t *w, uint32_t *h)
{
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    uint32_t *px = NULL;
    UINT ww = 0, hh = 0;

    if (!wic_ready()) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(s_wic, path, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))
        && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(s_wic, &cv))
        && SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &k_bgra,
                                                    WICBitmapDitherTypeNone, NULL, 0,
                                                    WICBitmapPaletteTypeCustom))
        && SUCCEEDED(IWICFormatConverter_GetSize(cv, &ww, &hh))
        && ww && hh && ww <= 16384 && hh <= 16384) {
        px = (uint32_t *)malloc((size_t)ww * hh * 4);
        if (px && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, ww * 4, ww * hh * 4, (BYTE *)px))) {
            free(px);
            px = NULL;
        }
        if (px) {                         /* back to the title's row order */
            uint32_t y, *row = (uint32_t *)malloc((size_t)ww * 4);
            for (y = 0; row && y < hh / 2; y++) {
                memcpy(row, px + (size_t)y * ww, (size_t)ww * 4);
                memcpy(px + (size_t)y * ww, px + (size_t)(hh - 1 - y) * ww, (size_t)ww * 4);
                memcpy(px + (size_t)(hh - 1 - y) * ww, row, (size_t)ww * 4);
            }
            free(row);
        }
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    *w = ww; *h = hh;
    return px;
}

static void tex_name(wchar_t *out, size_t n, const wchar_t *dir, const TexEnt *e)
{
    _snwprintf(out, n, L"textures\\%ls\\%016llX_%ux%u_%02X.png", dir,
               (unsigned long long)e->hash, e->w, e->h, e->fmt);
    out[n - 1] = 0;
}

/* The replacement folder's names, by hash, read once per reload. */
static uint64_t *s_repl;
static int s_nrepl;
/* Hashes already dumped this run (and on disk from earlier runs). */
static uint64_t *s_dumped;
static int s_ndumped, s_cap_dumped;

static int hash_in(const uint64_t *v, int n, uint64_t h)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (v[mid] == h) return 1;
        if (v[mid] < h) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int scan_dir(const char *pattern, uint64_t **out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int n = 0, cap = 0;
    uint64_t *v = NULL;
    if (h == INVALID_HANDLE_VALUE) { *out = NULL; return 0; }
    do {
        unsigned long long x;
        if (sscanf(fd.cFileName, "%16llX", &x) == 1) {
            if (n == cap) { cap = cap ? cap * 2 : 256; v = (uint64_t *)realloc(v, cap * sizeof *v); }
            v[n++] = x;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(v, n, sizeof *v, cmp_u64);
    *out = v;
    return n;
}

static void repl_scan(void)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    free(s_repl);
    s_nrepl = scan_dir("textures\\replace\\*.png", &s_repl);
    g->replacements_found = s_nrepl;
    g->replaced = 0;
}

static void dump_texture(TexEnt *e, const uint32_t *px)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    wchar_t path[MAX_PATH];
    static int dirs, scanned;
    if (!scanned) {
        scanned = 1;
        s_ndumped = scan_dir("textures\\dump\\*.png", &s_dumped);
        s_cap_dumped = s_ndumped;
    }
    if (hash_in(s_dumped, s_ndumped, e->hash))
        return;
    if (!dirs) { dirs = 1; CreateDirectoryA("textures", NULL); CreateDirectoryA("textures\\dump", NULL); }
    tex_name(path, MAX_PATH, L"dump", e);
    if (png_write(path, px, e->w, e->h))
        g->dumped++;
    /* Remembered either way: a failed write is not retried every frame. */
    if (s_ndumped == s_cap_dumped) {
        s_cap_dumped = s_cap_dumped ? s_cap_dumped * 2 : 256;
        s_dumped = (uint64_t *)realloc(s_dumped, s_cap_dumped * sizeof *s_dumped);
    }
    {
        int k = s_ndumped++;
        while (k > 0 && s_dumped[k - 1] > e->hash) { s_dumped[k] = s_dumped[k - 1]; k--; }
        s_dumped[k] = e->hash;
    }
}

static int tex_upload(TexEnt *e, const Nv2aGpuTexture *t)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    size_t n = (size_t)t->width * t->height;
    uint32_t faces = t->cube ? 6 : 1, f, tw = t->width, th = t->height;
    D3D11_SUBRESOURCE_DATA sd[6];
    D3D11_TEXTURE2D_DESC td;
    const uint32_t *px;
    uint32_t *repl = NULL;

    if (n * faces > s_decode_cap) {
        free(s_decode);
        s_decode_cap = n * faces;
        s_decode = (uint32_t *)malloc(s_decode_cap * 4);
        if (!s_decode) { s_decode_cap = 0; return 0; }
    }
    for (f = 0; f < faces; f++)
        if (!nv2a_gpu_decode_texture(t, f, s_decode + n * f)) {
            static int nd;
            if (nd++ < 50)
                fprintf(stderr, "[TEXMISS] decode failed: tex %08X fmt %02X %ux%u\n", t->offset, t->color, t->width, t->height);
            return 0;
        }
    S.tex_uploads++;
    S.tex_bytes += n * faces * 4;
    px = s_decode;
    if (!t->cube) {
        /* First upload only: a texture rewritten every frame (a movie, a
         * scrolling effect) is saved once, not once a frame. */
        if (g->tex_dump && !e->t)
            dump_texture(e, s_decode);
        if (g->tex_replace && hash_in(s_repl, s_nrepl, e->hash)) {
            wchar_t path[MAX_PATH];
            uint32_t rw, rh;
            tex_name(path, MAX_PATH, L"replace", e);
            repl = png_read(path, &rw, &rh);
            if (repl) {
                px = repl; tw = rw; th = rh;
                g->replaced++;
            }
        }
    }
    if (e->t && e->tw == tw && e->th == th) {
        /* Same shape, new content: update in place. */
        for (f = 0; f < faces; f++)
            ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)e->t, f, NULL,
                                                  px + (size_t)tw * th * f, tw * 4, 0);
        free(repl);
        return 1;
    }
    if (e->t) {
        k_reset();
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        ID3D11Texture2D_Release(e->t);
        e->t = NULL; e->srv = NULL;
    }
    memset(&td, 0, sizeof td);
    td.Width = tw; td.Height = th; td.MipLevels = 1;
    td.ArraySize = faces;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = t->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    for (f = 0; f < faces; f++) {
        sd[f].pSysMem = px + (size_t)tw * th * f;
        sd[f].SysMemPitch = tw * 4;
        sd[f].SysMemSlicePitch = 0;
    }
    if (FAILED(ID3D11Device_CreateTexture2D(D, &td, sd, &e->t))) {
        static int nc;
        if (nc++ < 50)
            fprintf(stderr, "[TEXMISS] CreateTexture2D failed: %ux%u faces %u\n", tw, th, faces);
        e->t = NULL;
        free(repl);
        return 0;
    }
    ID3D11Device_CreateShaderResourceView(D, (ID3D11Resource *)e->t, NULL, &e->srv);
    e->tw = tw; e->th = th;
    free(repl);
    return 1;
}

static void tex_cache_clear(void)
{
    int i;
    k_reset();
    for (i = 0; i < TEX_SLOTS; i++) {
        if (s_tex[i].srv) ID3D11ShaderResourceView_Release(s_tex[i].srv);
        if (s_tex[i].t) ID3D11Texture2D_Release(s_tex[i].t);
        memset(&s_tex[i], 0, sizeof s_tex[i]);
    }
}

/* 64 spread 32-byte pieces of the texture (and its palette): what the
 * per-frame check reads. The whole texture is hashed when they change, and
 * anyway once every 16 frames, staggered by slot, so an edit between the
 * samples is picked up within a quarter of a second. Hashing every texture
 * in full every frame was megabytes a frame on the engine thread. */
static uint64_t tex_sparse_hash(const Nv2aGpuTexture *t)
{
    const uint8_t *p = gmem() + t->offset;
    size_t n = t->bytes, step, k;
    uint64_t h = t->color;
    if (n <= 4096)
        h = hash_bytes(p, n, h);
    else {
        step = (n - 32) / 63;
        for (k = 0; k < 64; k++)
            h = hash_bytes(p + k * step, 32, h);
    }
    if (t->palette && t->color == 0x0B)
        h ^= hash_bytes(gmem() + t->palette, 1024, 0xC1u);
    return h;
}

static ID3D11ShaderResourceView *get_tex(const Nv2aGpuTexture *t)
{
    uint32_t h0 = (t->offset >> 6) ^ (t->offset >> 17) ^ (t->color * 131u) ^ (t->width << 3);
    uint32_t i, slot = TEX_SLOTS, oldest = TEX_SLOTS;

    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    for (i = 0; i < 16; i++) {
        uint32_t k = (h0 + i) & (TEX_SLOTS - 1);
        if (tex_same(&s_tex[k], t)) { slot = k; break; }
        if (!s_tex[k].t && slot == TEX_SLOTS) slot = k;
        if (oldest == TEX_SLOTS || s_tex[k].used < s_tex[oldest].used) oldest = k;
    }
    if (slot == TEX_SLOTS)
        slot = oldest;
    {
        TexEnt *e = &s_tex[slot];
        if (!tex_same(e, t)) {
            k_reset();
            if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
            if (e->t) ID3D11Texture2D_Release(e->t);
            memset(e, 0, sizeof *e);
            e->va = t->offset; e->fmt = t->color; e->w = t->width; e->h = t->height;
            e->pitch = t->pitch; e->palette = t->palette; e->cube = t->cube;
            e->bytes = t->bytes;
            e->hash = tex_content_hash(t);
            e->shash = tex_sparse_hash(t);
            e->checked = s_frame;
            if (!tex_upload(e, t)) {
                if (e->t) ID3D11Texture2D_Release(e->t);
                memset(e, 0, sizeof *e);
                return NULL;
            }
        } else if (e->checked != s_frame) {
            uint64_t sh = tex_sparse_hash(t);
            e->checked = s_frame;
            if (sh != e->shash || ((s_frame + slot) & 15) == 0) {
                uint64_t h = tex_content_hash(t);
                e->shash = sh;
                if (h != e->hash) {
                    e->hash = h;
                    if (s_clear_log || flash_on2()) {
                        /* how much of the texture is 0xFF / 0x00: a glyph
                         * atlas caught between its clear and its glyphs
                         * shows up as nearly all one value */
                        const uint8_t *tp = gmem() + t->offset;
                        uint32_t nb = t->bytes < 4096 ? t->bytes : 4096, k2, nff = 0, n00 = 0;
                        for (k2 = 0; k2 < nb; k2++) {
                            if (tp[k2] == 0xFF) nff++;
                            else if (tp[k2] == 0) n00++;
                        }
                        fprintf(stderr, "  [TEXCHG] frame %u tex %08X %ux%u changed; first4k ff %u%% 00 %u%% tid-last %lu\n",
                                s_frame, t->offset, t->width, t->height,
                                nff * 100u / nb, n00 * 100u / nb,
                                (unsigned long)xbox_GuestCpuOwner());
                    }
                    tex_upload(e, t);
                }
            }
        }
        e->used = s_frame;
        return e->srv;
    }
}

/* ---- fixed-function state ------------------------------------------------ */

static D3D11_BLEND blend_of(uint32_t f, int alpha)
{
    switch (f) {
    case 0:      return D3D11_BLEND_ZERO;
    case 1:      return D3D11_BLEND_ONE;
    case 0x300:  return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301:  return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:  return D3D11_BLEND_SRC_ALPHA;
    case 0x303:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:  return D3D11_BLEND_DEST_ALPHA;
    case 0x305:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:  return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307:  return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:  return D3D11_BLEND_SRC_ALPHA_SAT;   /* source only: see get_bs */
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static UINT8 write_mask(uint32_t cm)
{
    UINT8 m = 0;
    if (cm & 0x00010000u) m |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (cm & 0x00000100u) m |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (cm & 0x00000001u) m |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (cm & 0x01000000u) m |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return m;
}

typedef struct { uint64_t key; void *obj; } StateEnt;
static StateEnt s_bs[256], s_dss[256], s_ss[256];
static int s_nbs, s_ndss, s_nss;

static void *state_find(StateEnt *t, int n, uint64_t key)
{
    int i;
    for (i = 0; i < n; i++)
        if (t[i].key == key)
            return t[i].obj;
    return NULL;
}

static void state_add(StateEnt *t, int *n, uint64_t key, void *obj)
{
    if (*n < 256) { t[*n].key = key; t[*n].obj = obj; (*n)++; }
}

static ID3D11BlendState *get_bs(int enable, uint32_t src, uint32_t dst, uint32_t eq, UINT8 mask)
{
    uint64_t key = ((uint64_t)enable << 63) | ((uint64_t)(src & 0xFFFF) << 40)
                 | ((uint64_t)(dst & 0xFFFF) << 24) | ((uint64_t)(eq & 0xFFFF) << 8) | mask;
    ID3D11BlendState *bs = (ID3D11BlendState *)state_find(s_bs, s_nbs, key);
    D3D11_BLEND_DESC d;
    if (bs) return bs;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = enable;
    d.RenderTarget[0].SrcBlend = blend_of(src, 0);
    d.RenderTarget[0].DestBlend = blend_of(dst, 0);
    d.RenderTarget[0].BlendOp = blend_op(eq);
    d.RenderTarget[0].SrcBlendAlpha = blend_of(src, 1);
    d.RenderTarget[0].DestBlendAlpha = blend_of(dst, 1);
    if (dst == 0x308)                 /* invalid as a destination factor */
        d.RenderTarget[0].DestBlend = d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[0].BlendOpAlpha = blend_op(eq);
    d.RenderTarget[0].RenderTargetWriteMask = mask;
    if (FAILED(ID3D11Device_CreateBlendState(D, &d, &bs)))
        return NULL;
    state_add(s_bs, &s_nbs, key, bs);
    return bs;
}

static D3D11_COMPARISON_FUNC cmp_of(uint32_t f)
{
    if (f >= 0x200 && f <= 0x207)
        return (D3D11_COMPARISON_FUNC)(f - 0x200 + 1);
    return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP sop_of(uint32_t op)
{
    switch (op) {
    case 0:      return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11DepthStencilState *get_dss(const D3D11_DEPTH_STENCIL_DESC *d)
{
    uint64_t key = hash_bytes((const uint8_t *)d, sizeof *d, 3);
    ID3D11DepthStencilState *s = (ID3D11DepthStencilState *)state_find(s_dss, s_ndss, key);
    if (s) return s;
    if (FAILED(ID3D11Device_CreateDepthStencilState(D, d, &s)))
        return NULL;
    state_add(s_dss, &s_ndss, key, s);
    return s;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_of(uint32_t m)
{
    switch (m) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static ID3D11SamplerState *get_ss(int linear, uint32_t au, uint32_t av, uint32_t border,
                                  int border_on)
{
    uint64_t key;
    /* BORDER (4) reads the border colour only when FORMAT's border source
     * says colour; otherwise the hardware -- and the software path's
     * wrap_coord -- clamp to the edge texel. A transparent-black border there
     * painted the floor's shadow and light maps black past their edges. */
    if (!border_on) {
        if (au == 4) au = 3;
        if (av == 4) av = 3;
        border = 0;
    }
    key = ((uint64_t)linear << 40) | ((uint64_t)(au & 0xF) << 36)
        | ((uint64_t)(av & 0xF) << 32) | border;
    ID3D11SamplerState *s = (ID3D11SamplerState *)state_find(s_ss, s_nss, key);
    D3D11_SAMPLER_DESC d;
    if (s) return s;
    memset(&d, 0, sizeof d);
    d.Filter = linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = addr_of(au);
    d.AddressV = addr_of(av);
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.BorderColor[0] = (float)((border >> 16) & 0xFF) / 255.0f;
    d.BorderColor[1] = (float)((border >> 8) & 0xFF) / 255.0f;
    d.BorderColor[2] = (float)(border & 0xFF) / 255.0f;
    d.BorderColor[3] = (float)(border >> 24) / 255.0f;
    if (FAILED(ID3D11Device_CreateSamplerState(D, &d, &s)))
        return NULL;
    state_add(s_ss, &s_nss, key, s);
    return s;
}

/* A comparison sampler for a shadow map: clamped, point or bilinear (PCF),
 * comparing the reference (r/q) against the stored depth with the NV2A's
 * SET_SHADOW_COMPARE_FUNC. RECOMP_SHADOW_SWAP=1 compares the other way round
 * (stored depth against r). (Fatal Frame port.) */
static ID3D11SamplerState *get_ss_cmp(int linear, uint32_t func)
{
    static ID3D11SamplerState *cache[2][8];
    static int swap = -1;
    /* NV: 1 GREATER 2 EQUAL 3 GEQUAL 4 LESS 5 NOTEQUAL 6 LEQUAL */
    static const D3D11_COMPARISON_FUNC fwd[8] = {
        D3D11_COMPARISON_NEVER, D3D11_COMPARISON_GREATER, D3D11_COMPARISON_EQUAL,
        D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_LESS, D3D11_COMPARISON_NOT_EQUAL,
        D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_ALWAYS };
    static const D3D11_COMPARISON_FUNC rev[8] = {
        D3D11_COMPARISON_NEVER, D3D11_COMPARISON_LESS, D3D11_COMPARISON_EQUAL,
        D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_GREATER, D3D11_COMPARISON_NOT_EQUAL,
        D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_ALWAYS };
    D3D11_SAMPLER_DESC d;
    ID3D11SamplerState *s;

    linear = linear ? 1 : 0;
    func &= 7;
    if (cache[linear][func])
        return cache[linear][func];
    if (swap < 0) {
        const char *e = getenv("RECOMP_SHADOW_SWAP");
        swap = e && *e == '1';
    }
    memset(&d, 0, sizeof d);
    d.Filter = linear ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                      : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    d.ComparisonFunc = swap ? rev[func] : fwd[func];
    if (FAILED(ID3D11Device_CreateSamplerState(D, &d, &s)))
        return NULL;
    cache[linear][func] = s;
    return s;
}

static void argb_to_f4(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

/* ---- binding a surface -------------------------------------------------- */

static RtEnt *s_cur_rt;

/* What is bound now, so a draw that changes nothing re-sends nothing: the
 * club scenes issue thousands of draws a frame, most of them with the
 * previous draw's target, states and shaders. Reset whenever an object that
 * could be bound is freed (its address may come back as a new object). */
static struct {
    ID3D11RenderTargetView *rtv; ID3D11DepthStencilView *dsv;
    D3D11_VIEWPORT vp; D3D11_RECT sc;
    ID3D11DepthStencilState *dss; UINT sref;
    ID3D11BlendState *bs; float bf[4];
    ID3D11PixelShader *ps; ID3D11VertexShader *vs; ID3D11InputLayout *il;
    ID3D11ShaderResourceView *srv[4]; ID3D11SamplerState *smp[4];
    ID3D11Buffer *vb; UINT vstride; ID3D11Buffer *ib;
    ID3D11RasterizerState *rs_ptr;
    int valid, rs, topo, vcb_n, pcb, cb_vsh;
    float vcb[12];
    unsigned char pc[1024];
} K;
static void k_reset(void) { memset(&K, 0, sizeof K); }
/* Points (Fatal Frame's visibility tests draw one) and lines go to D3D11
 * as they are: one-pixel points, one-pixel lines. K.topo is topology + 1. */
static void set_topo(uint32_t topo)
{
    if (K.topo == (int)topo + 1)
        return;
    ID3D11DeviceContext_IASetPrimitiveTopology(C, topo == 1 ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                                               : topo == 2 ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                               : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    K.topo = (int)topo + 1;
}
static void k_targets(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv)
{
    if (K.valid && K.rtv == rtv && K.dsv == dsv) return;
    ID3D11DeviceContext_OMSetRenderTargets(C, 1, &rtv, dsv);
    /* Binding a target unbinds any view of it from the shader stages,
     * behind our back: forget what we think is bound there. */
    memset(K.srv, 0xFF, sizeof K.srv);
    K.rtv = rtv; K.dsv = dsv; K.valid = 1;
}
static void k_view(const D3D11_VIEWPORT *vp, const D3D11_RECT *sc)
{
    if (!K.valid || memcmp(&K.vp, vp, sizeof *vp)) {
        ID3D11DeviceContext_RSSetViewports(C, 1, vp); K.vp = *vp;
    }
    if (!K.valid || memcmp(&K.sc, sc, sizeof *sc)) {
        ID3D11DeviceContext_RSSetScissorRects(C, 1, sc); K.sc = *sc;
    }
}
static void k_vcb(const float *d)
{
    if (K.valid && K.vcb_n && !memcmp(K.vcb, d, sizeof K.vcb)) return;
    ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_vcb, 0, NULL, d, 0, 0);
    memcpy(K.vcb, d, sizeof K.vcb); K.vcb_n = 1;
}
static void k_dss(ID3D11DepthStencilState *d, UINT ref)
{
    if (K.valid && K.dss == d && K.sref == ref) return;
    ID3D11DeviceContext_OMSetDepthStencilState(C, d, ref); K.dss = d; K.sref = ref;
}
static void k_bs(ID3D11BlendState *b, const float bf[4])
{
    if (K.valid && K.bs == b && !memcmp(K.bf, bf, sizeof K.bf)) return;
    ID3D11DeviceContext_OMSetBlendState(C, b, bf, 0xFFFFFFFF);
    K.bs = b; memcpy(K.bf, bf, sizeof K.bf);
}
/* The vertex shaders' b0: vp (2/W, 2/H, 1/zmax, -), then for translated
 * programs (z offset, clip at it, -, -) and fog (enable, mode, p0, p1). */
static float s_vcb_data[12];

/* Bind the colour target (and depth, if `want_depth`) for state `st`; sets
 * the viewport to the whole target and the scissor to rect. */
static int bind_surface(const Nv2aGpuState *st, int want_depth, const uint32_t rect[4],
                        DsEnt **ds_out)
{
    RtEnt *rt = get_rt(st);
    DsEnt *ds = NULL;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;

    if (!rt) {
        extern void nv2a_drop_note(int, const char *, uint32_t, uint32_t);
        nv2a_drop_note(11, "d3d11: no render target for the colour surface, draw dropped", st->color_va, st->pitch);
        return 0;
    }
    if (want_depth && st->zeta_va)
        ds = get_ds(st->zeta_va, rt->pw, rt->ph);
    k_targets(rt->rtv, ds ? ds->dsv : NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)rt->pw; vp.Height = (float)rt->ph;
    vp.MinDepth = 0; vp.MaxDepth = 1;

    sc.left = (LONG)(rect[0] * s_scale); sc.top = (LONG)(rect[1] * s_scale);
    sc.right = (LONG)((rect[2] < rt->w ? rect[2] : rt->w) * s_scale);
    sc.bottom = (LONG)((rect[3] < rt->h ? rect[3] : rt->h) * s_scale);
    k_view(&vp, &sc);
    memset(s_vcb_data, 0, sizeof s_vcb_data);
    s_vcb_data[0] = 2.0f / (float)rt->w;
    s_vcb_data[1] = 2.0f / (float)rt->h;
    s_vcb_data[2] = 1.0f / (st->zeta_bits == 16 ? 65535.0f : 16777215.0f);
    /* the widescreen factor survives the wipe: every path reads it */
    s_vcb_data[6] = xbox_GfxConfig()->widescreen ? 0.75f : 0.0f;
    k_vcb(s_vcb_data);
    s_cur_rt = rt;
    if (ds_out) *ds_out = ds;
    return 1;
}

/* ---- the back end ------------------------------------------------------- */

static void d3d_clear(const Nv2aGpuState *st, uint32_t flags, uint32_t argb,
                      uint32_t zs, const uint32_t rect[4])
{
    DsEnt *ds = NULL;
    float k[8];
    D3D11_DEPTH_STENCIL_DESC dd;
    UINT8 mask = 0;
    int z = (flags & 1) != 0, s = (flags & 2) != 0;

    if (!bind_surface(st, z || s, rect, &ds))
        return;
    S.clears++;
    if (s_clear_log)
        fprintf(stderr, "  [CLEAR] rt %08X flags %02X argb %08X zs %08X rect %u,%u..%u,%u\n",
                st->color_va, flags, argb, zs, rect[0], rect[1], rect[2], rect[3]);
    if ((flags & 0xF0) && s_cur_rt)
        s_cur_rt->dirty = 1;
    if (flags & 0x10) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (flags & 0x20) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (flags & 0x40) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (flags & 0x80) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    argb_to_f4(argb, k);
    k[4] = st->zeta_bits == 16 ? (float)(zs & 0xFFFF) / 65535.0f
                               : (float)(zs >> 8) / 16777215.0f;
    k[5] = k[6] = k[7] = 0;
    ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_ccb, 0, NULL, k, 0, 0);
    memset(&dd, 0, sizeof dd);
    if (ds && z) {
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    }
    if (ds && s) {
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = 0xFF;
        dd.StencilWriteMask = 0xFF;
        dd.FrontFace.StencilFunc = dd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dd.FrontFace.StencilPassOp = dd.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        dd.FrontFace.StencilFailOp = dd.BackFace.StencilFailOp = D3D11_STENCIL_OP_REPLACE;
        dd.FrontFace.StencilDepthFailOp = dd.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE;
    }
    if (!dd.DepthEnable) {
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    }
    ID3D11DeviceContext_OMSetDepthStencilState(C, get_dss(&dd), zs & 0xFF);
    ID3D11DeviceContext_OMSetBlendState(C, get_bs(0, 1, 0, 0x8006, mask), NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_IASetInputLayout(C, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(C, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); K.topo = 1;
    ID3D11DeviceContext_VSSetShader(C, s_clear_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(C, s_clear_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(C, 0, 1, &s_ccb);
    ID3D11DeviceContext_RSSetState(C, s_rs); K.rs_ptr = s_rs;
    ID3D11DeviceContext_Draw(C, 3, 0);
    {   /* the clear bound its own shaders, states and buffers */
        ID3D11RenderTargetView *rtv = K.rtv; ID3D11DepthStencilView *dsv = K.dsv;
        D3D11_VIEWPORT vp = K.vp; D3D11_RECT sc = K.sc; float vcb[12];
        memcpy(vcb, K.vcb, sizeof vcb);
        k_reset();
        K.rtv = rtv; K.dsv = dsv; K.vp = vp; K.sc = sc; memcpy(K.vcb, vcb, sizeof vcb);
        K.vcb_n = 1; K.valid = 1;
    }
}

/* Everything a draw needs but its vertices: target, depth, blend, pixel
 * shader, textures. Returns 0 if the batch cannot be drawn. */

/* ---- draw debugging ---------------------------------------------------------
 * RECOMP_GPU_DEBUG=1: gpudbg.txt beside the exe is re-read when it changes.
 *   skip LO HI   -- draws LO..HI-1 of each frame are not drawn
 *   only LO HI   -- only draws LO..HI-1 are drawn
 *   log          -- the next frame's draws are listed on stderr
 * Draw numbers count setup_pixels calls from the start of a frame. */
static struct { int on, mode; uint32_t lo, hi; int log; FILETIME ft; uint32_t dix, flags; } s_dbg = { -1 };
/* flags: 1 unsampled stages read 0, 2 r0.a starts 0 without stage 0, 8 skip combiner draws with no texture stages, 16 print each new pixel shader */


/* RECOMP_FLASH_LOG=1: watch each presented frame's brightness and, when it
 * jumps (a white flash), print the additive and full-screen draws of that
 * frame and the one before -- the flash lasts one frame and cannot be caught
 * by hand. */
typedef struct { uint32_t dix, tex, bs, bd, n; float c[4], x0, y0, x1, y1; } FlashRec;
static FlashRec s_fr[2][64];
static int s_frn[2], s_frcur, s_flash_on = -1;
static int flash_on(void)
{
    if (s_flash_on < 0) {
        const char *e = getenv("RECOMP_FLASH_LOG");
        s_flash_on = e && *e == '1';
    }
    return s_flash_on;
}
static int flash_on2(void) { return flash_on(); }
static void flash_note(const Nv2aGpuState *st, const float *pos, uint32_t stride,
                       const float *col, uint32_t n)
{
    const Nv2aRenderState *rs = &st->rs;
    FlashRec *r;
    uint32_t k;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    if (!flash_on() || s_frn[s_frcur] >= 64)
        return;
    for (k = 0; k < n && k < 64; k++) {
        float x = pos[k * stride], y = pos[k * stride + 1];
        if (x < x0) x0 = x; if (x > x1) x1 = x;
        if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    if (!(rs->blend_enable && rs->blend_dst == 1) && !(x1 - x0 >= 600.0f && y1 - y0 >= 440.0f))
        return;
    r = &s_fr[s_frcur][s_frn[s_frcur]++];
    r->dix = s_dbg.dix; r->tex = st->tex[0].valid ? st->tex[0].offset : 0;
    r->bs = rs->blend_src; r->bd = rs->blend_dst; r->n = n;
    memcpy(r->c, col, sizeof r->c);
    r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1;
}
/* Every draw of the last two frames, for the one-frame UI corruption (text
 * as solid white boxes, icons drawn giant): when the presented frame's
 * near-white pixel count jumps, both frames' draws are printed and the bad
 * ones stand out by their boxes and textures. */
typedef struct { uint32_t dix, tex, n, be, bs, bd, pal, at, rc, filt, fmt; float c[4], x0, y0, x1, y1, z, u0, v0, u1, v1; } WRec;
static WRec s_wr[2][256];
static int s_wrn[2];
static void wrec_note(const Nv2aGpuState *st, const float *pos, uint32_t stride,
                      const float *col, uint32_t n)
{
    const Nv2aRenderState *rs = &st->rs;
    WRec *r;
    uint32_t k;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    if (s_wrn[s_frcur] >= 256)
        return;
    for (k = 0; k < n && k < 64; k++) {
        float x = pos[k * stride], y = pos[k * stride + 1];
        if (x < x0) x0 = x; if (x > x1) x1 = x;
        if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    r = &s_wr[s_frcur][s_wrn[s_frcur]++];
    r->dix = s_dbg.dix; r->tex = st->tex[0].valid ? st->tex[0].offset : 0;
    r->be = rs->blend_enable; r->bs = rs->blend_src; r->bd = rs->blend_dst;
    r->n = n;
    r->pal = st->tex[0].valid ? st->tex[0].palette : 0;
    r->fmt = st->tex[0].valid ? st->tex[0].color : 0;
    r->filt = st->tex[0].valid ? st->tex[0].filter : 0;
    r->at = (rs->alpha_test_enable << 16) | (rs->alpha_func << 8) | (rs->alpha_ref & 0xFF);
    r->rc = (st->use_rc << 16) | (st->rc.stage_program & 0xFFFF);
    memcpy(r->c, col, sizeof r->c);
    r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1;
    r->z = n ? pos[2] : 0.0f;
    r->u0 = r->v0 = 1e9f; r->u1 = r->v1 = -1e9f;
}
/* uv bbox of texture stage 0, for the fixed-function path */
static void wrec_uv(const Nv2aGpuVertex *v, uint32_t n)
{
    WRec *r;
    uint32_t k;
    if (!s_wrn[s_frcur])
        return;
    r = &s_wr[s_frcur][s_wrn[s_frcur] - 1];
    for (k = 0; k < n && k < 64; k++) {
        float u = v[k].tex[0][0], w2 = v[k].tex[0][1];
        if (u < r->u0) r->u0 = u; if (u > r->u1) r->u1 = u;
        if (w2 < r->v0) r->v0 = w2; if (w2 > r->v1) r->v1 = w2;
    }
}
static void wrec_dump(int which, uint32_t frame)
{
    int i;
    for (i = 0; i < s_wrn[which]; i++) {
        const WRec *r = &s_wr[which][i];
        fprintf(stderr, "  [WREC %u] draw %u tex %08X f%02X pal %08X filt %X at %X rc %X be %u blend %X/%X n %u z %g col %.2f %.2f %.2f %.2f box %.0f,%.0f..%.0f,%.0f uv %.3f,%.3f..%.3f,%.3f\n",
                frame, r->dix, r->tex, r->fmt, r->pal, r->filt, r->at, r->rc,
                r->be, r->bs, r->bd, r->n, r->z,
                r->c[0], r->c[1], r->c[2], r->c[3], r->x0, r->y0, r->x1, r->y1,
                r->u0, r->v0, r->u1, r->v1);
    }
}
static void flash_dump(int which, uint32_t frame)
{
    int i;
    for (i = 0; i < s_frn[which]; i++) {
        const FlashRec *r = &s_fr[which][i];
        fprintf(stderr, "  [FLASH %u] draw %u tex %08X blend %X/%X n %u col %.2f %.2f %.2f %.2f box %.0f,%.0f..%.0f,%.0f\n",
                frame, r->dix, r->tex, r->bs, r->bd, r->n, r->c[0], r->c[1], r->c[2], r->c[3],
                r->x0, r->y0, r->x1, r->y1);
    }
}
/* frame and draw index for other files' event logs */
uint32_t nv2a_d3d_mark(void) { return (s_frame << 10) | (s_dbg.dix & 0x3FFu); }

/* DEPTH_FUNC change trace. The stencil-fin pass that carves the flashlight
 * beam needs depth func LEQUAL (0x203); after a room change it has been seen
 * stuck at ALWAYS (0x207), which destroys the carve and kills the beam for
 * good. This records every change, tagged with frame and draw index, into a
 * ring so the last few seconds survive however long the title has run; F9
 * dumps the ring. The pattern across a transition shows whether the per-frame
 * "set back to 0x203 before the fins" write is still arriving afterwards or
 * has gone missing (a sticky-state loss at a scan seam). */
#define DF_RING 4096
static struct { uint32_t frame, draw, oldv, newv; } s_df_ring[DF_RING];
static unsigned s_df_head, s_df_count;
void nv2a_note_depthfunc(uint32_t oldv, uint32_t newv)
{
    unsigned i = s_df_head;
    s_df_ring[i].frame = s_frame;
    s_df_ring[i].draw  = s_dbg.dix & 0x3FFu;
    s_df_ring[i].oldv  = oldv;
    s_df_ring[i].newv  = newv;
    s_df_head = (i + 1) % DF_RING;
    if (s_df_count < DF_RING) s_df_count++;
}
static void df_dump(void)
{
    unsigned n = s_df_count;
    unsigned i = (s_df_head + DF_RING - n) % DF_RING;
    fprintf(stderr, "[DF] dumping last %u depth-func changes (oldest first):\n", n);
    for (; n; n--, i = (i + 1) % DF_RING)
        fprintf(stderr, "[DF] frame %u draw %u: %03X -> %03X\n",
                s_df_ring[i].frame, s_df_ring[i].draw,
                s_df_ring[i].oldv, s_df_ring[i].newv);
}

/* Dump a render target's current pixels to a PNG beside the exe, so the
 * flashlight's glow accumulation buffer (807F4000) and the composited scene
 * (80428000) can be inspected frame by frame -- whether the glow is building
 * up, holding, or decaying over time. F9 triggers it. */
/* Build L"<stem>_<va>_<tag>_f<frame>.png" widening the narrow tag by
 * hand: swprintf's %S reads the tag as wide on some runtimes and the
 * mojibake name then fails to open. */
static void dump_name(wchar_t *out, size_t n, const wchar_t *stem,
                      uint32_t va, const char *tag, uint32_t frame)
{
    size_t i = 0, k;
    i += (size_t)swprintf(out + i, n - i, L"%ls_%08X_", stem, va);
    for (k = 0; tag[k] && i + 1 < n; k++)
        out[i++] = (wchar_t)(unsigned char)tag[k];
    swprintf(out + i, n - i, L"_f%u.png", frame);
}

static void ds_dump_png(uint32_t va, const char *tag)
{
    DsEnt *e = find_ds(va);
    D3D11_TEXTURE2D_DESC td, sd;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t w, h, y, x;
    wchar_t path[MAX_PATH];
    if (!e || !e->t) {
        fprintf(stderr, "[DSDUMP] %08X (%s): no live DS entry\n", va, tag);
        return;
    }
    ID3D11Texture2D_GetDesc(e->t, &td);
    w = td.Width; h = td.Height;
    memset(&sd, 0, sizeof sd);
    sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = td.Format; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(D, &sd, NULL, &stg))) {
        fprintf(stderr, "[DSDUMP] %08X (%s): staging create failed\n", va, tag);
        return;
    }
    ID3D11DeviceContext_CopyResource(C, (ID3D11Resource *)stg, (ID3D11Resource *)e->t);
    if (SUCCEEDED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
        uint32_t dmin = 0xFFFFFF, dmax = 0, nclear = 0;
        if (px) {
            for (y = 0; y < h; y++) {
                const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
                for (x = 0; x < w; x++) {
                    uint32_t d = row[x] & 0xFFFFFFu, g = d >> 16;
                    if (d == 0xFFFFFFu || d == 0) nclear++;
                    else { if (d < dmin) dmin = d; if (d > dmax) dmax = d; }
                    px[(size_t)y * w + x] = 0xFF000000u | (g << 16) | (g << 8) | g;
                }
            }
            dump_name(path, MAX_PATH, L"dsdump", va, tag, s_frame);
            png_write_rows(path, px, w, h);
            fprintf(stderr, "[DSDUMP] %08X (%s): %ux%u frame %u; depth min %u max %u (z16 %.1f..%.1f), %u texels at clear\n",
                    va, tag, w, h, s_frame, dmin, dmax, dmin / 256.0, dmax / 256.0, nclear);
            free(px);
        }
        ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)stg, 0);
    } else {
        fprintf(stderr, "[DSDUMP] %08X (%s): map failed\n", va, tag);
    }
    ID3D11Texture2D_Release(stg);
}

/* The stencil plane of a 24-bit depth buffer, as a picture: the title's
 * foreground-prop ghosting marks pixels in stencil and redraws through an
 * EQUAL test, so a wrong mask shows up here as a blob where there should
 * be only the prop's opaque texels. 0 is black; each other value gets a
 * bright step. The log line gives the histogram. */
static void st_dump_png(DsEnt *e)
{
    D3D11_TEXTURE2D_DESC td, sd;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t w, h, y, x, hist[256];
    wchar_t path[MAX_PATH];
    if (!e || !e->t)
        return;
    ID3D11Texture2D_GetDesc(e->t, &td);
    if (td.Format != DXGI_FORMAT_R24G8_TYPELESS)
        return;                      /* Z16 has no stencil plane */
    w = td.Width; h = td.Height;
    memset(&sd, 0, sizeof sd);
    sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = td.Format; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(D, &sd, NULL, &stg)))
        return;
    ID3D11DeviceContext_CopyResource(C, (ID3D11Resource *)stg, (ID3D11Resource *)e->t);
    memset(hist, 0, sizeof hist);
    if (SUCCEEDED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
        if (px) {
            for (y = 0; y < h; y++) {
                const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
                for (x = 0; x < w; x++) {
                    uint32_t sv = row[x] >> 24;
                    uint32_t g = sv == 0 ? 0 : 64 + (sv * 48) % 192;
                    hist[sv]++;
                    px[(size_t)y * w + x] = 0xFF000000u | (g << 16) | (g << 8) | g;
                }
            }
            swprintf(path, MAX_PATH, L"stdump_%08X_f%u.png", e->va, s_frame);
            png_write_rows(path, px, w, h);
            fprintf(stderr, "[STDUMP] %08X: %ux%u frame %u; stencil", e->va, w, h, s_frame);
            for (x = 0; x < 256; x++)
                if (hist[x])
                    fprintf(stderr, " [%u]=%u", x, hist[x]);
            fputc('\n', stderr);
            free(px);
        }
        ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)stg, 0);
    }
    ID3D11Texture2D_Release(stg);
}

static void rt_dump_png(uint32_t va, const char *tag)
{
    RtEnt *e = find_rt(va);
    D3D11_TEXTURE2D_DESC td, sd;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t *px, w, h, y;
    wchar_t path[MAX_PATH];
    if (!e || !e->t) {
        fprintf(stderr, "[RTDUMP] %08X (%s): no live RT entry\n", va, tag);
        return;
    }
    ID3D11Texture2D_GetDesc(e->t, &td);
    w = td.Width; h = td.Height;
    memset(&sd, 0, sizeof sd);
    sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = td.Format; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(D, &sd, NULL, &stg))) {
        fprintf(stderr, "[RTDUMP] %08X (%s): staging create failed\n", va, tag);
        return;
    }
    ID3D11DeviceContext_CopyResource(C, (ID3D11Resource *)stg, (ID3D11Resource *)e->t);
    if (SUCCEEDED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        px = (uint32_t *)malloc((size_t)w * h * 4);
        if (px) {
            for (y = 0; y < h; y++)
                memcpy(px + (size_t)y * w,
                       (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
                       (size_t)w * 4);
            dump_name(path, MAX_PATH, L"rtdump", va, tag, s_frame);
            png_write_rows(path, px, w, h);
            fprintf(stderr, "[RTDUMP] %08X (%s): wrote %ux%u at frame %u\n", va, tag, w, h, s_frame);
            free(px);
        }
        ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)stg, 0);
    } else {
        fprintf(stderr, "[RTDUMP] %08X (%s): map failed\n", va, tag);
    }
    ID3D11Texture2D_Release(stg);
}
uint32_t g_comp_frame_count;    /* draws into the glow back buffer this frame */
uint32_t g_glow_target_sets;    /* times the guest set RT = glow buffer this
                                 * frame (from the pushbuffer executor) */
uint32_t g_glow_tex_binds;      /* times the guest bound the accumulation
                                 * buffer 807F4000 as a texture this frame */
uint32_t g_sig_cur[2048], g_tex_cur[2048];
static uint32_t s_sig[3][2048], s_tex3[3][2048];
extern volatile int g_vis_verbose;
static void dbg_poll(void)
{
    if (flash_on()) { s_frcur ^= 1; s_frn[s_frcur] = 0; s_wrn[s_frcur] = 0; }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILE *f;
    char line[128];
    if (s_dbg.on < 0) {
        const char *e = getenv("RECOMP_GPU_DEBUG");
        s_dbg.on = e && *e && *e != '0';
    }
    s_dbg.dix = 0;
    if (s_dbg.log > 0) s_dbg.log--;
    /* F9 in the game window: list the next frame's draws in the log, for a
     * picture that is wrong only at one spot in the game (a stuck camera, a
     * doorway) and so cannot be reproduced on demand. */
    if (GetAsyncKeyState(VK_F9) & 1) {
        s_dbg.log = 2;
        g_vis_verbose = 240;
        fprintf(stderr, "[F9] frame %u: listing the next frame's draws (scale %d)\n", s_frame, s_scale);
        df_dump();
        rt_dump_png(0x807F4000u, "accum");   /* glow accumulation buffer */
        rt_dump_png(0x80428000u, "bufA");    /* framebuffer A */
        rt_dump_png(0x80554000u, "bufB");    /* framebuffer B (glow composite) */
        ds_dump_png(0x80E57000u, "shadow");  /* flashlight shadow map */
        {
            int di;
            for (di = 0; di < 16; di++)
                if (s_ds[di].t) {
                    st_dump_png(&s_ds[di]);
                    if (s_ds[di].va != 0x80E57000u)
                        ds_dump_png(s_ds[di].va, "main");
                }
        }
        /* The flashlight cookie: decode the 64x64 A8 at its home address
         * straight from guest memory, with brightness centroid, so a dump
         * shows what the beam spot looks like and where it sits. */
        {
            Nv2aGpuTexture ct;
            static uint32_t cbuf[64 * 64];
            memset(&ct, 0, sizeof ct);
            ct.offset = 0x80E97000u;
            ct.width = 64; ct.height = 64; ct.color = 0x19; ct.levels = 1;
            if (nv2a_gpu_decode_texture(&ct, 0, cbuf)) {
                uint32_t x2, y2;
                double sa = 0, sx = 0, sy = 0; uint32_t amax = 0;
                static uint32_t px2[64 * 64];
                wchar_t cp[MAX_PATH];
                for (y2 = 0; y2 < 64; y2++)
                    for (x2 = 0; x2 < 64; x2++) {
                        uint32_t a2 = cbuf[y2 * 64 + x2] >> 24;
                        px2[y2 * 64 + x2] = 0xFF000000u | (a2 << 16) | (a2 << 8) | a2;
                        sa += a2; sx += (double)a2 * x2; sy += (double)a2 * y2;
                        if (a2 > amax) amax = a2;
                    }
                swprintf(cp, MAX_PATH, L"cookie_f%u.png", s_frame);
                png_write_rows(cp, px2, 64, 64);
                fprintf(stderr, "[COOKIE] 80E97000 frame %u: max %u avg %.1f centroid %.1f %.1f (of 64)\n",
                        s_frame, amax, sa / 4096.0, sa > 0 ? sx / sa : -1, sa > 0 ? sy / sa : -1);
            } else {
                fprintf(stderr, "[COOKIE] decode failed\n");
            }
        }
        {
            extern uint32_t xbox_GetDisplayFramebuffer(uint32_t *);
            uint32_t p = 0, fb = xbox_GetDisplayFramebuffer(&p);
            fprintf(stderr, "[F9] presented/scanout buffer = %08X (pitch %u)\n", fb, p);
        }
    }

    /* Flashlight glow-composite death watch. g_comp_frame_count holds the
     * previous frame's count of passes that paint the orb (they sample the
     * accumulation buffer 807F4000). Once those have run reliably for a while
     * and then stop for good, log the exact frame -- so it can be lined up
     * against the [WRAP] ring-wrap events and the stage-program history to
     * see what the game lost. Also a periodic pulse so the taper is visible. */
    {
        extern void xbox_pb_stageprog_dump(void);
        static unsigned live_frames, dead_run, died_at, last_pulse;
        static int was_live, announced;
        /* g_comp_frame_count = draws that reached our pipeline targeting the
         * glow buffer; g_glow_target_sets = times the guest ASKED for it in
         * the pushbuffer. The two together localize the death: if the guest
         * stops asking, it is guest-side; if it keeps asking but no draws
         * land, we are dropping them. */
        /* Death is keyed on the accumulation-buffer binds: those are the orb
         * composite reads, the thing that actually stops. Alongside it, the
         * RT=glow sets show the guest's render loop is otherwise unchanged. */
        if (g_glow_tex_binds > 2) {
            live_frames++; dead_run = 0;
            if (live_frames > 120) was_live = 1;
        } else {
            if (dead_run == 0) died_at = s_frame;
            dead_run++;
            if (was_live && !announced && dead_run > 30) {
                announced = 1;
                fprintf(stderr, "[COMPWATCH] orb composite (807F4000 binds) DIED at frame %u "
                        "(was live %u frames, now dead %u). That frame: %u accumulation-binds, "
                        "%u RT=glow sets, %u draws into 80554000. => %s\n",
                        died_at, live_frames, dead_run,
                        g_glow_tex_binds, g_glow_target_sets, g_comp_frame_count,
                        g_glow_tex_binds > 2 ? "still bound"
                        : (g_glow_target_sets > 0 ? "GUEST STILL RENDERS GLOW PHASE but stopped binding accumulation (our drop OR guest gate)"
                                                  : "guest stopped glow phase entirely"));
                xbox_pb_stageprog_dump();
            }
        }
        if (s_frame - last_pulse >= 120) {
            last_pulse = s_frame;
            fprintf(stderr, "[COMPWATCH] frame %u: %u accumulation-binds, %u RT=glow sets, "
                    "%u draws->80554000 (live %u dead %u)\n",
                    s_frame, g_glow_tex_binds, g_glow_target_sets, g_comp_frame_count,
                    live_frames, dead_run);
        }
        g_comp_frame_count = 0;
        g_glow_target_sets = 0;
        g_glow_tex_binds = 0;
    }
    if (s_dbg.on > 0 && GetFileAttributesA("gpulogall") != INVALID_FILE_ATTRIBUTES) {
        /* while a file named gpulogall exists beside the exe: every frame */
        s_dbg.log = 1;
        fprintf(stderr, "[FRAME %u]\n", s_frame);
    }
    s_clear_log = (s_dbg.log >= 1);
    g_gpu_frame_log = s_clear_log;
    if (!s_dbg.on || (s_frame & 15))
        return;
    if (!GetFileAttributesExA("gpudbg.txt", GetFileExInfoStandard, &fa))
        return;
    if (!memcmp(&fa.ftLastWriteTime, &s_dbg.ft, sizeof s_dbg.ft))
        return;
    s_dbg.ft = fa.ftLastWriteTime;
    f = fopen("gpudbg.txt", "r");
    if (!f) return;
    s_dbg.mode = 0; s_dbg.flags = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned lo, hi;
        if (sscanf(line, "skip %u %u", &lo, &hi) == 2) { s_dbg.mode = 1; s_dbg.lo = lo; s_dbg.hi = hi; }
        else if (sscanf(line, "only %u %u", &lo, &hi) == 2) { s_dbg.mode = 2; s_dbg.lo = lo; s_dbg.hi = hi; }
        else if (!strncmp(line, "log", 3)) s_dbg.log = 2;
        else if (sscanf(line, "flags %u", &lo) == 1) s_dbg.flags = lo;
    }
    fclose(f);
    fprintf(stderr, "[GPU-DBG] frame %u mode %d %u..%u log %d flags %X\n", s_frame, s_dbg.mode, s_dbg.lo, s_dbg.hi, s_dbg.log, s_dbg.flags);
}

static void dbg_log(uint32_t dix, const Nv2aGpuState *st, const PsKey *key)
{
    const Nv2aRenderState *rs = &st->rs;
    int i;
    fprintf(stderr, "[DRAW %4u] rt %08X z %08X/%u ps %016llX rc %u sp %05X blend %u %X %X %X bc %08X"
            " at %u %X %u depth %u %X %u st %u %X %u m%X/%X %X %X %X cm %08X",
            dix, st->color_va, st->zeta_va, st->zeta_bits,
            (unsigned long long)hash_bytes((const uint8_t *)key, sizeof *key, 7), st->use_rc,
            st->rc.stage_program, rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq, rs->blend_color,
            rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref,
            rs->depth_test_enable, rs->depth_func, rs->depth_write,
            rs->stencil_enable, rs->stencil_func, rs->stencil_ref, rs->stencil_func_mask, rs->stencil_write_mask,
            rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass, rs->color_mask);
    for (i = 0; i < 4; i++)
        if (st->tex[i].valid)
            fprintf(stderr, " t%d %08X f%02X %ux%u a%u%u", i, st->tex[i].offset, st->tex[i].color,
                    st->tex[i].width, st->tex[i].height, st->tex[i].addr_u, st->tex[i].addr_v);
    fputc('\n', stderr);
    if (s_dbg.flags & 16) {
        static uint64_t seen[256]; static int nseen;
        uint64_t h = hash_bytes((const uint8_t *)key, sizeof *key, 7);
        int q;
        for (q = 0; q < nseen && seen[q] != h; q++) ;
        if (q == nseen && nseen < 256) {
            char *src = gen_ps(key);
            seen[nseen++] = h;
            fprintf(stderr, "[PS %016llX] control %08X final %08X %08X icw %08X %08X ocw %08X %08X"
                    " aicw %08X %08X aocw %08X %08X f0 %08X f1 %08X\n%s\n", (unsigned long long)h,
                    key->control, key->final0, key->final1, key->color_icw[0], key->color_icw[1],
                    key->color_ocw[0], key->color_ocw[1], key->alpha_icw[0], key->alpha_icw[1],
                    key->alpha_ocw[0], key->alpha_ocw[1], st->rc.factor0[0], st->rc.factor1[0], src);
            free(src);
        }
    }
}

/* The rasteriser state for a draw: s_rs, or one with the title's polygon
 * offset as a depth bias. A Z16 unit is 256 of this D24 buffer's; the slope
 * term is per pixel, so a surface drawn at s_scale times the size needs
 * s_scale times the factor for the same offset. Without this a shadow map
 * compares each surface with itself and the flashlight comes out as a fine
 * lit/unlit checkerboard that reads as no light at all. */
static ID3D11RasterizerState *rs_for(const Nv2aRenderState *rs)
{
    static struct { int bias, cull, ccw, clamp; float slope; ID3D11RasterizerState *st; } cache[32];
    static int n, logged, mode = -2;
    D3D11_RASTERIZER_DESC rd;
    int bias = 0, i, cull = 0, ccw = 0, clamp = 0;
    float slope = 0.0f;

    /* RECOMP_GPU_CULL: 1 (default) honours SET_CULL_FACE as the title sets
     * it, 2 the same with the winding read the other way, 0 draws both faces
     * as before. A shadow-map pass that draws back faces only is what keeps
     * a lit surface from comparing against its own depth. */
    if (mode == -2) {
        const char *e = getenv("RECOMP_GPU_CULL");
        mode = e && *e ? atoi(e) : 1;
    }
    if (mode && rs->cull_enable) {
        if (rs->cull_face == 0x0404) cull = 1;          /* GL_FRONT */
        else if (rs->cull_face == 0x0405) cull = 2;     /* GL_BACK */
        else if (rs->cull_face == 0x0408) cull = 3;     /* both: nothing drawn */
        ccw = rs->front_face == 0x0901;                 /* GL_CCW */
        if (mode == 2) ccw = !ccw;
    }
    /* RECOMP_SHADOW_OFFSET (default 1): a depth-only pass into a Z16 surface
     * -- a shadow map -- takes the title's polygon offset even with the fill
     * enable clear. Fatal Frame loads factor 1, bias 200 for its flashlight's
     * shadow pass and leaves SET_POLY_OFFSET_FILL_ENABLE at 0; without the
     * offset every lit surface compares against its own stored depth and the
     * light comes out as curved lit/unlit bands. */
    static int shoff = -1;
    if (shoff < 0) {
        const char *e = getenv("RECOMP_SHADOW_OFFSET");
        shoff = !(e && *e == '0');
    }
    if ((rs->poly_fill || (shoff && rs->zeta_fmt == 1 && rs->color_mask == 0 && rs->depth_write))
        && (rs->poly_factor != 0.0f || rs->poly_units != 0.0f)) {
        bias = (int)(rs->poly_units * (rs->zeta_fmt == 1 ? 256.0f : 1.0f));
        slope = rs->poly_factor * (float)s_scale;
    }
    /* A depth-only pass into a Z16 surface -- the flashlight's shadow map --
     * stores geometry past the far plane saturated at 65535 on the console
     * (a Z16 write clamps).  D3D's far clip removed those fragments, leaving
     * holes the beam read as occluders.  Depth clamp restores the console's
     * writes; the w<=0 clip and the title's near plane are unaffected. */
    if (rs->zeta_fmt == 1 && rs->color_mask == 0 && rs->depth_write) {
        clamp = 1;
        /* RECOMP_SHADOW_CULL: 1 (default) the title's cull as mapped above,
         * 2 the opposite face, 0 both faces. The map should hold each ray's
         * EXIT from solid geometry (back faces), so lit surfaces compare
         * against the far side, not themselves. */
        static int scull = -1;
        if (scull < 0) {
            const char *e = getenv("RECOMP_SHADOW_CULL");
            scull = e && *e ? atoi(e) : 1;
        }
        if (scull == 2 && (cull == 1 || cull == 2))
            cull = cull == 1 ? 2 : 1;
        else if (scull == 0)
            cull = 0;
    }
    if (!cull && !bias && slope == 0.0f && !clamp)
        return s_rs;
    for (i = 0; i < n; i++)
        if (cache[i].bias == bias && cache[i].slope == slope
            && cache[i].cull == cull && cache[i].ccw == ccw && cache[i].clamp == clamp)
            return cache[i].st;
    if (n == 32)
        return s_rs;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = cull == 1 ? D3D11_CULL_FRONT : cull == 2 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;
    rd.DepthBias = bias;
    rd.SlopeScaledDepthBias = slope;
    if (FAILED(ID3D11Device_CreateRasterizerState(D, &rd, &cache[n].st)))
        return s_rs;
    cache[n].bias = bias; cache[n].slope = slope; cache[n].cull = cull; cache[n].ccw = ccw;
    if (logged++ < 12)
        fprintf(stderr, "[GPU-D3D11] raster state: cull %d (face %04X front %04X) bias %d slope %g zeta 0x%08X fmt %u control0 %08X poly %u %g %g\n",
                cull, rs->cull_face, rs->front_face, bias, slope, rs->zeta_va, rs->zeta_fmt, rs->control0,
                rs->poly_fill, rs->poly_factor, rs->poly_units);
    return cache[n++].st;
}

static int s_draw_samples_rt;   /* this draw reads a render target */
static uint32_t s_glow_fb, s_glow_blits;   /* feedback draws and frame blits
                                            * since the last [GLOW] report */
static uint32_t s_glow_blitfail;           /* blits refused (and the target
                                            * entry freed) since the report */
static uint32_t s_glow_plain;              /* full-screen feedback-sized
                                            * textures sampled WITHOUT an RT
                                            * entry: the broken state */

static int setup_pixels(const Nv2aGpuState *st)
{
    s_draw_samples_rt = 0;
    const Nv2aRenderState *rs = &st->rs;
    uint32_t rect[4];
    DsEnt *ds = NULL;
    PsKey key;
    PsConst pc;
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *smp[4] = { NULL, NULL, NULL, NULL };
    ID3D11PixelShader *ps;
    D3D11_DEPTH_STENCIL_DESC dd;
    float bf[4];
    int i, need_depth;
    uint32_t dix = s_dbg.dix++;
    if (st->color_va == 0x80554000u)
        g_comp_frame_count++;   /* a draw into the glow-composite back buffer;
                                 * when these stop, the orb is dead. */
    if (dix < 2048) {
        {
            /* the white-box event, caught in the act: many textured draws
             * in one frame with texture stage 0 off */
            static uint32_t nountex_frame, nountex_n; static unsigned ctxdumps;
            if (flash_on2() && st->use_rc && st->tex[0].valid
                && !(st->rc.stage_program & 0x1F)) {
                if (nountex_frame != s_frame) { nountex_frame = s_frame; nountex_n = 0; }
                if (++nountex_n == 6 && ctxdumps < 8) {
                    extern void nv2a_pb_dump_context(void);
                    ctxdumps++;
                    fprintf(stderr, "[UNTEX] frame %u draw %u: 6th textured draw with stage 0 off; ring context:\n",
                            s_frame, dix);
                    nv2a_pb_dump_context();
                }
            }
        }
        extern uint32_t g_sig_cur[2048], g_tex_cur[2048];
        g_sig_cur[dix] = (st->tex[0].valid ? st->tex[0].offset : 0) ^ (st->color_va << 1)
                       ^ (st->rs.blend_src << 7) ^ (st->rs.blend_dst << 13) ^ (st->topo << 29);
        g_tex_cur[dix] = st->tex[0].valid ? st->tex[0].offset : 0;
    }

    if (s_dbg.mode == 1 && dix >= s_dbg.lo && dix < s_dbg.hi) return 0;
    if (s_dbg.mode == 2 && (dix < s_dbg.lo || dix >= s_dbg.hi)) return 0;
    rect[0] = st->clip_x; rect[1] = st->clip_y;
    rect[2] = st->clip_x + st->clip_w; rect[3] = st->clip_y + st->clip_h;
    if (st->swizzled) {
        rect[0] = rect[1] = 0;
        if (rect[2] > st->swz_w) rect[2] = st->swz_w;
        if (rect[3] > st->swz_h) rect[3] = st->swz_h;
    }
    need_depth = rs->depth_test_enable || rs->depth_write || rs->stencil_enable;
    if (!bind_surface(st, need_depth, rect, &ds))
        return 0;

    /* Pixel shader and its constants. */
    memset(&key, 0, sizeof key);
    memset(&pc, 0, sizeof pc);
    key.use_rc = st->use_rc;
    key.textured = st->use_rc ? 0 : st->textured;
    if (st->use_rc) {
        memcpy(key.color_icw, st->rc.color_icw, sizeof key.color_icw);
        memcpy(key.alpha_icw, st->rc.alpha_icw, sizeof key.alpha_icw);
        memcpy(key.color_ocw, st->rc.color_ocw, sizeof key.color_ocw);
        memcpy(key.alpha_ocw, st->rc.alpha_ocw, sizeof key.alpha_ocw);
        key.final0 = st->rc.final0;
        key.final1 = st->rc.final1;
        key.control = st->rc.control;
        key.stage_program = st->rc.stage_program;
        key.clip_plane_mode = st->clip_plane_mode;
        for (i = 0; i < 8; i++) {
            argb_to_f4(st->rc.factor0[i], pc.f0[i]);
            argb_to_f4(st->rc.factor1[i], pc.f1[i]);
        }
        argb_to_f4(st->rc.final_c0, pc.fc0);
        argb_to_f4(st->rc.final_c1, pc.fc1);
        /* FOG_COLOR is R in bits 0-7 (raster_xf_triangle swaps it). */
        pc.fogc[0] = (float)(st->fog_color & 0xFF) / 255.0f;
        pc.fogc[1] = (float)((st->fog_color >> 8) & 0xFF) / 255.0f;
        pc.fogc[2] = (float)((st->fog_color >> 16) & 0xFF) / 255.0f;
        pc.fogc[3] = (float)(st->fog_color >> 24) / 255.0f;
    }
    for (i = 0; i < 4; i++) {
        const Nv2aGpuTexture *t = &st->tex[i];
        uint32_t mode = st->use_rc ? (st->rc.stage_program >> (i * 5)) & 0x1F
                                   : (i == 0 && st->textured ? 1u : 0u);
        RtEnt *rt;
        uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
        int linear;

        pc.ts[i][0] = pc.ts[i][1] = 1.0f;
        if (!(mode == 1 || mode == 2 || mode == 3) || !t->valid)
            continue;
        linear = st->bilinear || mag == 2 || min == 2 || min == 4 || min == 6;
        rt = t->cube ? NULL : find_rt(t->offset);
        if (rt && s_frame - rt->used > 2) {
            /* An entry nothing has written for a while. If the guest bytes
             * under it still read as they did when it was last written, the
             * title has not touched the memory and the copy on the GPU is
             * the surface's truth (a pause screen sampling an old frame).
             * Changed bytes mean the title reused the address -- usually
             * for an ordinary texture -- and sampling the old target fed
             * draws framebuffer leftovers instead: the flashlight's glow
             * died for good at the first room change. */
            uint64_t now = rt_gm_hash(rt->va, rt_gm_range(rt));
            if (!rt->gm_hash) {
                RT_TRACE(t->offset, "stale sample, baseline hash %016llX (used frame %u)",
                         (unsigned long long)now, rt->used);
                rt->gm_hash = now;      /* a drawn-to target: baseline on the
                                         * first stale hit, compare after */
            } else if (now != rt->gm_hash) {
                RT_TRACE(t->offset, "stale sample, memory changed %016llX -> %016llX (used frame %u): entry dropped",
                         (unsigned long long)rt->gm_hash, (unsigned long long)now, rt->used);
                free_rt(rt);
                memset(rt, 0, sizeof *rt);
                rt = NULL;
            } else {
                RT_TRACE(t->offset, "stale sample, memory unchanged: kept (used frame %u)", rt->used);
            }
        }
        if (!rt && t->color >= 0x2A && t->color <= 0x31) {
            DsEnt *dz = find_ds(t->offset);
            if (dz && dz->srv && (!ds || dz != ds)) {
                float zw = (float)dz->w / (float)s_scale, zh = (float)dz->h / (float)s_scale;
                srv[i] = dz->srv;
                key.kind[i] = 3;
                key.shadow_func = st->shadow_func;
                {
                    static uint32_t seen[16]; static int ns;
                    uint32_t sig = (st->shadow_func << 28) ^ (t->offset & 0x0FFFFFFF) ^ ((uint32_t)i << 26);
                    int k2;
                    for (k2 = 0; k2 < ns && seen[k2] != sig; k2++) ;
                    if (k2 == ns && ns < 16) {
                        seen[ns++] = sig;
                        fprintf(stderr, "[SHADOW] stage %d tex 0x%08X fmt 0x%02X %ux%u norm %d func %u -> ds %ux%u (scale %d)\n",
                                i, t->offset, t->color, t->width, t->height, t->normalized,
                                st->shadow_func, dz->w, dz->h, s_scale);
                    }
                }
                if (t->normalized) {
                    pc.ts[i][0] = (float)t->width / zw;
                    pc.ts[i][1] = (float)t->height / zh;
                } else {
                    pc.ts[i][0] = 1.0f / zw;
                    pc.ts[i][1] = 1.0f / zh;
                }
                pc.ts[i][2] = (t->color == 0x2C || t->color == 0x2D || t->color == 0x30
                               || t->color == 0x31) ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
                smp[i] = get_ss_cmp(linear, st->shadow_func);
                continue;
            }
        }
        if (!rt && t->width >= 512 && t->height >= 384
            && (t->color == 0x07 || t->color == 0x1E || t->color == 0x3A || t->color == 0x3F)) {
            /* A frame-sized linear surface sampled with no live RT entry:
             * the feedback chain lost its buffer and is reading stale guest
             * memory -- the dead-flashlight state. */
            static unsigned said2;
            s_glow_plain++;
            if (said2 < 20) {
                said2++;
                fprintf(stderr, "[GLOW] frame %u draw %u: %ux%u f%02X at %08X sampled as a plain texture (no RT entry)\n",
                        s_frame, s_dbg.dix, t->width, t->height, t->color, t->offset);
            }
        }
        if (rt) {
            /* Render-to-texture: sample the target itself. Its texels hold
             * A8R8G8B8 as drawn; a texture read in the reverse order swaps. */
            srv[i] = rt->srv;
            s_draw_samples_rt = 1;
            key.kind[i] = 1;
            key.swap_rb[i] = (t->color == 0x3A || t->color == 0x3F);
            key.force_a1[i] = (t->color == 0x07 || t->color == 0x1E);
            if (t->normalized) {
                pc.ts[i][0] = (float)t->width / (float)rt->w;
                pc.ts[i][1] = (float)t->height / (float)rt->h;
            } else {
                pc.ts[i][0] = 1.0f / (float)rt->w;
                pc.ts[i][1] = 1.0f / (float)rt->h;
            }
            S.tex_hits_rt++;
            if (s_stats > 0) {
                static uint32_t seen[64][4]; static int nseen;
                int q, hit = 0;
                for (q = 0; q < nseen; q++)
                    if (seen[q][0] == t->offset && seen[q][1] == t->color && seen[q][2] == t->width)
                        hit = 1;
                if (!hit && nseen < 64) {
                    seen[nseen][0] = t->offset; seen[nseen][1] = t->color; seen[nseen][2] = t->width; nseen++;
                    fprintf(stderr, "[GPU-D3D11] render-to-texture read: va %08X fmt %02X %ux%u norm %u"
                            " (target %ux%u swz %u) stage %d mode %u\n", t->offset, t->color,
                            t->width, t->height, t->normalized, rt->w, rt->h, rt->swizzled, i, mode);
                }
            }
        } else {
            if (s_stats > 0) {
                static uint32_t seen2[64]; static int nseen2;
                int q, r, hit = 0;
                for (q = 0; q < nseen2; q++) if (seen2[q] == t->offset) hit = 1;
                if (!hit && nseen2 < 64)
                    for (r = 0; r < MAX_RT; r++) {
                        RtEnt *e = &s_rt[r];
                        uint32_t span = e->pitch ? e->pitch * e->h : e->w * e->h * 4;
                        if (e->t && ((t->offset ^ e->va) & 0x0FFFFFFFu) < 0x10000000u
                            && (t->offset & 0x0FFFFFFFu) >= (e->va & 0x0FFFFFFFu)
                            && (t->offset & 0x0FFFFFFFu) < (e->va & 0x0FFFFFFFu) + span) {
                            seen2[nseen2++] = t->offset;
                            fprintf(stderr, "[GPU-D3D11] texture %08X fmt %02X %ux%u inside target %08X %ux%u"
                                    " pitch %u swz %u but not at its start\n", t->offset, t->color,
                                    t->width, t->height, e->va, e->w, e->h, e->pitch, e->swizzled);
                            break;
                        }
                    }
            }
            srv[i] = get_tex(t);
            if (!srv[i]) {
                /* A stage the title set up that drew untextured: white
                 * text boxes and black panels on the load menu. Say why. */
                static int nmiss;
                if (nmiss < 200) {
                    nmiss++;
                    fprintf(stderr, "[TEXMISS] frame %u stage %d tex %08X fmt %02X %ux%u pitch %u bytes %u swz? cube %d\n",
                            s_frame, i, t->offset, t->color, t->width, t->height, t->pitch, t->bytes, t->cube);
                }
                continue;
            }
            key.kind[i] = t->cube ? 2 : 1;
            if (!t->normalized) {
                pc.ts[i][0] = 1.0f / (float)t->width;
                pc.ts[i][1] = 1.0f / (float)t->height;
            }
        }
        smp[i] = get_ss(linear, t->addr_u, t->addr_v, t->border, t->border_color != 0);
    }
    /* Afterimage filter off: skip the frame-feedback crossfade, the pass
     * that blends the previously accumulated frame back over the new one
     * with the constant-alpha blend and leaves motion trails on the player
     * and the ghosts. The additive bloom and the flashlight orb blend with
     * ONE, not constant alpha, so they stay. */
    if (!xbox_GfxConfig()->afterimage && rs->blend_enable
        && rs->blend_src == 0x8003 && rs->blend_dst == 0x8004
        && s_draw_samples_rt)
        return 0;
    key.dbg = s_dbg.flags & 3;
    if ((s_dbg.flags & 8) && st->use_rc && !(st->rc.stage_program & 0x7FFF)) return 0;
    key.alpha_test = rs->alpha_test_enable;
    key.alpha_func = rs->alpha_test_enable ? rs->alpha_func : 0;
    pc.aref[0] = (int)(rs->alpha_ref & 0xFF);
    if (s_dbg.log == 1) dbg_log(dix, st, &key);
    ps = get_ps(&key);
    if (!ps)
        return 0;

    /* Depth and stencil, with OpenGL's rule (the NV2A's): depth test off
     * means no depth write either, whatever the mask says. The Sims draws
     * its floor shadows with the test off and the mask on; writing their
     * depth hid the walls and furniture drawn after them, leaving black
     * holes where the cleared background showed through. */
    memset(&dd, 0, sizeof dd);
    if (ds) {
        dd.DepthEnable = rs->depth_test_enable != 0;
        dd.DepthFunc = rs->depth_test_enable ? cmp_of(rs->depth_func) : D3D11_COMPARISON_ALWAYS;
        {
            static int noz = -1;
            if (noz < 0) { const char *e = getenv("RECOMP_DBG_LIGHT_NOZ"); noz = e && *e == '1'; }
            if (noz && (key.kind[1] == 3 || key.kind[2] == 3)) dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        }
        dd.DepthWriteMask = rs->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        /* The title's relight and ghosting passes redraw geometry the frame
         * has already laid down, gated on a stencil mark (flashlight beam
         * cone, foreground-prop ghosting), with z-write left on. On NV2A the
         * redraw lands on bit-identical depth, so the write is harmless; here
         * the relight vertex path can land a hair nearer than the first
         * pass, and the stray depth then rejects the characters drawn after
         * it (the doll room hid Miku below the beam cone's footprint). The
         * redraw's depth is redundant by construction: drop the write.
         *
         * Only where the redraw is additive (destination factor ONE). The
         * title gates other things on a stencil EQUAL as well -- a mirror
         * stencils its surface and then draws the reflected scene through
         * it with ordinary alpha blending, and there the depth writes are
         * what sorts the reflection internally. Dropping those put 190 of
         * one frame's 374 draws through unsorted and wrecked the mirrors
         * in Mafuyu's section. */
        {
            static int nozw = -1;
            if (nozw < 0) {
                const char *e = getenv("RECOMP_RELIGHT_NOZW");
                nozw = !(e && *e == '0');
            }
            if (nozw && rs->stencil_enable && rs->stencil_func == 0x202
                && rs->blend_enable && rs->blend_dst == 1
                && rs->depth_func == 0x203 && rs->depth_write)
                dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        }
        if (rs->stencil_enable) {
            dd.StencilEnable = TRUE;
            dd.StencilReadMask = (UINT8)rs->stencil_func_mask;
            dd.StencilWriteMask = (UINT8)rs->stencil_write_mask;
            dd.FrontFace.StencilFunc = cmp_of(rs->stencil_func);
            dd.FrontFace.StencilFailOp = sop_of(rs->stencil_fail);
            dd.FrontFace.StencilDepthFailOp = sop_of(rs->stencil_zfail);
            dd.FrontFace.StencilPassOp = sop_of(rs->stencil_zpass);
            dd.BackFace = dd.FrontFace;
        }
    }
    if (!dd.DepthEnable) {
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    }
    k_dss(get_dss(&dd), rs->stencil_ref & 0xFF);
    argb_to_f4(rs->blend_color, bf);
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004
        || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];
    k_bs(get_bs(rs->blend_enable && !(rs->blend_src == 1 && rs->blend_dst == 0),
                rs->blend_src, rs->blend_dst, rs->blend_eq, write_mask(rs->color_mask)), bf);
    /* Only colour written makes the target worth reading back: Fatal
     * Frame's shadow pass has its colour "surface" at physical 0 with every
     * colour write masked off, and copying that back would clobber low
     * memory. */
    if (write_mask(rs->color_mask) && s_cur_rt)
        s_cur_rt->dirty = 1;

    if (!K.valid || !K.pcb || memcmp(K.pc, &pc, sizeof pc)) {
        ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_pcb, 0, NULL, &pc, 0, 0);
        memcpy(K.pc, &pc, sizeof pc);
    }
    if (!K.valid || K.ps != ps) {
        ID3D11DeviceContext_PSSetShader(C, ps, NULL, 0);
        K.ps = ps;
    }
    if (!K.valid || !K.pcb) {
        ID3D11DeviceContext_PSSetConstantBuffers(C, 0, 1, &s_pcb);
        K.pcb = 1;
    }
    /* A target is never sampled while bound: a pass that reads the surface it
     * draws into would be undefined on hardware too. */
    for (i = 0; i < 4; i++)
        if (srv[i] && s_cur_rt && srv[i] == s_cur_rt->srv)
            srv[i] = NULL;
    if (!K.valid || memcmp(K.srv, srv, sizeof K.srv)) {
        ID3D11DeviceContext_PSSetShaderResources(C, 0, 4, srv);
        memcpy(K.srv, srv, sizeof K.srv);
    }
    if (!K.valid || memcmp(K.smp, smp, sizeof K.smp)) {
        ID3D11DeviceContext_PSSetSamplers(C, 0, 4, smp);
        memcpy(K.smp, smp, sizeof K.smp);
    }
    {
        ID3D11RasterizerState *want = rs_for(rs);
        if (!K.valid || K.rs_ptr != want) {
            ID3D11DeviceContext_RSSetState(C, want);
            K.rs_ptr = want;
        }
    }
    {
        /* Widescreen: a draw that samples a render target is the game
         * re-drawing its own frame (the miasma veil, heat haze). The frame
         * already spans the full target, so this draw keeps the identity
         * mapping; squeezed, the copy lands offset against the live scene
         * -- a double image.
         *
         * A draw into a sub-screen offscreen target (the flashlight's
         * 256x256 shadow map, and any other small intermediate buffer) must
         * also keep the identity mapping. The squeeze is a screen-aspect
         * correction; applied to a light-space shadow map it shifts the map's
         * x against the light-matrix texture coordinates the beam samples it
         * with -- which our x-only squeeze never touches -- so the cone
         * collapses to a thin line and the glow dies. The screen targets are
         * 640 wide; anything narrower is not the screen. */
        int offscreen_small = s_cur_rt && s_cur_rt->w < 512;
        float ws = (!s_draw_samples_rt && !offscreen_small
                    && xbox_GfxConfig()->widescreen) ? 0.75f : 0.0f;
        if (s_draw_samples_rt)
            s_glow_fb++;
        if (s_vcb_data[6] != ws) {
            s_vcb_data[6] = ws;
            k_vcb(s_vcb_data);
        }
    }
    return 1;
}

/* The last single point drawn (the title's visibility-test points), and how
 * many more visibility reports to describe in the log (F9 sets it). */
float g_d3d_last_point[4];
volatile int g_vis_verbose;

static void d3d_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v, uint32_t count)
{
    uint32_t done = 0;
    /* F9: every small draw (the flare quads: save orbs, lens flares) in
     * surface pixels, to check where the title put them against where
     * their anchor renders. */
    if (s_dbg.log >= 1 && count <= 8 && count >= 3) {
        float x0 = v[0].pos[0], x1 = x0, y0 = v[0].pos[1], y1 = y0;
        uint32_t k;
        for (k = 1; k < count; k++) {
            if (v[k].pos[0] < x0) x0 = v[k].pos[0];
            if (v[k].pos[0] > x1) x1 = v[k].pos[0];
            if (v[k].pos[1] < y0) y0 = v[k].pos[1];
            if (v[k].pos[1] > y1) y1 = v[k].pos[1];
        }
        /* sprites only: a hand-sized box on the 640x480 surface */
        if (x1 - x0 < 120.0f && y1 - y0 < 120.0f) {
            static uint32_t lines, lframe;
            if (lframe != s_frame) { lframe = s_frame; lines = 0; }
            if (lines++ < 200)
                fprintf(stderr, "  [SPRITE %u] t0 %08X %ux%u  n%u  box %.1f,%.1f..%.1f,%.1f  w %.4f\n",
                        s_dbg.dix ? s_dbg.dix - 1 : 0,
                        st->tex[0].valid ? st->tex[0].offset : 0,
                        st->tex[0].valid ? st->tex[0].width : 0,
                        st->tex[0].valid ? st->tex[0].height : 0,
                        count, x0, y0, x1, y1, v[0].pos[3]);
        }
    }

    if (st->topo == 1 && count == 1)
        memcpy(g_d3d_last_point, v[0].pos, sizeof g_d3d_last_point);
    if (count && flash_on()) {
        flash_note(st, v[0].pos, sizeof(Nv2aGpuVertex) / 4, v[0].d0, count);
        wrec_note(st, v[0].pos, sizeof(Nv2aGpuVertex) / 4, v[0].d0, count);
        wrec_uv(v, count);
    }
    if (s_dbg.log == 1 && count) {
        uint32_t k, last = count < 4 ? count : 4;
        fprintf(stderr, "  [VTX] n %u topo %u:", count, st->topo);
        for (k = 0; k < last; k++)
            fprintf(stderr, " (%.0f %.0f %.3f w%.3f c%.2f %.2f %.2f %.2f)", v[k].pos[0], v[k].pos[1], v[k].pos[2],
                    v[k].pos[3], v[k].d0[0], v[k].d0[1], v[k].d0[2], v[k].d0[3]);
        fprintf(stderr, "\n");
    }

    if (!count || !setup_pixels(st))
        return;
    if (K.il != s_layout) { ID3D11DeviceContext_IASetInputLayout(C, s_layout); K.il = s_layout; }
    set_topo(st->topo);
    if (K.vs != s_vs) { ID3D11DeviceContext_VSSetShader(C, s_vs, NULL, 0); K.vs = s_vs; }
    if (K.cb_vsh != 1) { ID3D11DeviceContext_VSSetConstantBuffers(C, 0, 1, &s_vcb); K.cb_vsh = 1; }
    K.vb = NULL;                       /* rebound below each chunk */

    /* Vertices through a ring buffer, in triangle-aligned chunks. */
    while (done < count) {
        UINT stride = sizeof(Nv2aGpuVertex), off = 0, n = count - done;
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (n > s_vb_cap) n = s_vb_cap - s_vb_cap % 6;
        if (s_vb_pos + n > s_vb_cap) { s_vb_pos = 0; how = D3D11_MAP_WRITE_DISCARD; }
        if (FAILED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)s_vb, 0, how, 0, &m)))
            return;
        memcpy((Nv2aGpuVertex *)m.pData + s_vb_pos, v + done, (size_t)n * sizeof *v);
        ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)s_vb, 0);
        ID3D11DeviceContext_IASetVertexBuffers(C, 0, 1, &s_vb, &stride, &off);
        ID3D11DeviceContext_Draw(C, n, s_vb_pos);
        s_vb_pos += n;
        done += n;
    }
    S.draws++;
    S.verts += count;
}

/* ---- vertex programs as vertex shaders ------------------------------------
 *
 * The NV2A program, instruction for instruction, as HLSL: the same reads,
 * the same MAC and ILU arithmetic (run_mac / run_ilu in nv2a_vsh_interp.c),
 * the same write rules (both units read before either writes; an ILU op
 * paired with a MAC op writes R1), the same output defaults and clamps.
 * Then the executor's own post-processing: w = +0 becomes 1, fog_factor,
 * and the clip planes raster_xf_clipped applies (w above 1e-3, and screen z
 * at or beyond the viewport's z offset) as clip distances. */

static uint32_t vf(const uint32_t *w, int dw, int lo, int width)
{
    return (w[dw] >> lo) & ((1u << width) - 1u);
}

static void vs_src(Buf *b, const uint32_t *w, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char base[32];

    if (which == 0) {
        neg = vf(w, 1, 8, 1); swz = vf(w, 1, 0, 8);
        mux = vf(w, 2, 26, 2); reg = vf(w, 2, 28, 4);
    } else if (which == 1) {
        neg = vf(w, 2, 25, 1); swz = vf(w, 2, 17, 8);
        mux = vf(w, 2, 11, 2); reg = vf(w, 2, 13, 4);
    } else {
        neg = vf(w, 2, 10, 1); swz = vf(w, 2, 2, 8);
        mux = vf(w, 3, 28, 2);
        reg = (vf(w, 2, 0, 2) << 2) | vf(w, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(base, sizeof base, "R%u", reg <= 12 ? reg : 0u);   /* 13..15 read R0 */
    else if (mux == 2)
        snprintf(base, sizeof base, "i.v%u", vf(w, 1, 9, 4));
    else if (vf(w, 3, 1, 1))
        snprintf(base, sizeof base, "CR(a0 + %u)", vf(w, 1, 13, 8));
    else
        snprintf(base, sizeof base, "c[%u]", vf(w, 1, 13, 8) < 192 ? vf(w, 1, 13, 8) : 0);
    bput(b, "%s(%s.%c%c%c%c)", neg ? "-" : "", base,
         sw[(swz >> 6) & 3], sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
}

static const char *mask_str(uint32_t m)
{
    static char t[5];
    int n = 0;
    if (m & 8) t[n++] = 'x';
    if (m & 4) t[n++] = 'y';
    if (m & 2) t[n++] = 'z';
    if (m & 1) t[n++] = 'w';
    t[n] = 0;
    return t;
}

static char *gen_vs(const Nv2aGpuVsh *vs, int clipd)
{
    Buf b = { (char *)malloc(32768), 0, 32768 };
    uint32_t k, n;

    bput(&b,
         "cbuffer V : register(b0) { float4 vp; float4 vx; float4 fogp; };\n"
         "cbuffer K : register(b1) { float4 c[192]; };\n"
         "float4 CR(int i) { return (i >= 0 && i < 192) ? c[i] : c[0]; }\n"
         "struct VI {");
    for (k = 0; k < 16; k++)
        if (vs->in_mask >> k & 1)
            bput(&b, " float4 v%u : ATTR%u;", k, k);
    bput(&b, " };\n"
         "struct VO { float4 p : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
         "  noperspective float fog : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
         "  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;%s };\n"
         "float fogf(float d) {\n"
         "  if (fogp.x == 0) return 1;\n"
         "  int mode = (int)fogp.y; float f;\n"
         "  if (mode == 0x800 || mode == 0x802) f = fogp.z + exp2(d * fogp.w * 16.0) - 1.5;\n"
         "  else if (mode == 0x801 || mode == 0x803) f = fogp.z + exp2(-d * d * fogp.w * fogp.w * 32.0) - 1.5;\n"
         "  else f = fogp.z + d * fogp.w - 1.0;\n"
         "  if (mode == 0x802 || mode == 0x803 || mode == 0x804) f = abs(f);\n"
         "  if ((asuint(f) & 0x7FFFFFFF) > 0x7F800000) f = 1;\n"
         "  return saturate(f); }\n"
         "VO vs(VI i) {\n"
         "  float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0,\n"
         "         R8 = 0, R9 = 0, R10 = 0, R11 = 0, R12 = 0;\n"
         "  float4 O1 = 0, O2 = 0, O3 = float4(0,0,0,1), O4 = float4(0,0,0,1), O5 = 0,\n"
         "         O6 = 0, O7 = 0, O8 = 0, O9 = float4(0,0,0,1), O10 = float4(0,0,0,1),\n"
         "         O11 = float4(0,0,0,1), O12 = float4(0,0,0,1);\n"
         "  int a0 = 0;\n", clipd ? " float3 cd : SV_ClipDistance0;" : "");
    n = vs->nins;
    for (k = 0; k < n; k++) {
        const uint32_t *w = vs->prog[k];
        uint32_t mac = vf(w, 1, 21, 4), ilu = vf(w, 1, 25, 3);
        uint32_t mmask = vf(w, 3, 24, 4), imask = vf(w, 3, 16, 4), tdst = vf(w, 3, 20, 4);
        uint32_t omask = vf(w, 3, 12, 4), oaddr = vf(w, 3, 3, 8);
        uint32_t reads = (mac ? 1u : 0u) | ((mac == 2 || (mac >= 4 && mac <= 12)) ? 2u : 0u)
                       | ((mac == 3 || mac == 4 || ilu) ? 4u : 0u);
        uint32_t it = mac ? 1u : tdst;
        if (!mac && !ilu)
            goto next;
        bput(&b, "  { float4 A = 0, B = 0, C = 0, m = 0, u = 0;\n");
        if (reads & 1) { bput(&b, "    A = "); vs_src(&b, w, 0); bput(&b, ";\n"); }
        if (reads & 2) { bput(&b, "    B = "); vs_src(&b, w, 1); bput(&b, ";\n"); }
        if (reads & 4) { bput(&b, "    C = "); vs_src(&b, w, 2); bput(&b, ";\n"); }
        switch (mac) {
        case 0:  break;
        case 1:  bput(&b, "    m = A;\n"); break;
        case 2:  bput(&b, "    m = A * B;\n"); break;
        case 3:  bput(&b, "    m = A + C;\n"); break;
        case 4:  bput(&b, "    m = A * B + C;\n"); break;
        case 5:  bput(&b, "    m = dot(A.xyz, B.xyz).xxxx;\n"); break;
        case 6:  bput(&b, "    m = (dot(A.xyz, B.xyz) + B.w).xxxx;\n"); break;
        case 7:  bput(&b, "    m = dot(A, B).xxxx;\n"); break;
        case 8:  bput(&b, "    m = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9:  bput(&b, "    m = float4(A.x < B.x ? A.x : B.x, A.y < B.y ? A.y : B.y,"
                          " A.z < B.z ? A.z : B.z, A.w < B.w ? A.w : B.w);\n"); break;
        case 10: bput(&b, "    m = float4(A.x >= B.x ? A.x : B.x, A.y >= B.y ? A.y : B.y,"
                          " A.z >= B.z ? A.z : B.z, A.w >= B.w ? A.w : B.w);\n"); break;
        case 11: bput(&b, "    m = float4(A < B);\n"); break;
        case 12: bput(&b, "    m = float4(A >= B);\n"); break;
        case 13: bput(&b, "    m = A; a0 = (int)floor(A.x + 0.001);\n"); break;
        default: bput(&b, "    m = A;\n"); break;
        }
        switch (ilu) {
        case 0:  break;
        case 1:  bput(&b, "    u = C;\n"); break;
        case 2:  bput(&b, "    u = (C.x == 0 ? asfloat(0x7F800000) : 1.0 / C.x).xxxx;\n"); break;
        case 3:  bput(&b, "    { float f = 1.0 / C.x;\n"
                          "      if (abs(f) < 5.42101e-20) f = f < 0 ? -5.42101e-20 : 5.42101e-20;\n"
                          "      if (abs(f) > 1.884467e19) f = f < 0 ? -1.884467e19 : 1.884467e19;\n"
                          "      u = f.xxxx; }\n"); break;
        case 4:  bput(&b, "    u = rsqrt(abs(C.x)).xxxx;\n"); break;
        case 5:  bput(&b, "    { float fl = floor(C.x); u = float4(exp2(fl), C.x - fl, exp2(C.x), 1.0); }\n"); break;
        case 6:  bput(&b, "    { float ax = abs(C.x);\n"
                          "      if (ax == 0) u = float4(asfloat(0xFF800000), 1.0, asfloat(0xFF800000), 1.0);\n"
                          "      else { float e = floor(log2(ax)); u = float4(e, ax / exp2(e), log2(ax), 1.0); } }\n"); break;
        case 7:  bput(&b, "    { float nl = max(C.x, 0), nh = max(C.y, 0), pw = clamp(C.w, -127.9961, 127.9961);\n"
                          "      u = float4(1.0, nl, C.x > 0 ? pow(nh, pw) : 0.0, 1.0); }\n"); break;
        default: break;
        }
        if (mac && mac != 13 && mmask && tdst <= 12)
            bput(&b, "    R%u.%s = m.%s;\n", tdst, mask_str(mmask), mask_str(mmask));
        if (ilu && imask && it <= 12)
            bput(&b, "    R%u.%s = u.%s;\n", it, mask_str(imask), mask_str(imask));
        if (omask && vf(w, 3, 11, 1)) {
            const char *src = vf(w, 3, 2, 1) ? "u" : "m";
            if (oaddr == 0)
                bput(&b, "    R12.%s = %s.%s;\n", mask_str(omask), src, mask_str(omask));
            else if (oaddr < 13)
                bput(&b, "    O%u.%s = %s.%s;\n", oaddr, mask_str(omask), src, mask_str(omask));
        }
        bput(&b, "  }\n");
    next:
        if (vf(w, 3, 0, 1))
            break;
    }
    /* The far plane. A title parks a sprite it does not want seen just
     * past it -- Fatal Frame's save-point glow sits at z = 1.007 when its
     * anchor is out of view -- and counts on the hardware to throw the
     * sprite away. Depth clipping is off here for the shadow pass's sake,
     * so such a sprite was depth-clamped and drawn instead: a save orb
     * hanging in the sky, in 4:3 as in widescreen. A clip distance cuts it
     * at the far plane alone, leaving the near plane and the w <= 0 rule
     * untouched. A little give, as the near plane has: plenty of geometry
     * sits exactly at far. RECOMP_FAR_CLIP=0 restores the old behaviour. */
    static int farclip = -1;
    if (farclip < 0) {
        const char *ef = getenv("RECOMP_FAR_CLIP");
        farclip = !(ef && *ef == '0');
    }
    bput(&b,
         "  VO o;\n"
         "  float4 P = R12;\n"
         "  if (P.w == 0 && asuint(P.w) == 0) P.w = 1;\n"
         "  float wv = P.w;\n"
         "  float wsx = (vx.z != 0 && abs(wv - 1.0) > 1e-4) ? vx.z : 1.0;\n"
         "  o.p = float4((P.x * vp.x - 1.0) * wsx * wv, (1.0 - P.y * vp.y) * wv, P.z * vp.z * wv, wv);\n"
         "  o.d0 = saturate(O3); o.d1 = saturate(O4);\n"
         "  o.fog = fogf(O5.x);\n"
         "  o.t0 = O9; o.t1 = O10; o.t2 = O11; o.t3 = O12;\n"
         "%s"
         "  return o; }\n",
         /* The near plane with a little give: 2D sprites and text sit exactly
          * on it (z == the viewport's z offset), and the program's rounding
          * put a corner a hair in front of it on some frames -- a random
          * letter or panel piece clipped away, the load menu's flicker. */
         !clipd ? ""
         : farclip ? "  o.cd = float3(wv - 1e-3, vx.y != 0 ? (P.z - vx.x + 1e-2 + abs(vx.x) * 1e-5) * wv : 1.0,\n"
                     "                (1.0 + 1e-3 - P.z * vp.z) * wv);\n"
         : "  o.cd = float3(wv - 1e-3, vx.y != 0 ? (P.z - vx.x + 1e-2 + abs(vx.x) * 1e-5) * wv : 1.0, 1.0);\n");
    return b.p;
}

typedef struct {
    uint64_t hash;
    uint32_t nins, in_mask;
    ID3D11VertexShader *vs;
    ID3D11InputLayout *il;
    int failed;
} VsEnt;

static VsEnt *s_vsc;
static int s_nvsc, s_cap_vsc;
static ID3D11Buffer *s_kcb, *s_vb2, *s_ib2;
static UINT s_vb2_cap, s_vb2_pos, s_ib2_cap, s_ib2_pos;     /* bytes */
static float s_kcb_data[192][4];
static int s_kcb_valid;

static VsEnt *get_vs(const Nv2aGpuVsh *vs)
{
    int i;
    VsEnt *e;
    char *src;
    ID3DBlob *blob;

    for (i = 0; i < s_nvsc; i++)
        if (s_vsc[i].hash == vs->hash && s_vsc[i].nins == vs->nins
            && s_vsc[i].in_mask == vs->in_mask)
            return s_vsc[i].failed ? NULL : &s_vsc[i];
    if (s_nvsc == s_cap_vsc) {
        s_cap_vsc = s_cap_vsc ? s_cap_vsc * 2 : 32;
        s_vsc = (VsEnt *)realloc(s_vsc, (size_t)s_cap_vsc * sizeof *s_vsc);
    }
    e = &s_vsc[s_nvsc++];
    memset(e, 0, sizeof *e);
    e->hash = vs->hash; e->nins = vs->nins; e->in_mask = vs->in_mask;
    {
        /* RECOMP_GPU_CLIPDIST=0: the vertex programs without clip distances,
         * as on a compiler that lacks them -- to tell whether a picture
         * fault lives in that path. */
        static int clipd = -1;
        if (clipd < 0) {
            const char *e2 = getenv("RECOMP_GPU_CLIPDIST");
            clipd = !(e2 && *e2 == '0');
            if (!clipd)
                fprintf(stderr, "[GPU-D3D11] clip distances off (RECOMP_GPU_CLIPDIST=0)\n");
        }
        src = gen_vs(vs, clipd);
        blob = clipd ? compile(src, "vs", "vs_4_0") : NULL;
        if (!clipd) {
            free(src);
            src = NULL;
        }
    }
    if (!blob) {
        /* A compiler without SV_ClipDistance (wine's): no exact clip planes,
         * the GPU's own clipping only. */
        free(src);
        src = gen_vs(vs, 0);
        blob = compile(src, "vs", "vs_4_0");
    }
    if (getenv("RECOMP_GPU_VSH_DUMP"))
        fprintf(stderr, "[GPU-D3D11] vertex program %016llX (%u instructions):\n%s\n",
                (unsigned long long)vs->hash, vs->nins, src);
    free(src);
    if (blob) {
        D3D11_INPUT_ELEMENT_DESC il[16];
        UINT k, n = 0;
        static const char *sem = "ATTR";
        for (k = 0; k < 16; k++)
            if (vs->in_mask >> k & 1) {
                il[n].SemanticName = sem;
                il[n].SemanticIndex = k;
                il[n].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                il[n].InputSlot = 0;
                il[n].AlignedByteOffset = n * 16;
                il[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                il[n].InstanceDataStepRate = 0;
                n++;
            }
        if (FAILED(ID3D11Device_CreateVertexShader(D, ID3D10Blob_GetBufferPointer(blob),
                       ID3D10Blob_GetBufferSize(blob), NULL, &e->vs))
            || (n && FAILED(ID3D11Device_CreateInputLayout(D, il, n,
                       ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &e->il))))
            e->failed = 1;
        ID3D10Blob_Release(blob);
    } else {
        e->failed = 1;
    }
    S.vprogs++;
    return e->failed ? NULL : e;
}

/* Bytes into a dynamic ring buffer; returns the byte offset or -1. */
static long ring_put(ID3D11Buffer *buf, UINT cap, UINT *pos, const void *data, UINT bytes, UINT align)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT at = (*pos + align - 1) / align * align;
    if (bytes > cap)
        return -1;
    if (at + bytes > cap) { at = 0; how = D3D11_MAP_WRITE_DISCARD; }
    if (FAILED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)buf, 0, how, 0, &m)))
        return -1;
    memcpy((uint8_t *)m.pData + at, data, bytes);
    ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)buf, 0);
    *pos = at + bytes;
    return (long)at;
}

static int d3d_draw_vsh(const Nv2aGpuState *st, const Nv2aGpuVsh *vs,
                        const float *in, uint32_t nverts,
                        const uint32_t *idx, uint32_t nidx)
{
    VsEnt *e;
    long vo, io;
    UINT stride = vs->stride * 4, zero = 0;

    if (!s_vb2 || !nidx)
        return 0;
    e = get_vs(vs);
    if (!e) {
        extern void nv2a_drop_note(int, const char *, uint32_t, uint32_t);
        nv2a_drop_note(9, "d3d11: vertex program would not compile", (uint32_t)vs->hash, vs->nins);
        return 0;
    }
    if (!setup_pixels(st))
        return 1;                     /* nothing the CPU could do better */
    if (nverts && flash_on() && vs->stride >= 16) {
        flash_note(st, in, vs->stride, in + 12, nverts);
        wrec_note(st, in, vs->stride, in + 12, nverts);
    }
    if (s_dbg.log == 1 && nverts) {
        uint32_t k, n = vs->stride < 16 ? vs->stride : 16, v2;
        fprintf(stderr, "  [VSH] nv %u ni %u mask %X stride %u:", nverts, nidx, vs->in_mask, vs->stride);
        for (v2 = 0; v2 < nverts && v2 < 2; v2++) {
            fprintf(stderr, " [");
            for (k = 0; k < n; k++) fprintf(stderr, " %g", in[v2 * vs->stride + k]);
            fprintf(stderr, " ]");
        }
        fprintf(stderr, "\n");
        /* Full-precision vertex-program outputs for the dumped frame: the
         * shadow pass's stored depth is oPos.z, the lit pass's shadow
         * reference is oT2.z/oT2.w; printing both for real vertices pins
         * the depth law numerically instead of from screenshots. */
        if (vs->stride >= 64)
            for (v2 = 0; v2 < nverts && v2 < 3; v2++) {
                Nv2aVshOutput vo2;
                if (nv2a_vsh_run((const float (*)[4])(in + v2 * vs->stride), &vo2))
                    fprintf(stderr, "  [VSHRUN %u] pos %.4f %.4f %.6f %.6f t0 %.4f %.4f %.4f %.4f t1 %.4f %.4f %.4f %.4f t2 %.6f %.6f %.6f %.6f\n",
                            v2, vo2.pos[0], vo2.pos[1], vo2.pos[2], vo2.pos[3],
                            vo2.tex[0][0], vo2.tex[0][1], vo2.tex[0][2], vo2.tex[0][3],
                            vo2.tex[1][0], vo2.tex[1][1], vo2.tex[1][2], vo2.tex[1][3],
                            vo2.tex[2][0], vo2.tex[2][1], vo2.tex[2][2], vo2.tex[2][3]);
            }
        /* Shadow-law pairing: collect every shadow-pass vertex as a point in
         * light-map space (map x, map y, stored z, clip w), then for each
         * lit-pass vertex that samples the shadow map, find the nearest
         * collected point and print both sides.  Same map position means
         * same surface, so stored-vs-reference pairs fall out exactly. */
        if (vs->stride >= 64) {
            static float smp2[4096][4];
            static int   smn2, smpr;
            static uint32_t smfr = ~0u;
            int shadow_pass = st->zeta_bits == 16;
            int lit_pass = 0, ti;
            for (ti = 0; ti < 4 && !lit_pass; ti++)
                if (st->tex[ti].valid && st->tex[ti].color >= 0x2A && st->tex[ti].color <= 0x31
                    && st->zeta_bits != 16)
                    lit_pass = 1;
            if (smfr != s_frame) { smfr = s_frame; smn2 = 0; smpr = 0; }
            if (shadow_pass) {
                for (v2 = 0; v2 < nverts && smn2 < 4096; v2++) {
                    Nv2aVshOutput vo2;
                    if (nv2a_vsh_run((const float (*)[4])(in + v2 * vs->stride), &vo2)) {
                        smp2[smn2][0] = vo2.pos[0]; smp2[smn2][1] = vo2.pos[1];
                        smp2[smn2][2] = vo2.pos[2]; smp2[smn2][3] = vo2.pos[3];
                        smn2++;
                    }
                }
                fprintf(stderr, "  [SMPROBE] collected %d shadow verts\n", smn2);
            } else if (lit_pass && smn2) {
                /* One staging read of the live shadow map per dump frame, so
                 * each pair also shows the texel the GPU will compare with. */
                static uint32_t *gmap; static uint32_t gw, gh, gfr = ~0u;
                if (gfr != s_frame) {
                    DsEnt *de = find_ds(0x80E57000u);
                    gfr = s_frame; gw = gh = 0;
                    if (de && de->t) {
                        D3D11_TEXTURE2D_DESC td2, sd2;
                        ID3D11Texture2D *stg2 = NULL;
                        D3D11_MAPPED_SUBRESOURCE m2;
                        ID3D11Texture2D_GetDesc(de->t, &td2);
                        memset(&sd2, 0, sizeof sd2);
                        sd2.Width = td2.Width; sd2.Height = td2.Height;
                        sd2.MipLevels = 1; sd2.ArraySize = 1;
                        sd2.Format = td2.Format; sd2.SampleDesc.Count = 1;
                        sd2.Usage = D3D11_USAGE_STAGING; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        if (SUCCEEDED(ID3D11Device_CreateTexture2D(D, &sd2, NULL, &stg2))) {
                            ID3D11DeviceContext_CopyResource(C, (ID3D11Resource *)stg2, (ID3D11Resource *)de->t);
                            if (SUCCEEDED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)stg2, 0, D3D11_MAP_READ, 0, &m2))) {
                                uint32_t yy;
                                gw = td2.Width; gh = td2.Height;
                                gmap = (uint32_t *)realloc(gmap, (size_t)gw * gh * 4);
                                for (yy = 0; yy < gh; yy++)
                                    memcpy(gmap + (size_t)yy * gw,
                                           (const uint8_t *)m2.pData + (size_t)yy * m2.RowPitch,
                                           (size_t)gw * 4);
                                ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)stg2, 0);
                                fprintf(stderr, "  [SMGPU] staged live shadow map %ux%u\n", gw, gh);
                                /* Self-check: every collected shadow vertex
                                 * against the texel it rasterised into.  A
                                 * systematic deficit here means the write
                                 * path, not the lookup, carries the error. */
                                {
                                    int i3, nprint = 0, nok = 0, nlo = 0, nhi = 0;
                                    double dsum = 0; float dmn = 1e30f, dmx = -1e30f;
                                    for (i3 = 0; i3 < smn2; i3++) {
                                        float vx2 = smp2[i3][0], vy2 = smp2[i3][1], vz2 = smp2[i3][2];
                                        int gx3 = (int)(vx2 * s_scale), gy3 = (int)(vy2 * s_scale);
                                        float gz3, df;
                                        if (vx2 < 0 || vy2 < 0 || vx2 > 256 || vy2 > 256) continue;
                                        if (gx3 < 0 || gy3 < 0 || (uint32_t)gx3 >= gw || (uint32_t)gy3 >= gh) continue;
                                        if (vz2 < 0 || vz2 > 65535) continue;
                                        gz3 = (float)(gmap[(size_t)gy3 * gw + gx3] & 0xFFFFFFu) / 256.0f;
                                        df = gz3 - vz2;      /* + means map deeper (bias) */
                                        /* the texel may belong to a nearer surface; count only
                                         * roughly-matching ones for the bias statistic */
                                        if (df > -400 && df < 3000) { nok++; dsum += df; if (df < dmn) dmn = df; if (df > dmx) dmx = df; }
                                        else if (df <= -400) nlo++;
                                        else nhi++;
                                        if (nprint < 10 && (i3 % 97) == 0) {
                                            fprintf(stderr, "  [SMSELF] vert %.2f %.2f z %.1f | gpu %.1f diff %.1f\n",
                                                    vx2, vy2, vz2, gz3, df);
                                            nprint++;
                                        }
                                    }
                                    fprintf(stderr, "  [SMSELF] verts matching own texel (+bias window): %d, diff avg %.1f min %.1f max %.1f; nearer-surface %d, farther %d\n",
                                            nok, nok ? dsum / nok : 0.0, dmn, dmx, nlo, nhi);
                                }
                            }
                            ID3D11Texture2D_Release(stg2);
                        }
                    }
                }
                {
                    int dpr = 0;
                    float rmin = 1e30f, rmax = -1e30f;
                    int inmap = 0, nlit = 0, ndark = 0;
                {
                    static int pdone;
                    if (!pdone && st->tex[0].valid && st->tex[0].palette) {
                        const uint32_t *pal2 = (const uint32_t *)(gmem() + st->tex[0].palette);
                        int i4, n255 = 0, n0 = 0;
                        for (i4 = 0; i4 < 256; i4++) {
                            uint32_t a4 = pal2[i4] >> 24;
                            if (a4 == 0xFF) n255++; else if (!a4) n0++;
                        }
                        fprintf(stderr, "  [PAL0] tex %08X palette %08X alpha: ff x%d, 00 x%d, first4 %08X %08X %08X %08X\n",
                                st->tex[0].offset, st->tex[0].palette, n255, n0,
                                pal2[0], pal2[1], pal2[2], pal2[3]);
                        pdone = 1;
                    }
                }
                for (v2 = 0; v2 < nverts && smpr < 240; v2++) {
                    Nv2aVshOutput vo2;
                    if (!nv2a_vsh_run((const float (*)[4])(in + v2 * vs->stride), &vo2))
                        continue;
                    {
                        float q = vo2.tex[2][3];
                        float mx, my, ref;
                        int bi = -1, i2;
                        float bd = 16.0f;         /* 4px radius, squared */
                        if (q < 1e-3f && q > -1e-3f) continue;
                        mx = vo2.tex[2][0] / q; my = vo2.tex[2][1] / q;
                        ref = vo2.tex[2][2] / q;
                        if (mx < 0 || mx > 256 || my < 0 || my > 256) continue;
                        inmap++;
                        if (ref < rmin) rmin = ref;
                        if (ref > rmax) rmax = ref;
                        if (gmap && gw) {
                            int gx2 = (int)(mx * s_scale), gy2 = (int)(my * s_scale);
                            if (gx2 >= 0 && gy2 >= 0 && (uint32_t)gx2 < gw && (uint32_t)gy2 < gh) {
                                float gz2 = (float)(gmap[(size_t)gy2 * gw + gx2] & 0xFFFFFFu) / 256.0f;
                                if (gz2 >= ref) nlit++; else ndark++;
                            }
                        }
                        if (dpr >= 8) continue;
                        for (i2 = 0; i2 < smn2; i2++) {
                            float dx = smp2[i2][0] - mx, dy = smp2[i2][1] - my;
                            float d2 = dx * dx + dy * dy;
                            if (d2 < bd) { bd = d2; bi = i2; }
                        }
                        if (bi >= 0) {
                            float gz = -1;
                            if (gmap && gw) {
                                int gx = (int)(mx * s_scale), gy = (int)(my * s_scale);
                                if (gx >= 0 && gy >= 0 && (uint32_t)gx < gw && (uint32_t)gy < gh)
                                    gz = (float)(gmap[(size_t)gy * gw + gx] & 0xFFFFFFu) / 256.0f;
                            }
                            fprintf(stderr, "  [SMPAIR] map %.2f %.2f ref %.2f q %.3f | vert %.2f vw %.3f | gpu %.2f %s\n",
                                    mx, my, ref, q, smp2[bi][2], smp2[bi][3], gz,
                                    gz >= 0 ? (gz >= ref ? "LIT" : "dark") : "?");
                            smpr++; dpr++;
                        }
                    }
                }
                    if (inmap)
                        fprintf(stderr, "  [SMSTAT] %d verts in map, ref %.0f..%.0f, verdict lit %d dark %d\n",
                                inmap, rmin, rmax, nlit, ndark);
                }
            }
        }
    }
    /* Constants: re-uploaded only when they changed. */
    if (!s_kcb_valid || memcmp(s_kcb_data, vs->consts, sizeof s_kcb_data)) {
        memcpy(s_kcb_data, vs->consts, sizeof s_kcb_data);
        ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_kcb, 0, NULL, s_kcb_data, 0, 0);
        s_kcb_valid = 1;
        S.const_uploads++;
    }
    s_vcb_data[4] = vs->z_offset;
    s_vcb_data[5] = vs->near_z ? 1.0f : 0.0f;
    s_vcb_data[8] = vs->fog_enable ? 1.0f : 0.0f;
    s_vcb_data[9] = (float)vs->fog_mode;
    s_vcb_data[10] = vs->fog_param[0];
    s_vcb_data[11] = vs->fog_param[1];
    k_vcb(s_vcb_data);

    vo = stride ? ring_put(s_vb2, s_vb2_cap, &s_vb2_pos, in, nverts * stride, stride) : 0;
    io = ring_put(s_ib2, s_ib2_cap, &s_ib2_pos, idx, nidx * 4, 4);
    if (vo < 0 || io < 0) {
        extern void nv2a_drop_note(int, const char *, uint32_t, uint32_t);
        nv2a_drop_note(10, "d3d11: vertex/index ring overflow", nverts, nidx);
        return 0;
    }
    if (K.il != e->il) { ID3D11DeviceContext_IASetInputLayout(C, e->il); K.il = e->il; }
    set_topo(st->topo);
    if (stride && (K.vb != s_vb2 || K.vstride != stride)) {
        ID3D11DeviceContext_IASetVertexBuffers(C, 0, 1, &s_vb2, &stride, &zero);
        K.vb = s_vb2; K.vstride = stride;
    }
    if (K.ib != s_ib2) { ID3D11DeviceContext_IASetIndexBuffer(C, s_ib2, DXGI_FORMAT_R32_UINT, 0); K.ib = s_ib2; }
    if (K.vs != e->vs) { ID3D11DeviceContext_VSSetShader(C, e->vs, NULL, 0); K.vs = e->vs; }
    if (K.cb_vsh != 2) {
        ID3D11Buffer *cb[2] = { s_vcb, s_kcb };
        ID3D11DeviceContext_VSSetConstantBuffers(C, 0, 2, cb);
        K.cb_vsh = 2;
    }
    ID3D11DeviceContext_DrawIndexed(C, nidx, (UINT)(io / 4), stride ? (INT)(vo / stride) : 0);
    S.draws++;
    S.verts += nidx;
    S.vsh_draws++;
    return 1;
}

/* ---- presenting ------------------------------------------------------------
 *
 * The finished frame goes to the screen straight from the GPU: a swap chain
 * on the game window, the render target scaled into it (letterboxed, linear
 * filter), the F6 menu / FPS overlay composited on top. Nothing is read back
 * for that, so the internal resolution costs GPU time only.
 *
 * Guest memory still gets the frame, at the title's own size: a title can
 * read its front buffer, and the GDI window shows it when the GPU does not
 * present (no swap chain, or the video overlay plane in use). That copy is
 * asynchronous -- copied to a staging texture now, read the next frame -- so
 * the CPU never waits for the GPU to finish. */

static const char s_blit_src[] =
    "cbuffer B : register(b0) { float4 uv; };\n"
    "Texture2D T : register(t0); SamplerState S : register(s0);\n"
    "void vs(uint i : SV_VertexID, out float4 p : SV_Position, out float2 t : TEXCOORD0) {\n"
    "  float2 q = float2((i << 1) & 2, i & 2);\n"
    "  p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); t = q * uv.xy; }\n"
    "float4 ps(float4 p : SV_Position, float2 t : TEXCOORD0) : SV_Target { return T.Sample(S, t); }\n";

/* Post-processing: a soft-threshold bloom at half resolution plus a final
 * colour grade (contrast, saturation, bloom add). Driven by the EFFECTS tab
 * of the F6 menu (fx_bloom / fx_contrast / fx_saturation, all live). */
static const char s_fx_src[] =
    "cbuffer F : register(b0) { float4 a; float4 b; float4 e; float4 f; };\n"
    "Texture2D T : register(t0); SamplerState S : register(s0);\n"
    "Texture2D T1 : register(t1);\n"
    "float4 ps_thresh(float4 p : SV_Position, float2 t : TEXCOORD0) : SV_Target {\n"
    "  float3 c = T.Sample(S, t * a.xy).rgb;\n"
    "  float l = dot(c, float3(0.299, 0.587, 0.114));\n"
    "  float f = saturate((l - 0.55) / 0.45);\n"
    "  return float4(c * f * f, 1); }\n"
    "float4 ps_blur(float4 p : SV_Position, float2 t : TEXCOORD0) : SV_Target {\n"
    "  float3 c = T.Sample(S, t).rgb * 0.227;\n"
    "  c += (T.Sample(S, t + a.zw * 1.385).rgb + T.Sample(S, t - a.zw * 1.385).rgb) * 0.316;\n"
    "  c += (T.Sample(S, t + a.zw * 3.231).rgb + T.Sample(S, t - a.zw * 3.231).rgb) * 0.070;\n"
    "  return float4(c, 1); }\n"
    "float4 ps_grade(float4 p : SV_Position, float2 t : TEXCOORD0) : SV_Target {\n"
    "  float2 uv = t * a.xy;\n"
    "  float3 c;\n"
    "  if (f.z > 0) {\n"
    "    float2 d = (t - 0.5) * f.z * 0.012 * a.xy;\n"
    "    c.r = T.Sample(S, uv + d).r;\n"
    "    c.g = T.Sample(S, uv).g;\n"
    "    c.b = T.Sample(S, uv - d).b;\n"
    "  } else c = T.Sample(S, uv).rgb;\n"
    "  if (e.w > 0) {\n"
    "    float3 n = T.Sample(S, uv + float2(a.z, 0)).rgb\n"
    "             + T.Sample(S, uv - float2(a.z, 0)).rgb\n"
    "             + T.Sample(S, uv + float2(0, a.w)).rgb\n"
    "             + T.Sample(S, uv - float2(0, a.w)).rgb;\n"
    "    c = saturate(c + (c - n * 0.25) * e.w);\n"
    "  }\n"
    "  c *= b.w;\n"
    "  c = pow(saturate(c), 1.0 / max(e.x, 0.1));\n"
    "  c = (c - 0.5) * b.y + 0.5;\n"
    "  float l = dot(c, float3(0.299, 0.587, 0.114));\n"
    "  c = lerp(l.xxx, c, b.z);\n"
    "  c += float3(f.x, f.x * 0.2, -f.x) * 0.1;\n"
    "  c += T1.Sample(S, t).rgb * b.x;\n"
    "  if (e.y > 0) {\n"
    "    float2 q = t - 0.5;\n"
    "    c *= 1.0 - e.y * smoothstep(0.25, 0.75, dot(q, q) * 2.0);\n"
    "  }\n"
    "  if (e.z > 0) {\n"
    "    float g = frac(sin(dot(p.xy + f.w, float2(12.9898, 78.233))) * 43758.5453);\n"
    "    c += (g - 0.5) * e.z * 0.25;\n"
    "  }\n"
    "  if (f.y > 0)\n"
    "    c *= 1.0 - f.y * 0.35 * (0.5 + 0.5 * sin(p.y * 3.14159));\n"
    "  return float4(saturate(c), 1); }\n";

static ID3D11PixelShader *s_fx_thresh, *s_fx_blur, *s_fx_grade;
static ID3D11Buffer *s_fx_cb;
static ID3D11Texture2D *s_fx_tex[2];
static ID3D11RenderTargetView *s_fx_rtv[2];
static ID3D11ShaderResourceView *s_fx_srv[2];
static UINT s_fx_w, s_fx_h;

static ID3D11VertexShader *s_blit_vs;
static ID3D11PixelShader *s_blit_ps;
static ID3D11Buffer *s_blit_cb;
static ID3D11SamplerState *s_blit_lin, *s_blit_pt;
static ID3D11BlendState *s_blit_alpha;
static IDXGISwapChain *s_swap;
static ID3D11RenderTargetView *s_swap_rtv;
static UINT s_swap_w, s_swap_h;
static HWND s_swap_hwnd;
static int s_swap_failed;
static ID3D11Texture2D *s_ovl_tex;
static ID3D11ShaderResourceView *s_ovl_srv;
static uint32_t s_ovl_w, s_ovl_h;
static int s_ovl_seq = -1;
/* 1x copies for guest memory: render target (for a scaled frame), and two
 * staging textures used in turn. */
static RtEnt s_small;
static ID3D11Texture2D *s_stage[2];
static uint32_t s_stage_w[2], s_stage_h[2];
static struct { int valid; uint32_t va, pitch, bpp, w, h; } s_pend[2];
static int s_stage_i;

static int blit_init(void)
{
    static int done;
    ID3DBlob *b;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_BLEND_DESC bl;
    if (done) return s_blit_ps != NULL;
    done = 1;
    if (!(b = compile(s_blit_src, "vs", "vs_4_0"))) return 0;
    ID3D11Device_CreateVertexShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_vs);
    ID3D10Blob_Release(b);
    if (!(b = compile(s_blit_src, "ps", "ps_4_0"))) return 0;
    ID3D11Device_CreatePixelShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_ps);
    ID3D10Blob_Release(b);
    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.ByteWidth = 16;
    ID3D11Device_CreateBuffer(D, &bd, NULL, &s_blit_cb);
    bd.ByteWidth = 64;
    ID3D11Device_CreateBuffer(D, &bd, NULL, &s_fx_cb);
    if ((b = compile(s_fx_src, "ps_thresh", "ps_4_0"))) {
        ID3D11Device_CreatePixelShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_fx_thresh);
        ID3D10Blob_Release(b);
    }
    if ((b = compile(s_fx_src, "ps_blur", "ps_4_0"))) {
        ID3D11Device_CreatePixelShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_fx_blur);
        ID3D10Blob_Release(b);
    }
    if ((b = compile(s_fx_src, "ps_grade", "ps_4_0"))) {
        ID3D11Device_CreatePixelShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_fx_grade);
        ID3D10Blob_Release(b);
    }
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(D, &sd, &s_blit_lin);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(D, &sd, &s_blit_pt);
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(D, &bl, &s_blit_alpha);
    return s_blit_ps != NULL;
}

/* Draw `srv` (the region uv of it) into `rtv` at viewport x,y,w,h. */
static void blit(ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *srv,
                 float u, float v, float x, float y, float w, float h,
                 ID3D11SamplerState *smp, ID3D11BlendState *bs)
{
    D3D11_VIEWPORT vp = { x, y, w, h, 0, 1 };
    D3D11_RECT sc = { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
    float cb[4] = { u, v, 0, 0 };
    float bf[4] = { 0, 0, 0, 0 };
    ID3D11ShaderResourceView *none = NULL;
    D3D11_DEPTH_STENCIL_DESC dd;

    memset(&dd, 0, sizeof dd);
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DeviceContext_OMSetRenderTargets(C, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(C, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(C, 1, &sc);
    ID3D11DeviceContext_RSSetState(C, s_rs); K.rs_ptr = s_rs;
    ID3D11DeviceContext_OMSetBlendState(C, bs, bf, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(C, get_dss(&dd), 0);
    ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_blit_cb, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_IASetInputLayout(C, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(C, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); K.topo = 1;
    ID3D11DeviceContext_VSSetShader(C, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(C, 0, 1, &s_blit_cb);
    ID3D11DeviceContext_PSSetShader(C, s_blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(C, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(C, 0, 1, &smp);
    ID3D11DeviceContext_Draw(C, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(C, 0, 1, &none);
    k_reset();                      /* everything above bypassed the shadow */
}

/* blit with a chosen pixel shader, FX constants, and an optional second
 * texture; geometry and state otherwise as blit(). */
static void fx_pass(ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *t0,
                    ID3D11ShaderResourceView *t1, ID3D11PixelShader *ps,
                    float x, float y, float w, float h, const float fx[16])
{
    D3D11_VIEWPORT vp = { x, y, w, h, 0, 1 };
    D3D11_RECT sc = { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
    float cb[4] = { 1, 1, 0, 0 };
    ID3D11ShaderResourceView *none[2] = { NULL, NULL };
    D3D11_DEPTH_STENCIL_DESC dd;

    memset(&dd, 0, sizeof dd);
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DeviceContext_OMSetRenderTargets(C, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(C, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(C, 1, &sc);
    ID3D11DeviceContext_RSSetState(C, s_rs); K.rs_ptr = s_rs;
    ID3D11DeviceContext_OMSetBlendState(C, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(C, get_dss(&dd), 0);
    ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_blit_cb, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_fx_cb, 0, NULL, fx, 0, 0);
    ID3D11DeviceContext_IASetInputLayout(C, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(C, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); K.topo = 1;
    ID3D11DeviceContext_VSSetShader(C, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(C, 0, 1, &s_blit_cb);
    ID3D11DeviceContext_PSSetShader(C, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(C, 0, 1, &s_fx_cb);
    ID3D11DeviceContext_PSSetShaderResources(C, 0, 1, &t0);
    if (t1) ID3D11DeviceContext_PSSetShaderResources(C, 1, 1, &t1);
    ID3D11DeviceContext_PSSetSamplers(C, 0, 1, &s_blit_lin);
    ID3D11DeviceContext_Draw(C, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(C, 0, 2, none);
    k_reset();
}

static int fx_targets(UINT w, UINT h)
{
    int i;
    if (s_fx_w == w && s_fx_h == h && s_fx_srv[1])
        return 1;
    for (i = 0; i < 2; i++) {
        if (s_fx_srv[i]) { ID3D11ShaderResourceView_Release(s_fx_srv[i]); s_fx_srv[i] = NULL; }
        if (s_fx_rtv[i]) { ID3D11RenderTargetView_Release(s_fx_rtv[i]); s_fx_rtv[i] = NULL; }
        if (s_fx_tex[i]) { ID3D11Texture2D_Release(s_fx_tex[i]); s_fx_tex[i] = NULL; }
    }
    for (i = 0; i < 2; i++) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(D, &td, NULL, &s_fx_tex[i])))
            return 0;
        ID3D11Device_CreateRenderTargetView(D, (ID3D11Resource *)s_fx_tex[i], NULL, &s_fx_rtv[i]);
        ID3D11Device_CreateShaderResourceView(D, (ID3D11Resource *)s_fx_tex[i], NULL, &s_fx_srv[i]);
    }
    s_fx_w = w; s_fx_h = h;
    return s_fx_srv[1] != NULL;
}

static void swap_release(void)
{
    if (s_swap_rtv) { ID3D11RenderTargetView_Release(s_swap_rtv); s_swap_rtv = NULL; }
    if (s_swap) { IDXGISwapChain_Release(s_swap); s_swap = NULL; }
}

static int swap_ready(HWND hwnd)
{
    RECT c;
    UINT cw, ch;
    if (s_swap_failed || !hwnd || !blit_init())
        return 0;
    if (!GetClientRect(hwnd, &c))
        return 0;
    cw = (UINT)(c.right - c.left); ch = (UINT)(c.bottom - c.top);
    if (!cw || !ch)
        return 0;                        /* minimised */
    if (s_swap && s_swap_hwnd != hwnd)
        swap_release();
    if (!s_swap) {
        static const GUID iid_dev = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
        static const GUID iid_fac = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
        IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory *fa = NULL;
        DXGI_SWAP_CHAIN_DESC sd;
        HRESULT hr = E_FAIL;
        memset(&sd, 0, sizeof sd);
        sd.BufferDesc.Width = cw; sd.BufferDesc.Height = ch;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = hwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (SUCCEEDED(ID3D11Device_QueryInterface(D, &iid_dev, (void **)&xd))
            && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad))
            && SUCCEEDED(IDXGIAdapter_GetParent(ad, &iid_fac, (void **)&fa)))
            hr = IDXGIFactory_CreateSwapChain(fa, (IUnknown *)D, &sd, &s_swap);
        if (fa) {
            if (SUCCEEDED(hr))
                IDXGIFactory_MakeWindowAssociation(fa, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            IDXGIFactory_Release(fa);
        }
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
        if (FAILED(hr)) {
            fprintf(stderr, "[GPU-D3D11] no swap chain (%08lX): the window keeps drawing with GDI\n",
                    (unsigned long)hr);
            s_swap_failed = 1;
            s_swap = NULL;
            return 0;
        }
        s_swap_hwnd = hwnd;
        s_swap_w = s_swap_h = 0;
        fprintf(stderr, "[GPU-D3D11] presenting to the window from the GPU\n");
    }
    if (cw != s_swap_w || ch != s_swap_h || !s_swap_rtv) {
        ID3D11Texture2D *bb = NULL;
        static const GUID iid_tex = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
        if (s_swap_rtv) { ID3D11RenderTargetView_Release(s_swap_rtv); s_swap_rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets(C, 0, NULL, NULL);
        k_reset();
        if (s_swap_w && FAILED(IDXGISwapChain_ResizeBuffers(s_swap, 1, cw, ch, DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
            return 0;
        if (FAILED(IDXGISwapChain_GetBuffer(s_swap, 0, &iid_tex, (void **)&bb)))
            return 0;
        ID3D11Device_CreateRenderTargetView(D, (ID3D11Resource *)bb, NULL, &s_swap_rtv);
        ID3D11Texture2D_Release(bb);
        s_swap_w = cw; s_swap_h = ch;
    }
    return s_swap_rtv != NULL;
}

static void overlay_update(void)
{
    uint32_t w, h;
    int seq;
    const uint32_t *px;
    xbox_FramebufferWindowOverlayLock(1);
    px = xbox_FramebufferWindowOverlayImage(&w, &h, &seq);
    if (seq != s_ovl_seq) {
        s_ovl_seq = seq;
        if (px && (!s_ovl_tex || s_ovl_w != w || s_ovl_h != h)) {
            D3D11_TEXTURE2D_DESC td;
            if (s_ovl_srv) { ID3D11ShaderResourceView_Release(s_ovl_srv); s_ovl_srv = NULL; }
            if (s_ovl_tex) { ID3D11Texture2D_Release(s_ovl_tex); s_ovl_tex = NULL; }
            memset(&td, 0, sizeof td);
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(D, &td, NULL, &s_ovl_tex)))
                ID3D11Device_CreateShaderResourceView(D, (ID3D11Resource *)s_ovl_tex, NULL, &s_ovl_srv);
            s_ovl_w = w; s_ovl_h = h;
        }
        if (px && s_ovl_tex)
            ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_ovl_tex, 0, NULL, px, w * 4, 0);
        if (!px && s_ovl_srv) {
            ID3D11ShaderResourceView_Release(s_ovl_srv); s_ovl_srv = NULL;
            ID3D11Texture2D_Release(s_ovl_tex); s_ovl_tex = NULL;
        }
    }
    xbox_FramebufferWindowOverlayLock(0);
}

/* The frame to the window. Returns 0 if the GPU did not present it. */
static int present_window(RtEnt *rt, uint32_t w, uint32_t h)
{
    HWND hwnd = (HWND)xbox_FramebufferWindowHwnd();
    float cw, ch, dw, dh;
    float black[4] = { 0, 0, 0, 1 };
    static int off = -1;

    if (off < 0) {
        const char *e = getenv("RECOMP_GPU_PRESENT");
        off = e && e[0] == '0';
    }
    if (off || xbox_FramebufferWindowOverlayPlane() || !swap_ready(hwnd)) {
        xbox_FramebufferWindowSetGpuPresent(0);
        return 0;
    }
    cw = (float)s_swap_w; ch = (float)s_swap_h;
    {
        /* content aspect: the frame's own, or 16:9 with widescreen on */
        float aw = (float)w, ah = (float)h;
        if (xbox_GfxConfig()->widescreen) { aw = 16.0f; ah = 9.0f; }
        if (cw * ah <= ch * aw) { dw = cw; dh = cw * ah / aw; }
        else { dh = ch; dw = ch * aw / ah; }
    }
    ID3D11DeviceContext_ClearRenderTargetView(C, s_swap_rtv, black);
    {
        XboxGfxConfig *g = xbox_GfxConfig();
        float su = (float)(w * s_scale) / rt->pw, sv = (float)(h * s_scale) / rt->ph;
        float bx = (float)(int)((cw - dw) / 2), by = (float)(int)((ch - dh) / 2);
        static float fx_time;
        int fx_on = (g->fx_bloom > 0 || g->fx_contrast != 100 || g->fx_saturation != 100
                     || g->fx_brightness != 100 || g->fx_gamma != 100 || g->fx_vignette > 0
                     || g->fx_grain > 0 || g->fx_sharpen > 0 || g->fx_temp != 0
                     || g->fx_scanlines > 0 || g->fx_ca > 0)
                    && s_fx_grade && s_fx_blur && s_fx_thresh;
        float cg[16];
        fx_time += 1.0f;
        if (fx_time > 4096.0f) fx_time = 0;
        cg[0] = su; cg[1] = sv;
        cg[2] = su / (float)(rt->pw ? rt->pw : 1) * 2.0f;
        cg[3] = sv / (float)(rt->ph ? rt->ph : 1) * 2.0f;
        cg[4] = (float)g->fx_bloom / 100.0f;
        cg[5] = (float)g->fx_contrast / 100.0f;
        cg[6] = (float)g->fx_saturation / 100.0f;
        cg[7] = (float)g->fx_brightness / 100.0f;
        cg[8] = (float)g->fx_gamma / 100.0f;
        cg[9] = (float)g->fx_vignette / 100.0f;
        cg[10] = (float)g->fx_grain / 100.0f;
        cg[11] = (float)g->fx_sharpen / 100.0f * 1.5f;
        cg[12] = (float)g->fx_temp / 100.0f;
        cg[13] = (float)g->fx_scanlines / 100.0f;
        cg[14] = (float)g->fx_ca / 100.0f;
        cg[15] = fx_time;
        if (fx_on && g->fx_bloom > 0 && fx_targets((UINT)(dw / 2) < 8 ? 8 : (UINT)(dw / 2),
                                                   (UINT)(dh / 2) < 8 ? 8 : (UINT)(dh / 2))) {
            float fxw = (float)s_fx_w, fxh = (float)s_fx_h;
            float c0[16] = { su, sv, 0, 0 };
            float cH[16] = { 1, 1, 1.0f / fxw, 0 };
            float cV[16] = { 1, 1, 0, 1.0f / fxh };
            fx_pass(s_fx_rtv[0], rt->srv, NULL, s_fx_thresh, 0, 0, fxw, fxh, c0);
            fx_pass(s_fx_rtv[1], s_fx_srv[0], NULL, s_fx_blur, 0, 0, fxw, fxh, cH);
            fx_pass(s_fx_rtv[0], s_fx_srv[1], NULL, s_fx_blur, 0, 0, fxw, fxh, cV);
            fx_pass(s_swap_rtv, rt->srv, s_fx_srv[0], s_fx_grade, bx, by,
                    (float)(int)dw, (float)(int)dh, cg);
        } else if (fx_on) {
            cg[4] = 0;
            fx_pass(s_swap_rtv, rt->srv, NULL, s_fx_grade, bx, by,
                    (float)(int)dw, (float)(int)dh, cg);
        } else {
            blit(s_swap_rtv, rt->srv, su, sv, bx, by,
                 (float)(int)dw, (float)(int)dh, s_blit_lin, NULL);
        }
    }
    overlay_update();
    if (s_ovl_srv)
        blit(s_swap_rtv, s_ovl_srv, 1.0f, 1.0f, 0, 0, (float)s_ovl_w, (float)s_ovl_h,
             s_blit_pt, s_blit_alpha);
    IDXGISwapChain_Present(s_swap, 0, 0);
    xbox_FramebufferWindowSetGpuPresent(1);
    return 1;
}

/* The 1x copy for guest memory: scaled frames are shrunk on the GPU first;
 * the copy lands in staging now and is read next time round. */
static void guest_copy(RtEnt *rt, uint32_t va, uint32_t pitch, uint32_t bpp,
                       uint32_t w, uint32_t h)
{
    int cur = s_stage_i, prev = s_stage_i ^ 1;
    ID3D11Resource *src = (ID3D11Resource *)rt->t;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t sw = rt->w, sh = rt->h, y, x;

    if (s_scale > 1) {
        if (!s_small.t || s_small.w != rt->w || s_small.h != rt->h) {
            uint32_t keep = s_scale;
            if (s_small.t) free_rt(&s_small);
            s_scale = 1;                     /* allocate at 1x */
            make_rt(&s_small, rt->w, rt->h);
            s_scale = keep;
        }
        if (!s_small.t || !blit_init())
            return;
        blit(s_small.rtv, rt->srv, 1.0f, 1.0f, 0, 0, (float)rt->w, (float)rt->h, s_blit_pt, NULL);
        src = (ID3D11Resource *)s_small.t;
    }
    if (!s_stage[cur] || s_stage_w[cur] != sw || s_stage_h[cur] != sh) {
        D3D11_TEXTURE2D_DESC td;
        if (s_stage[cur]) ID3D11Texture2D_Release(s_stage[cur]);
        s_stage[cur] = NULL;
        memset(&td, 0, sizeof td);
        td.Width = sw; td.Height = sh; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(D, &td, NULL, &s_stage[cur])))
            return;
        s_stage_w[cur] = sw; s_stage_h[cur] = sh;
    }
    ID3D11DeviceContext_CopyResource(C, (ID3D11Resource *)s_stage[cur], src);
    s_pend[cur].valid = 1; s_pend[cur].va = va; s_pend[cur].pitch = pitch;
    s_pend[cur].bpp = bpp; s_pend[cur].w = w; s_pend[cur].h = h;
    {
        /* Synchronous by default: the frame is in guest memory before the
         * title can look at its front buffer. Fatal Frame copies the front
         * buffer with the CPU to save each photo the Camera Obscura takes;
         * read a frame late, the photo was of an earlier (often black) frame.
         * RECOMP_GPU_ASYNC_READBACK=1 goes back to reading a frame late,
         * which never waits for the GPU. (Fatal Frame port.) */
        static int async = -1;
        if (async < 0) {
            const char *e = getenv("RECOMP_GPU_ASYNC_READBACK");
            async = e && *e == '1';
        }
        if (!async) {
            if (s_pend[prev].valid)
                s_pend[prev].valid = 0;       /* older than this frame: drop */
            prev = cur;                       /* read this one now */
        } else {
            s_stage_i = prev;
        }
    }
    /* Last time's copy: finished on the GPU by now, so no wait. */
    if (!s_pend[prev].valid || !s_stage[prev])
        return;
    if (FAILED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)s_stage[prev], 0, D3D11_MAP_READ, 0, &m)))
        return;
    {
        uint8_t *dst = gmem() + s_pend[prev].va;
        uint32_t pw = s_pend[prev].w < s_stage_w[prev] ? s_pend[prev].w : s_stage_w[prev];
        uint32_t ph = s_pend[prev].h < s_stage_h[prev] ? s_pend[prev].h : s_stage_h[prev];
        for (y = 0; y < ph; y++) {
            const uint8_t *srow = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            uint8_t *row = dst + (size_t)y * s_pend[prev].pitch;
            if (s_pend[prev].bpp == 4) {
                memcpy(row, srow, (size_t)pw * 4);
            } else {
                for (x = 0; x < pw; x++) {
                    uint32_t c = ((const uint32_t *)srow)[x];
                    ((uint16_t *)row)[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                                    | ((c >> 3) & 0x001F));
                }
            }
        }
    }
    ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)s_stage[prev], 0);
    s_pend[prev].valid = 0;
    S.readbacks++;
}

/* A render target into guest memory, now: copied at 1x into its own staging
 * texture and mapped, which waits for the GPU. Only targets drawn since
 * their last readback, and only linear ones (a swizzled target would need
 * swizzling on the way back). (Fatal Frame port.) */
static void rt_readback(RtEnt *e)
{
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Resource *src;
    uint32_t w, h, y, x;

    if (!e->t || !e->dirty || e->swizzled || !e->pitch || (e->bpp != 4 && e->bpp != 2))
        return;
    if ((e->va & 0x0FFFFFFFu) < 0x00010000u)
        return;                  /* the shadow pass's dummy surface sits at
                                  * physical 0: never write that back */
    if (e->pitch != e->shown_pitch) {
        /* Only what reaches the screen is copied back: the front and back
         * buffers, which the CPU reads (photos). Offscreen targets -- a
         * shadow map, a 256x256 16-bit pass the title runs in the back
         * buffer's memory at a different pitch -- stay on the GPU; copying
         * those back is how pixels ended up in DirectSound's memory (crashes
         * opening doors and on the stairs). */
        e->dirty = 0;
        return;
    }
    if (!(e->va & 0x80000000u)) {
        /* Not in the contiguous window: the offset was not memory the
         * runtime handed out for the GPU, so as a guest address it is heap.
         * Copying back there wrote pixels over DirectSound's voice lists
         * (crashes in CommitDeferredSettings). Never write it. */
        static int said;
        if (said < 8) {
            said++;
            fprintf(stderr, "[GPU-D3D11] not copying target %08X (%ux%u pitch %u) back: not GPU memory\n",
                    e->va, e->w, e->h, e->pitch);
        }
        e->dirty = 0;
        return;
    }
    if (getenv("RECOMP_FLIP_LOG"))
        fprintf(stderr, "[RB] copy %08X rows %u frame %u%s\n", e->va, e->gh, s_frame,
                e == s_cur_rt ? " (bound)" : "");
    e->dirty = 0;
    src = (ID3D11Resource *)e->t;
    if (s_scale > 1) {
        if (!s_small.t || s_small.w != e->w || s_small.h != e->h) {
            uint32_t keep = s_scale;
            if (s_small.t) free_rt(&s_small);
            s_scale = 1;
            make_rt(&s_small, e->w, e->h);
            s_scale = keep;
        }
        if (!s_small.t || !blit_init())
            return;
        blit(s_small.rtv, e->srv, 1.0f, 1.0f, 0, 0, (float)e->w, (float)e->h, s_blit_pt, NULL);
        src = (ID3D11Resource *)s_small.t;
    }
    if (!e->stage) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = e->w; td.Height = e->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(D, &td, NULL, &e->stage))) {
            e->stage = NULL;
            return;
        }
    }
    ID3D11DeviceContext_CopyResource(C, (ID3D11Resource *)e->stage, src);
    if (FAILED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)e->stage, 0, D3D11_MAP_READ, 0, &m)))
        return;
    /* Only the rows the title drew into, never the target's full (grown)
     * height: the same address can later be a shorter surface, and the rows
     * past it are someone else's memory (heap corruption on loading a
     * save). */
    w = e->w;
    if (w * e->bpp > e->pitch) w = e->pitch / e->bpp;
    h = e->gh < e->h ? e->gh : e->h;
    if (h < e->h) {
        static int said;
        if (said < 8) {
            said++;
            fprintf(stderr, "[GPU-D3D11] readback of %08X limited to %u of its %u rows (pitch %u)\n",
                    e->va, h, e->h, e->pitch);
        }
    }
    e->gh = 0;
    if (!h) {
        ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)e->stage, 0);
        return;
    }
    {
        uint8_t *dst = gmem() + e->va;
        for (y = 0; y < h; y++) {
            const uint8_t *srow = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            uint8_t *row = dst + (size_t)y * e->pitch;
            if (e->bpp == 4) {
                memcpy(row, srow, (size_t)w * 4);
            } else {
                for (x = 0; x < w; x++) {
                    uint32_t c = ((const uint32_t *)srow)[x];
                    ((uint16_t *)row)[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                                    | ((c >> 3) & 0x001F));
                }
            }
        }
    }
    ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)e->stage, 0);
    S.readbacks++;
    if (getenv("RECOMP_DBG_RB") && (S.readbacks % 97) == 0)
        fprintf(stderr, "[RB] %08X %ux%u rows %u pitch %u cur %d frame %u\n", e->va, e->w, e->h, h,
                e->pitch, e == s_cur_rt, s_frame);
}

/* NV09F image blit on the GPU: from a render target into another (made if
 * need be, seeded from guest memory). Fatal Frame copies its frame into a
 * spare buffer this way each frame and blends it back for its glow and
 * afterimage effects. (Fatal Frame port.) */
static int d3d_blit(uint32_t src_va, uint32_t src_pitch, uint32_t dst_va, uint32_t dst_pitch,
                    uint32_t bpp, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                    uint32_t w, uint32_t h)
{
    RtEnt *src = find_rt(src_va), *dst;
    Nv2aGpuState st;
    D3D11_BOX box;
    if (s_dbg.log == 1)
        fprintf(stderr, "  [BLIT] %08X -> %08X %ux%u (after draw %u)%s\n", src_va, dst_va, w, h,
                s_dbg.dix, src ? "" : " src not a target");

    (void)src_pitch;
    if (!src || src->swizzled || src_va == dst_va || !w || !h
        || (src->bpp && src->bpp != bpp)) {
        RT_TRACE(dst_va, "blit refused (src %s %ux%u): dst entry freed",
                 !src ? "missing" : src->swizzled ? "swizzled" : "bad", w, h);
        static unsigned said;
        s_glow_blitfail++;
        if (said < 40) {
            said++;
            fprintf(stderr, "[GLOW] blit refused: %08X -> %08X %ux%u (%s); target entry freed\n",
                    src_va, dst_va, w, h,
                    !src ? "src not a target" : src->swizzled ? "src swizzled"
                    : src_va == dst_va ? "same surface" : !w || !h ? "empty" : "bpp mismatch");
        }
        dst = find_rt(dst_va);
        if (dst) free_rt(dst);
        return 0;
    }
    if (sx >= src->w || sy >= src->h)
        return 0;
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    memset(&st, 0, sizeof st);
    st.color_va = dst_va; st.pitch = dst_pitch; st.bpp = bpp;
    st.clip_x = 0; st.clip_y = 0; st.clip_w = dx + w; st.clip_h = dy + h;
    dst = get_rt(&st);
    if (!dst)
        return 0;
    src = find_rt(src_va);                 /* get_rt may have moved entries */
    if (!src)
        return 0;
    box.left = sx * s_scale; box.top = sy * s_scale; box.front = 0;
    box.right = (sx + w) * s_scale; box.bottom = (sy + h) * s_scale; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(C, (ID3D11Resource *)dst->t, 0,
                                              dx * s_scale, dy * s_scale, 0,
                                              (ID3D11Resource *)src->t, 0, &box);
    dst->used = s_frame;
    dst->gm_hash = rt_gm_hash(dst->va, rt_gm_range(dst));
    s_glow_blits++;
    /* Not marked for copying back: the title blits its frame aside to sample
     * it (glow, afterimage), which stays on the GPU; copying the 1.2 MB back
     * at every fence was most of the cost of the fence sync. */
    return 1;
}

/* At a fence: targets the title has finished with -- not the one still
 * bound, which it is mid-way through drawing (a frame has eight fences;
 * copying the half-drawn frame back at each cost 13 ms a frame and showed as
 * flicker). The bound one is copied at its flip, or at the first fence after
 * the title moves on from it. */
static void d3d_sync_guest(void)
{
    int i;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].t && s_rt[i].dirty && &s_rt[i] != s_cur_rt)
            rt_readback(&s_rt[i]);
}

static ID3D11InfoQueue *s_iq;
/* The debug layer's messages since the last call, warnings and worse. Each
 * distinct message is printed the first few times only. */
static void d3d_debug_drain(void)
{
    static struct { int id; int n; } seen[256];
    static int nseen, total;
    UINT64 i, n;
    if (!s_iq)
        return;
    n = ID3D11InfoQueue_GetNumStoredMessages(s_iq);
    for (i = 0; i < n; i++) {
        SIZE_T len = 0;
        D3D11_MESSAGE *m;
        int k;
        if (FAILED(ID3D11InfoQueue_GetMessage(s_iq, i, NULL, &len)) || !len)
            continue;
        m = (D3D11_MESSAGE *)malloc(len);
        if (!m) continue;
        if (SUCCEEDED(ID3D11InfoQueue_GetMessage(s_iq, i, m, &len))
            && m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            for (k = 0; k < nseen && seen[k].id != (int)m->ID; k++) ;
            if (k == nseen && nseen < 256) { seen[k].id = (int)m->ID; seen[k].n = 0; nseen++; }
            if (k < 256 && seen[k].n++ < 5 && total++ < 2000)
                fprintf(stderr, "[D3DDBG] frame %u sev %d id %d: %.*s\n", s_frame, (int)m->Severity,
                        (int)m->ID, (int)m->DescriptionByteLength, m->pDescription);
        }
        free(m);
    }
    ID3D11InfoQueue_ClearStoredMessages(s_iq);
}

static void d3d_present_sync(uint32_t va, uint32_t pitch, uint32_t bpp,
                             uint32_t w, uint32_t h)
{
    RtEnt *rt = find_rt(va);

    if (!rt || rt->swizzled || (bpp != 4 && bpp != 2))
        return;
    if (w > rt->w) w = rt->w;
    if (h > rt->h) h = rt->h;
    d3d_debug_drain();
    {
        /* RECOMP_DRAWDIP_LOG (on unless 0): a frame whose draw count differs
         * from both neighbours, which agree -- the title sent fewer (or more)
         * draws for that one frame. Says whether a vanished object was never
         * submitted or was drawn and lost. */
        static int on = -1;
        static uint32_t cnt[3], fr[3], pbe[3], last_dix_frame, logged;
        extern uint32_t nv2a_pb_take_ends(void);
        if (on < 0) { const char *e = getenv("RECOMP_DRAWDIP_LOG"); on = !(e && *e == '0'); }
        if (on && s_frame != last_dix_frame) {
            uint32_t n = s_dbg.dix < 2048 ? s_dbg.dix : 2048;
            last_dix_frame = s_frame;
            cnt[0] = cnt[1]; cnt[1] = cnt[2]; cnt[2] = s_dbg.dix;
            fr[0] = fr[1]; fr[1] = fr[2]; fr[2] = s_frame;
            pbe[0] = pbe[1]; pbe[1] = pbe[2]; pbe[2] = nv2a_pb_take_ends();
            memcpy(s_sig[0], s_sig[1], sizeof s_sig[0]);
            memcpy(s_sig[1], s_sig[2], sizeof s_sig[0]);
            memcpy(s_sig[2], g_sig_cur, n * 4);
            memcpy(s_tex3[0], s_tex3[1], sizeof s_tex3[0]);
            memcpy(s_tex3[1], s_tex3[2], sizeof s_tex3[0]);
            memcpy(s_tex3[2], g_tex_cur, n * 4);
            if (cnt[0] && cnt[0] == cnt[2] && cnt[1] != cnt[0] && logged < 400) {
                /* The draws in the frame before that have no partner in this
                 * frame (multiset difference): the ones the title skipped. */
                static uint8_t used[2048];
                uint32_t a = cnt[0] < 2048 ? cnt[0] : 2048;
                uint32_t b = cnt[1] < 2048 ? cnt[1] : 2048;
                uint32_t i2, j2, shown = 0;
                logged++;
                memset(used, 0, b);
                fprintf(stderr, "[DRAWDIP] frame %u: %u draws, frames either side %u; title sent %u, either side %u/%u\n",
                        fr[1], cnt[1], cnt[0], pbe[1], pbe[0], pbe[2]);
                for (i2 = 0; i2 < a; i2++) {
                    for (j2 = 0; j2 < b; j2++)
                        if (!used[j2] && s_sig[1][j2] == s_sig[0][i2]) { used[j2] = 1; break; }
                    if (j2 == b && shown < 6) {
                        shown++;
                        fprintf(stderr, "  [DRAWDIP] missing here: the frame before's draw %u, tex %08X sig %08X\n",
                                i2, s_tex3[0][i2], s_sig[0][i2]);
                    }
                }
            }
        }
    }
    if (!w || !h)
        return;
    if (flash_on() || (s_dbg.on > 0 && GetFileAttributesA("gpulogall") != INVALID_FILE_ATTRIBUTES)) {
        /* debug: the presented frame's mean brightness, to find a flash */
        static ID3D11Texture2D *st; static UINT sw, sh;
        D3D11_TEXTURE2D_DESC td;
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11Texture2D_GetDesc(rt->t, &td);
        if (!st || sw != td.Width || sh != td.Height) {
            if (st) ID3D11Texture2D_Release(st);
            st = NULL;
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0; td.MipLevels = 1;
            ID3D11Device_CreateTexture2D(D, &td, NULL, &st);
            sw = td.Width; sh = td.Height;
        }
        if (st) {
            ID3D11DeviceContext_CopyResource(C, (ID3D11Resource *)st, (ID3D11Resource *)rt->t);
            if (SUCCEEDED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
                uint64_t sum = 0; uint32_t n = 0, x, y, nwhite = 0;
                for (y = 0; y < h * s_scale; y += 8)
                    for (x = 0; x < w * s_scale; x += 8) {
                        const uint8_t *p = (const uint8_t *)m.pData + y * m.RowPitch + x * 4;
                        sum += p[0] + p[1] + p[2]; n += 3;
                        if (p[0] >= 245 && p[1] >= 245 && p[2] >= 245)
                            nwhite++;
                    }
                ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)st, 0);
                {
                    unsigned mean = n ? (unsigned)(sum / n) : 0;
                    static unsigned prev, dumps;
                    if (s_dbg.on > 0 && GetFileAttributesA("gpulogall") != INVALID_FILE_ATTRIBUTES)
                        fprintf(stderr, "[PRESENT %u] mean %u\n", s_frame, mean);
                    {
                        /* a burst of near-white: the one-frame UI corruption */
                        static uint32_t prevw = 0xFFFFFFFFu; static unsigned wdumps;
                        if (flash_on() && prevw != 0xFFFFFFFFu && nwhite >= prevw + 40 && wdumps < 20) {
                            wdumps++;
                            fprintf(stderr, "[WHITE] frame %u: near-white 8x8 samples %u -> %u; all draws of the last frame and this one:\n",
                                    s_frame, prevw, nwhite);
                            wrec_dump(s_frcur ^ 1, s_frame - 1);
                            wrec_dump(s_frcur, s_frame);
                            {
                                extern void xbox_pb_stageprog_dump(void);
                                xbox_pb_stageprog_dump();
                            }
                        }
                        prevw = nwhite;
                    }
                    if (flash_on() && prev && ((mean >= prev * 2 && mean >= prev + 15) || (getenv("RECOMP_FLASH_TEST") && s_frame == 6000)) && dumps < 30) {
                        dumps++;
                        fprintf(stderr, "[FLASH] frame %u: brightness %u -> %u; this frame's and the last frame's glare and full-screen draws:\n",
                                s_frame, prev, mean);
                        flash_dump(s_frcur ^ 1, s_frame - 1);
                        flash_dump(s_frcur, s_frame);
                    }
                    prev = mean;
                }
            }
        }
    }
    present_window(rt, w, h);
    {
        static int async = -1;
        if (async < 0) {
            const char *e = getenv("RECOMP_GPU_ASYNC_READBACK");
            async = e && *e == '1';
        }
        if (async)
            guest_copy(rt, va, pitch, bpp, w, h);
        else {
            if (getenv("RECOMP_FLIP_LOG"))
                fprintf(stderr, "[RB] frame %u va %08X dirty %u pitch %u/%u gh %u h %u\n",
                        s_frame, va, rt->dirty, rt->pitch, pitch, rt->gh, h);
            rt->shown_pitch = pitch;
            rt_readback(rt);              /* nothing if a semaphore did it */
        }
    }
}

/* The F6 menu's choices, taken between frames. A new internal resolution
 * drops every render target and depth buffer: the title redraws them from
 * the next frame on, at the new size. */
static void apply_gfx_settings(void)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    static int reload_seen = -1;
    uint32_t want = g->scale < 1 ? 1u : (g->scale > 4 ? 4u : (uint32_t)g->scale);
    int i;

    if (want != s_scale) {
        for (i = 0; i < MAX_RT; i++)
            if (s_rt[i].t) free_rt(&s_rt[i]);
        for (i = 0; i < 16; i++)
            if (s_ds[i].t) {
                ID3D11DepthStencilView_Release(s_ds[i].dsv);
                if (s_ds[i].srv) ID3D11ShaderResourceView_Release(s_ds[i].srv);
                ID3D11Texture2D_Release(s_ds[i].t);
                memset(&s_ds[i], 0, sizeof s_ds[i]);
            }
        if (s_small.t) free_rt(&s_small);
        for (i = 0; i < 2; i++) {
            if (s_stage[i]) { ID3D11Texture2D_Release(s_stage[i]); s_stage[i] = NULL; }
            s_pend[i].valid = 0;
        }
        k_reset();
        ID3D11DeviceContext_OMSetRenderTargets(C, 0, NULL, NULL);
        s_cur_rt = NULL;
        s_scale = want;
        fprintf(stderr, "[GPU-D3D11] internal resolution %ux (%ux%u)\n", s_scale,
                640 * s_scale, 480 * s_scale);
    }
    g->scale_applied = (int)s_scale;
    {
        /* The glow pipeline's pulse, once every ~5 s: how many draws since
         * the last report sampled a render target (the afterglow, bloom and
         * veil passes) and how many frame blits fed them. Zeroes here while
         * the flashlight looks dead mean the title stopped issuing them. */
        static uint32_t last;
        if (s_frame - last >= 300) {
            fprintf(stderr, "[GLOW] frame %u: %u rt-sampling draws, %u frame blits, %u blit refusals, %u plain-texture reads in the last %u frames\n",
                    s_frame, s_glow_fb, s_glow_blits, s_glow_blitfail, s_glow_plain, s_frame - last);
            last = s_frame;
            s_glow_fb = s_glow_blits = 0;
            s_glow_blitfail = s_glow_plain = 0;
        }
    }
    /* Widescreen: perspective draws squeeze x by 3/4 in the render target
     * and the window stretches 4:3 to 16:9 -- net: the same proportions
     * with a third more world at the sides (Hor+). Screen-space draws
     * (w == 1: UI, text) keep their place. */
    s_vcb_data[6] = g->widescreen ? 0.75f : 0.0f;
    if (reload_seen != g->reload_seq) {
        reload_seen = g->reload_seq;
        tex_cache_clear();
        repl_scan();
    }
}

static void dbg_ds_dump(void)
{
    static int on = -1, n;
    int i;
    if (on < 0) { const char *e = getenv("RECOMP_DBG_DS"); on = e && *e == '1'; }
    if (!on || (n++ % 120)) return;
    for (i = 0; i < 16; i++) {
        DsEnt *z = &s_ds[i];
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *stg;
        D3D11_MAPPED_SUBRESOURCE m;
        uint32_t x, y, lo = 0xFFFFFF, hi = 0, st0 = 0, hist[16];
        memset(hist, 0, sizeof hist);
        if (!z->t) continue;
        ID3D11Texture2D_GetDesc(z->t, &td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(D, &td, NULL, &stg))) continue;
        ID3D11DeviceContext_CopyResource(C, (ID3D11Resource *)stg, (ID3D11Resource *)z->t);
        if (SUCCEEDED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
            for (y = 0; y < td.Height; y++)
                for (x = 0; x < td.Width; x++) {
                    uint32_t v = ((uint32_t *)((uint8_t *)m.pData + y * m.RowPitch))[x];
                    uint32_t d = v & 0xFFFFFF;
                    if (d < lo) lo = d;
                    if (d > hi) hi = d;
                    if (v >> 24) st0++;
                    { float fv; memcpy(&fv, &v, 4); if (fv >= 0 && fv <= 1) hist[(int)(fv * 15.999f)]++; }
                }
            ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)stg, 0);
        }
        ID3D11Texture2D_Release(stg);
        fprintf(stderr, "[DS] va %08X %ux%u depth %06X..%06X stencil-nonzero %u hist", z->va,
                td.Width, td.Height, lo, hi, st0);
        for (x = 0; x < 16; x++) fprintf(stderr, " %u", hist[x]);
        fprintf(stderr, "\n");
    }
}

static void d3d_end_frame(void)
{
    dbg_ds_dump();
    static DWORD last;
    DWORD now = GetTickCount();
    s_frame++;
    dbg_poll();
    apply_gfx_settings();
    if (s_stats < 0) {
        const char *e = getenv("RECOMP_GPU_STATS");
        s_stats = e && *e && *e != '0';
    }
    if (s_stats && now - last > 5000) {
        fprintf(stderr, "[GPU-D3D11] frame %u: %u draws (%u by vertex program) %u verts %u clears,"
                " %u uploads (%.1f MB), %u render-target reads, %d pixel shaders, %d vertex"
                " programs, %u constant uploads\n",
                s_frame, S.draws, S.vsh_draws, S.verts, S.clears, S.tex_uploads,
                (double)S.tex_bytes / 1048576.0, S.tex_hits_rt, s_nps, s_nvsc, S.const_uploads);
        memset(&S, 0, sizeof S);
        last = now;
    }
}

/* ---- visibility tests ------------------------------------------------------
 *
 * One D3D11 occlusion query per test. The title reads the count right after
 * GET_REPORT, so the result is waited for here (the queries are short and
 * few: Fatal Frame issues a handful a frame, for the ghosts in the camera's
 * viewfinder). Counts are in host samples; at a raised internal resolution
 * they are scaled back to the title's pixels. (Fatal Frame port.) */
static ID3D11Query *s_occl_q;
static int s_occl_running;

static void d3d_occlusion_begin(void)
{
    if (!s_occl_q) {
        D3D11_QUERY_DESC qd;
        qd.Query = D3D11_QUERY_OCCLUSION;
        qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(D, &qd, &s_occl_q))) {
            s_occl_q = NULL;
            return;
        }
    }
    if (s_occl_running)
        ID3D11DeviceContext_End(C, (ID3D11Asynchronous *)s_occl_q);
    ID3D11DeviceContext_Begin(C, (ID3D11Asynchronous *)s_occl_q);
    s_occl_running = 1;
}

static uint32_t d3d_occlusion_end(void)
{
    UINT64 n = 0;
    HRESULT hr;
    int spins = 0;

    if (!s_occl_q || !s_occl_running)
        return 0;
    ID3D11DeviceContext_End(C, (ID3D11Asynchronous *)s_occl_q);
    s_occl_running = 0;
    while ((hr = ID3D11DeviceContext_GetData(C, (ID3D11Asynchronous *)s_occl_q, &n,
                                             sizeof n, 0)) == S_FALSE) {
        if (++spins > 200000) {             /* a lost device: do not hang */
            static int nt;
            if (nt++ < 50)
                fprintf(stderr, "[VIS] frame %u: visibility query timed out, reported 0\n", s_frame);
            return 0;
        }
        if ((spins & 63) == 0)
            SwitchToThread();
    }
    if (FAILED(hr))
        return 0;
    if (s_scale > 1) {
        /* Ceiling, not floor. The title's visibility tests draw single-pixel
         * points (ghosts in the viewfinder, lens flares): one point is one
         * host sample at ANY internal resolution, and 1/scale^2 floored to 0
         * read as "never visible" -- the Camera Obscura stopped detecting
         * ghosts the moment the internal resolution went above 1x. Any
         * surviving sample must report at least one title pixel; larger
         * areas still scale back proportionally. */
        UINT64 ss = (UINT64)s_scale * s_scale;
        UINT64 scaled = (n + ss - 1) / ss;
        static int nlog;
        if (n && nlog < 60) {
            fprintf(stderr, "[VIS] raw %llu -> %llu (scale %d)\n",
                    (unsigned long long)n, (unsigned long long)scaled, s_scale);
            nlog++;
        }
        n = scaled;
    }
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

static const Nv2aGpuBackend s_backend = {
    d3d_clear, d3d_draw, d3d_present_sync, d3d_end_frame, d3d_draw_vsh,
    d3d_occlusion_begin, d3d_occlusion_end, d3d_sync_guest, d3d_blit
};

int nv2a_d3d11_init(void)
{
    static const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "FOG",      0, DXGI_FORMAT_R32_FLOAT,          0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 68,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 84,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 100, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    const char *e = getenv("RECOMP_GPU");
    D3D_FEATURE_LEVEL fl;
    ID3DBlob *b;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    if (e && *e == '0')
        return 0;
    {
        /* Shader model 4: feature level 10.0 or better. An older adapter
         * keeps the software rasteriser rather than failing shader by shader. */
        static const D3D_FEATURE_LEVEL want[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        /* RECOMP_GPU_D3D_DEBUG=1: Direct3D's debug layer, its warnings and
         * errors copied into the log once a frame (d3d_debug_drain). Needs
         * Windows' "Graphics Tools" feature; without it, a normal device. */
        const char *dbg = getenv("RECOMP_GPU_D3D_DEBUG");
        const char *warp = getenv("RECOMP_GPU_WARP");
        UINT flags = (dbg && *dbg == '1') ? D3D11_CREATE_DEVICE_DEBUG : 0;
        /* RECOMP_GPU_WARP=1: Microsoft's software Direct3D 11 (WARP) in place
         * of the graphics card -- the same runtime and shaders on the CPU, to
         * tell a driver's behaviour from our own. Slower. */
        if (warp && *warp == '1') {
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, flags, want, 4,
                                   D3D11_SDK_VERSION, &D, &fl, &C);
            if (hr == E_INVALIDARG)
                hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, flags, want + 1, 3,
                                       D3D11_SDK_VERSION, &D, &fl, &C);
            fprintf(stderr, "[GPU-D3D11] WARP software device: %s (%08lX)\n",
                    SUCCEEDED(hr) ? "in use" : "unavailable, using the graphics card", (unsigned long)hr);
            if (SUCCEEDED(hr))
                goto created;
        }
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, want, 4,
                               D3D11_SDK_VERSION, &D, &fl, &C);
        if (hr == E_INVALIDARG)        /* the 11.1 runtime is missing (Windows 7) */
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, want + 1, 3,
                                   D3D11_SDK_VERSION, &D, &fl, &C);
        if (FAILED(hr) && flags) {
            fprintf(stderr, "[GPU-D3D11] debug layer not available (%08lX); normal device\n", (unsigned long)hr);
            flags = 0;
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 4,
                                   D3D11_SDK_VERSION, &D, &fl, &C);
            if (hr == E_INVALIDARG)
                hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want + 1, 3,
                                       D3D11_SDK_VERSION, &D, &fl, &C);
        }
    created:
        if (SUCCEEDED(hr) && flags) {
            if (SUCCEEDED(ID3D11Device_QueryInterface(D, &IID_ID3D11InfoQueue, (void **)&s_iq)))
                fprintf(stderr, "[GPU-D3D11] debug layer on: its messages follow as [D3DDBG]\n");
        }
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] no hardware device (%08lX); using the CPU rasteriser\n",
                (unsigned long)hr);
        return 0;
    }
    if (sizeof(Nv2aGpuVertex) != 116) {
        fprintf(stderr, "[GPU-D3D11] unexpected vertex size %u\n", (unsigned)sizeof(Nv2aGpuVertex));
        return 0;
    }
    b = compile(s_vs_src, "vs", "vs_4_0");
    if (!b) return 0;
    ID3D11Device_CreateVertexShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_vs);
    hr = ID3D11Device_CreateInputLayout(D, il, sizeof il / sizeof il[0],
                                        ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), &s_layout);
    ID3D10Blob_Release(b);
    if (FAILED(hr)) return 0;
    b = compile(s_clear_src, "vs", "vs_4_0");
    if (!b) return 0;
    ID3D11Device_CreateVertexShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_clear_vs);
    ID3D10Blob_Release(b);
    b = compile(s_clear_src, "ps", "ps_4_0");
    if (!b) return 0;
    ID3D11Device_CreatePixelShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_clear_ps);
    ID3D10Blob_Release(b);

    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = sizeof s_vcb_data;
    ID3D11Device_CreateBuffer(D, &bd, NULL, &s_vcb);
    bd.ByteWidth = (sizeof(PsConst) + 15) & ~15u;
    ID3D11Device_CreateBuffer(D, &bd, NULL, &s_pcb);
    bd.ByteWidth = 32;
    ID3D11Device_CreateBuffer(D, &bd, NULL, &s_ccb);
    s_vb_cap = 3 * 40000;
    bd.ByteWidth = s_vb_cap * sizeof(Nv2aGpuVertex);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(D, &bd, NULL, &s_vb))) return 0;
    /* Translated vertex programs: raw inputs and triangle indices. */
    s_vb2_cap = 32u << 20;
    bd.ByteWidth = s_vb2_cap;
    if (FAILED(ID3D11Device_CreateBuffer(D, &bd, NULL, &s_vb2))) s_vb2 = NULL;
    s_ib2_cap = 8u << 20;
    bd.ByteWidth = s_ib2_cap;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(D, &bd, NULL, &s_ib2))) s_vb2 = NULL;
    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = sizeof s_kcb_data;
    if (FAILED(ID3D11Device_CreateBuffer(D, &bd, NULL, &s_kcb))) s_vb2 = NULL;

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;     /* the software path draws both faces */
    rd.DepthClipEnable = FALSE;        /* ...and does not clip at the far plane */
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(D, &rd, &s_rs);

    {
        XboxGfxConfig *g = xbox_GfxConfig();
        IDXGIDevice *xd = NULL;
        IDXGIAdapter *ad = NULL;
        static const GUID iid_dxgidev = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
        strcpy(g->adapter, "Direct3D 11");
        if (SUCCEEDED(ID3D11Device_QueryInterface(D, &iid_dxgidev, (void **)&xd))
            && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g->adapter,
                                    sizeof g->adapter, NULL, NULL);
        }
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
        g->gpu_active = 1;
        fprintf(stderr, "[GPU-D3D11] adapter: %s\n", g->adapter);
    }
    nv2a_gpu_backend_register(&s_backend);
    fprintf(stderr, "[GPU-D3D11] NV2A drawing on the host GPU (feature level %X)\n", (unsigned)fl);
    return 1;
}

#else  /* !_WIN32 */
int nv2a_d3d11_init(void) { return 0; }
#endif
