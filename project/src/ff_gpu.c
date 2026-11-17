/**
 * ff_gpu.c - keep the pushbuffer executor's semaphore pointed at D3D's fence
 *
 * Fatal Frame statically links the XDK 4721 Direct3D. Its device object is
 * reached through the global at 0x000F9B80, and in this XDK the device keeps
 *   +0x30  the write sequence (last fence submitted)
 *   +0x34  pointer to the GPU-written semaphore (progress the GPU has made)
 * (later XDKs, documented in d3d8ltcg-device-context.md, use +0x2C / +0x30).
 *
 * D3D's fence wait (sub_000EDC10) spins on *[device+0x34] until it reaches the
 * fence. On hardware the GPU writes that word with
 * BACK_END_WRITE_SEMAPHORE_RELEASE; the toolkit's executor does the same, once
 * it is told where the word is. D3D rewrites the field on device reset, so a
 * small host thread re-reads it rather than setting it once.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "ff_launcher.h"

#define FF_D3D_DEVICE_PTR_VA  0x000F9B80u
#define FF_DEV_SEMAPHORE_PTR  0x34u

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

static volatile uint32_t *guest32(uint32_t va)
{
    return (volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va);
}

extern uint32_t nv2a_pb_exec_flips(void);
extern void ff_anim_poll(void);          /* RECOMP_ANIMSCAN value scan */
extern int  ff_anim_status(char *, int);
extern void fb_set_status_cb(int (*)(char *, int));
extern void        ff_cheats_init(const char *dir);
extern void        ff_cheats_apply(void);
extern void        ff_cheats_list(char *, int);
extern int         ff_cheat_count(void);
extern const char *ff_cheat_name(int);
extern int         ff_cheat_on(int);
extern void        ff_cheat_toggle(int);
extern void fb_set_cheat_hooks(int (*)(void), const char *(*)(int),
                               int (*)(int), void (*)(int));
extern void xbox_KernelThreadDump(void);

/* No flip for a while (a hang -- or a movie, which plays on the overlay
 * without flipping): say where every guest thread is. Once after 4 s, then
 * every 20 s while it lasts. */
static void hang_watch(void)
{
    static uint32_t last_flips;
    static DWORD since, next;
    DWORD now = GetTickCount();
    uint32_t f = nv2a_pb_exec_flips();

    if (f != last_flips || f < 120) {      /* not while booting / loading */
        last_flips = f;
        since = now;
        next = 4000;
        return;
    }
    if (now - since >= next) {
        fprintf(stderr, "[HANG] no flip for %lu ms (flip %u)\n",
                (unsigned long)(now - since), f);
        xbox_KernelThreadDump();
        next += 20000;
    }
}

static DWORD WINAPI glue_thread(LPVOID arg)
{
    (void)arg;
    uint32_t last = 0;
    DWORD tick = 0;
    for (;;) {
        if (GetTickCount() - tick >= 250) {
            tick = GetTickCount();
            hang_watch();
        }
        ff_anim_poll();
        ff_cheats_apply();
        { extern void ff_model_frame_end(void); ff_model_frame_end(); }
        uint32_t dev = *guest32(FF_D3D_DEVICE_PTR_VA);
        if (dev && dev < 0x04000000u) {
            uint32_t sem = *guest32(dev + FF_DEV_SEMAPHORE_PTR);
            if (sem && sem != last) {
                nv2a_pb_set_semaphore_target(sem);
                fprintf(stderr, "[FF] D3D device 0x%08X, GPU semaphore at 0x%08X\n",
                        dev, sem);
                last = sem;
            }
        }
        Sleep(1);
    }
    return 0;
}

void ff_gpu_glue_start(void)
{
    const char *pb = getenv("RECOMP_PB_EXEC");
    if (!pb || !*pb || *pb == '0')
        return;
    fb_set_status_cb(ff_anim_status);
    {
        extern char g_ff_root[];
        extern int ff_tmdl_boot(void);
        ff_cheats_init(g_ff_root);
        ff_tmdl_boot();
        fb_set_cheat_hooks(ff_cheat_count, ff_cheat_name,
                           ff_cheat_on, ff_cheat_toggle);
    }
    HANDLE h = CreateThread(NULL, 0, glue_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

/* ---- host-GPU renderer glue ---------------------------------------------
 *
 * nv2a_d3d11.c (the toolkit's Direct3D 11 back end for the pushbuffer
 * executor) expects a few services from the framebuffer window that this
 * toolkit version's window does not have yet: graphics settings, and a
 * swap chain on the window. Until those exist the renderer presents by
 * copying each finished frame back into guest memory, where the existing
 * window shows it -- so these report "no window to present into". */
#include "video/gfx_config.h"

static XboxGfxConfig s_gfx;

static int env_int(const char *name, int dflt)
{
    const char *e = getenv(name);
    return e && *e ? atoi(e) : dflt;
}

XboxGfxConfig *xbox_GfxConfig(void)
{
    static int init;
    if (!init) {
        init = 1;
        s_gfx.scale = env_int("RECOMP_GPU_SCALE", 1);
        if (s_gfx.scale < 1) s_gfx.scale = 1;
        if (s_gfx.scale > 4) s_gfx.scale = 4;
        s_gfx.stretch = env_int("RECOMP_STRETCH", 1) != 0;
        s_gfx.show_fps = env_int("RECOMP_SHOW_FPS", 0) != 0;
        s_gfx.window_w = env_int("RECOMP_WINDOW_W", 0);
        s_gfx.window_h = env_int("RECOMP_WINDOW_H", 0);
        s_gfx.fullscreen = env_int("RECOMP_FULLSCREEN", 0) != 0;
        s_gfx.afterimage = env_int("RECOMP_AFTERIMAGE", 0) != 0;
        s_gfx.fx_bloom = env_int("RECOMP_FX_BLOOM", 40);
        s_gfx.fx_brightness = env_int("RECOMP_FX_BRIGHTNESS", 100);
        s_gfx.fx_gamma = env_int("RECOMP_FX_GAMMA", 100);
        s_gfx.fx_vignette = env_int("RECOMP_FX_VIGNETTE", 0);
        s_gfx.fx_grain = env_int("RECOMP_FX_GRAIN", 0);
        s_gfx.fx_sharpen = env_int("RECOMP_FX_SHARPEN", 95);
        s_gfx.fx_temp = env_int("RECOMP_FX_TEMP", 10);
        s_gfx.fx_scanlines = env_int("RECOMP_FX_SCANLINES", 0);
        s_gfx.fx_ca = env_int("RECOMP_FX_CA", 0);
        s_gfx.fx_contrast = env_int("RECOMP_FX_CONTRAST", 105);
        s_gfx.fx_saturation = env_int("RECOMP_FX_SATURATION", 125);
        s_gfx.costume = env_int("RECOMP_COSTUME", 0);
        s_gfx.light_prop = env_int("RECOMP_LIGHT_PROP", 1);
        s_gfx.tex_dump = env_int("RECOMP_TEX_DUMP", 0);
    }
    return &s_gfx;
}

/* The F6 menu's choices, written back to fatalframe.ini beside the exe:
 * known keys are replaced in place, missing ones appended, everything else
 * (comments included) kept as it is. */
void xbox_GfxConfigSave(void)
{
    extern char g_ff_root[];
    char path[MAX_PATH + 32], tmp[MAX_PATH + 32];
    char *text = NULL;
    size_t len = 0;
    FILE *f;
    static const char *keys[] = { "RECOMP_GPU_SCALE", "RECOMP_STRETCH",
                                  "RECOMP_SHOW_FPS", "RECOMP_WINDOW_W",
                                  "RECOMP_WINDOW_H", "RECOMP_FULLSCREEN",
                                  "RECOMP_AFTERIMAGE", "RECOMP_FX_BLOOM",
                                  "RECOMP_FX_CONTRAST", "RECOMP_FX_SATURATION",
                                  "RECOMP_COSTUME", "RECOMP_FX_BRIGHTNESS",
                                  "RECOMP_FX_GAMMA", "RECOMP_FX_VIGNETTE",
                                  "RECOMP_FX_GRAIN", "RECOMP_FX_SHARPEN",
                                  "RECOMP_FX_TEMP", "RECOMP_FX_SCANLINES",
                                  "RECOMP_FX_CA", "RECOMP_LIGHT_PROP",
                                  "RECOMP_CHEATS_ON" };
    char vals[21][96];
    int seen[21] = { 0 };
    int k;

    snprintf(vals[0], sizeof vals[0], "%d", s_gfx.scale);
    snprintf(vals[1], sizeof vals[1], "%d", s_gfx.stretch ? 1 : 0);
    snprintf(vals[2], sizeof vals[2], "%d", s_gfx.show_fps ? 1 : 0);
    snprintf(vals[3], sizeof vals[3], "%d", s_gfx.window_w);
    snprintf(vals[4], sizeof vals[4], "%d", s_gfx.window_h);
    snprintf(vals[5], sizeof vals[5], "%d", s_gfx.fullscreen ? 1 : 0);
    snprintf(vals[6], sizeof vals[6], "%d", s_gfx.afterimage ? 1 : 0);
    snprintf(vals[7], sizeof vals[7], "%d", s_gfx.fx_bloom);
    snprintf(vals[8], sizeof vals[8], "%d", s_gfx.fx_contrast);
    snprintf(vals[9], sizeof vals[9], "%d", s_gfx.fx_saturation);
    snprintf(vals[10], sizeof vals[10], "%d", s_gfx.costume);
    snprintf(vals[11], sizeof vals[11], "%d", s_gfx.fx_brightness);
    snprintf(vals[12], sizeof vals[12], "%d", s_gfx.fx_gamma);
    snprintf(vals[13], sizeof vals[13], "%d", s_gfx.fx_vignette);
    snprintf(vals[14], sizeof vals[14], "%d", s_gfx.fx_grain);
    snprintf(vals[15], sizeof vals[15], "%d", s_gfx.fx_sharpen);
    snprintf(vals[16], sizeof vals[16], "%d", s_gfx.fx_temp);
    snprintf(vals[17], sizeof vals[17], "%d", s_gfx.fx_scanlines);
    snprintf(vals[18], sizeof vals[18], "%d", s_gfx.fx_ca);
    snprintf(vals[19], sizeof vals[19], "%d", s_gfx.light_prop);
    ff_cheats_list(vals[20], (int)sizeof vals[20]);

    snprintf(path, sizeof path, "%s\\fatalframe.ini", g_ff_root);
    snprintf(tmp, sizeof tmp, "%s\\fatalframe.ini.new", g_ff_root);
    f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        len = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        text = (char *)malloc(len + 1);
        if (text) {
            len = fread(text, 1, len, f);
            text[len] = 0;
        }
        fclose(f);
    }
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "[GFX] settings not saved: cannot write %s\n", tmp);
        free(text);
        return;
    }
    if (text) {
        char *line = text;
        while (*line) {
            char *nl = strchr(line, '\n');
            size_t ll = nl ? (size_t)(nl - line) + 1 : strlen(line);
            int replaced = 0;
            for (k = 0; k < (int)(sizeof keys / sizeof keys[0]); k++) {
                size_t kl = strlen(keys[k]);
                if (!strncmp(line, keys[k], kl) && line[kl] == '=') {
                    fprintf(f, "%s=%s\n", keys[k], vals[k]);
                    seen[k] = 1;
                    replaced = 1;
                    break;
                }
            }
            if (!replaced)
                fwrite(line, 1, ll, f);
            line += ll;
        }
        if (len && text[len - 1] != '\n')
            fputc('\n', f);
    }
    for (k = 0; k < (int)(sizeof keys / sizeof keys[0]); k++)
        if (!seen[k])
            fprintf(f, "%s=%s\n", keys[k], vals[k]);
    fclose(f);
    free(text);
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "[GFX] settings not saved: replace failed (%lu) %s\n",
                GetLastError(), path);
        DeleteFileA(tmp);
    } else {
        fprintf(stderr, "[GFX] settings saved to fatalframe.ini\n");
    }
}
