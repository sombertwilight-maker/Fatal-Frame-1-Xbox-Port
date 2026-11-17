/* A model of our own: see ff_tmdl.c for the file layout and why it exists. */
#ifndef FF_TMDL_H
#define FF_TMDL_H

#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    float    pos[3];
    float    nrm[3];
    float    uv[2];
    uint16_t bone_a, bone_b;   /* indices into the game's own skeleton */
    float    weight_a;         /* 1.0 for a vertex that rides one bone */
} FfVertex;                    /* 40 bytes, matching the file */
#pragma pack(pop)

typedef struct {
    FfVertex *verts;
    uint32_t *idx;
    uint32_t  n_vert, n_idx, texture;
} FfMesh;

typedef struct {
    uint8_t  *rgba;
    uint32_t  w, h;
} FfTexture;

typedef struct {
    FfMesh    *mesh;
    FfTexture *tex;
    uint32_t   n_mesh, n_tex, tot_vert, tot_idx;
} FfModel;

int  ff_tmdl_load(const char *path, FfModel *out);
int  ff_tmdl_load_named(const char *name, FfModel *out);
void ff_tmdl_free(FfModel *m);

#endif
