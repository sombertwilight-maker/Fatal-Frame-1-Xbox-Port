/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * ponytail: register-combiner state and texture stages 1-3 are not passed on:
 * a back end sees stage 0 and the diffuse colour. Extend Nv2aBatch when that
 * matters rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_combiner.h"

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    /* SET_POLY_OFFSET_FILL_ENABLE and SET_POLYGON_OFFSET_SCALE_FACTOR/BIAS,
     * glPolygonOffset's factor and units; units are the depth surface's own
     * (SET_SURFACE_FORMAT zeta: 1 Z16, 2 Z24S8). A shadow-map pass leans on
     * them to keep a surface from shadowing itself. */
    uint32_t poly_fill, zeta_fmt;
    uint32_t control0;          /* SET_CONTROL0: bit 16 Z_PERSPECTIVE (w-buffer) */
    float    poly_factor, poly_units;
} Nv2aRenderState;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
} Nv2aBatch;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Register a back end that must synchronize a surface into guest memory
 * before the flip's window snapshot or framebuffer dump. present_sync runs
 * synchronously on the executor thread and must complete pending GPU work and
 * readback before returning; it must not present a window. flip still runs
 * after the snapshot and dump. A NULL present_sync disables synchronization;
 * a NULL backend removes both registrations. A later nv2a_backend_register
 * call replaces both registrations. */
void nv2a_backend_register_with_present_sync(const Nv2aBackend *backend,
                                             void (*present_sync)(void));

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* ---- Full-pipeline GPU back end -------------------------------------------
 *
 * The interface above hands a back end stage 0 and the diffuse colour only.
 * This one hands over everything the CPU rasteriser itself uses per pixel:
 * all four texture stages, both vertex colours, fog, the register-combiner
 * program and the surface/zeta state -- so a host-GPU renderer (nv2a_d3d11.c)
 * can draw exactly what the software path would. When one is registered the
 * executor stops rasterising on the CPU altogether. (Fatal Frame port.) */

typedef struct {
    uint32_t offset;                /* guest address of texel (0,0)            */
    uint32_t width, height;
    uint32_t pitch;                 /* linear formats                          */
    uint32_t color;                 /* NV097 colour-format code                */
    uint32_t addr_u, addr_v;        /* wrap mode per axis                      */
    uint32_t palette;               /* CLUT address (P8)                       */
    uint32_t levels, filter, border;
    int      border_color, cube, valid;
    int      normalized;            /* coordinates arrive in [0,1]             */
    uint32_t bytes;                 /* bytes of texel data (all faces/levels)  */
} Nv2aGpuTexture;

/* A transformed vertex: pos is (screen x, screen y, screen z, w), the rest as
 * the vertex program wrote them; fog is already the fog factor. 116 bytes. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog;
    float tex[4][4];
} Nv2aGpuVertex;

typedef struct {
    uint32_t color_va, pitch, bpp;
    uint32_t swizzled, swz_w, swz_h;
    uint32_t clip_x, clip_y, clip_w, clip_h;     /* real pixels             */
    uint32_t zeta_va, zeta_bits;                 /* 16 or 24                */
    Nv2aRenderState rs;
    int      use_rc, textured, bilinear;
    Nv2aCombiner rc;
    uint32_t clip_plane_mode, fog_color;
    Nv2aGpuTexture tex[4];
    uint32_t topo;                  /* 0 triangle list, 1 points, 2 line list */
    uint32_t shadow_func;           /* SET_SHADOW_COMPARE_FUNC (0 never .. 7 always) */
} Nv2aGpuState;

/* A vertex program for the back end to run itself (optional path). */
typedef struct {
    uint64_t hash;
    uint32_t nins, in_mask, stride;
    uint32_t prog[136][4];
    float    consts[192][4];
    float    z_offset;
    int      near_z;
    uint32_t fog_enable, fog_mode;
    float    fog_param[2];
} Nv2aGpuVsh;

typedef struct {
    /* flags: CLEAR_SURFACE bits; rect: x0, y0, x1, y1 in real pixels */
    void (*clear)(const Nv2aGpuState *st, uint32_t flags, uint32_t argb,
                  uint32_t zstencil, const uint32_t rect[4]);
    /* triangle list */
    void (*draw)(const Nv2aGpuState *st, const Nv2aGpuVertex *v, uint32_t count);
    /* the frame at va is being presented: make guest memory (and the screen)
     * show it */
    void (*present_sync)(uint32_t va, uint32_t pitch, uint32_t bpp,
                         uint32_t w, uint32_t h);
    void (*end_frame)(void);
    /* optional; 0 = not drawn, the executor transforms on the CPU instead */
    int  (*draw_vsh)(const Nv2aGpuState *st, const Nv2aGpuVsh *vs,
                     const float *in, uint32_t nverts,
                     const uint32_t *idx, uint32_t nidx);
    /* Visibility tests (D3D's Begin/EndVisibilityTest -> the NV2A's ZPASS
     * pixel count): begin counting the pixels that pass the depth test, and
     * end -- returning how many did, in the title's pixels. Optional. */
    void (*occlusion_begin)(void);
    uint32_t (*occlusion_end)(void);
    /* Every render target drawn since it was last copied back, copied into
     * guest memory now (waiting for the GPU). Called where the CPU can next
     * look: a semaphore release the title waits on. (Fatal Frame port.) */
    void (*sync_guest)(void);
    /* A 2D blit (NV062 surfaces + NV09F image blit) between guest addresses.
     * Returns 1 if the source is a render target and the copy was made on
     * the GPU (the destination becomes, or stays, a render target); 0 if the
     * caller must copy guest memory itself -- the back end has then dropped
     * any render target at the destination, so the copy is what is seen.
     * (Fatal Frame port.) */
    int (*blit)(uint32_t src_va, uint32_t src_pitch, uint32_t dst_va, uint32_t dst_pitch,
                uint32_t bpp, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                uint32_t w, uint32_t h);
} Nv2aGpuBackend;

void nv2a_gpu_backend_register(const Nv2aGpuBackend *backend);

/* Decode one face of a texture to 0xAARRGGBB, width*height entries. */
int nv2a_gpu_decode_texture(const Nv2aGpuTexture *tex, uint32_t face,
                            uint32_t *argb_out);

#endif /* NV2A_BACKEND_H */
