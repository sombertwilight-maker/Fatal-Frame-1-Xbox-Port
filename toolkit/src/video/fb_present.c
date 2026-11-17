/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 * RECOMP_D3D_PRESENT=1 shows the same frames through a D3D11 swap chain of
 * the window's own instead (gpu_present.c): scaled on the GPU, vsync-paced.
 *
 * The window opens at RECOMP_WINDOW_SCALE times the frame (default 2, made
 * smaller until it fits the desktop) and can be resized; the frame is
 * letterboxed to its 4:3 shape. F11 or Alt+Enter toggles borderless
 * fullscreen, RECOMP_FULLSCREEN=1 starts in it. Closing the window ends the
 * process, as the SDL window does.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Which keys are down, for the pad stand-in in src/input: this window
 * records them in the shared key table (src/input/keyboard.c).
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them. */
#include "keyboard.h"   /* src/input */
#include "xinput_xbox.h" /* src/input -- pad for the menu */
#include "gpu_present.h"
#include "recomp_exit_hook.h"   /* src/platform */
#include <stdbool.h>
#include "pvideo_scanout.h"
#include "../nv2a/nv2a_mmio_hook.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * The lock also protects the window's copy: two flips can otherwise reuse
 * the buffer the window is still reading. */
static uint32_t     *s_present[2];
static LONG          s_present_idx = -1;   /* -1 until the first flip */
static SRWLOCK       s_present_lock = SRWLOCK_INIT;

/* The last PVIDEO frame shown. A frame stays on screen until the next one or
 * STOP, but once its bank is consumed the title may reuse the memory, so the
 * pixels are kept here rather than read again. */
static uint8_t      *s_pvideo_source;
static size_t        s_pvideo_bytes;
static unsigned      s_pvideo_bank;
static uint32_t      s_pvideo_regs[0x1000 / 4];

/* Draw the video overlay over s_rgb: a newly submitted bank if there is one
 * (copied, drawn, then handed back to the title), else the last frame. */
static void fb_overlay(void)
{
    uint32_t regs[0x1000 / 4];
    unsigned bank;
    bool presented = false;

    if (!s_rgb || !nv2a_hook_pvideo_snapshot(regs)) return;
    if ((regs[NV_PVIDEO_STOP / 4] & 1u) ||
        (s_pvideo_source &&
         regs[(NV_PVIDEO_SIZE_IN + s_pvideo_bank * 4) / 4] == UINT32_MAX)) {
        free(s_pvideo_source);
        s_pvideo_source = NULL;
        if (regs[NV_PVIDEO_STOP / 4] & 1u) return;
    }
    for (bank = 0; bank < 2; ++bank) {
        uint64_t physical = (uint64_t)regs[(NV_PVIDEO_BASE + bank * 4) / 4] +
                            regs[(NV_PVIDEO_OFFSET + bank * 4) / 4];
        unsigned pitch = regs[(NV_PVIDEO_FORMAT + bank * 4) / 4] & NV_PVIDEO_FORMAT_PITCH;
        unsigned height = (regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] >> 16) & 0x7ff;
        uint64_t bytes = (uint64_t)pitch * height;
        uintptr_t va;
        MEMORY_BASIC_INFORMATION mapping;
        uint8_t *snapshot;

        if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u)))) continue;
        if (!bytes || physical + bytes > 0x04000000u) continue;
        va = (uintptr_t)(0x80000000u + (uint32_t)physical) +
             (uintptr_t)xbox_GetMemoryOffset();
        if (!VirtualQuery((const void *)va, &mapping, sizeof(mapping)) ||
            mapping.State != MEM_COMMIT ||
            (mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            bytes > mapping.RegionSize - (size_t)(va - (uintptr_t)mapping.BaseAddress))
            continue;
        snapshot = (uint8_t *)malloc((size_t)bytes);
        if (!snapshot) continue;
        memcpy(snapshot, (const void *)va, (size_t)bytes);
        if (!pvideo_scanout(regs, snapshot, (size_t)bytes, s_rgb,
                            s_fb_width, s_fb_height, bank)) {
            free(snapshot);
            continue;
        }
        free(s_pvideo_source);
        s_pvideo_source = snapshot;
        s_pvideo_bytes = (size_t)bytes;
        s_pvideo_bank = bank;
        memcpy(s_pvideo_regs, regs, sizeof(s_pvideo_regs));
        presented = true;
        nv2a_hook_pvideo_consume(bank);
    }
    if (!presented && s_pvideo_source)
        pvideo_scanout(s_pvideo_regs, s_pvideo_source, s_pvideo_bytes, s_rgb,
                       s_fb_width, s_fb_height, s_pvideo_bank);
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Vertical anti-aliasing factor of the surface being shown: 2 for 2x2, where
 * the surface has twice the rows as well as twice the columns, and 1 for
 * everything else (2x1 included). Only the pushbuffer executor knows it. */
static uint32_t s_fb_aa_sy = 1;

void xbox_FramebufferWindowSetAA(uint32_t sy)
{
    s_fb_aa_sy = sy ? sy : 1;
}

static volatile LONG s_flips;
/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    AcquireSRWLockExclusive(&s_present_lock);
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next]) {
            ReleaseSRWLockExclusive(&s_present_lock);
            fprintf(stderr, "  [FBWIN] cannot allocate a presentation frame\n");
            return;
        }
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* 2x-wide anti-aliased 32-bit surface: every other pixel, as in
             * fb_convert. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    s_present_idx = next;
    {
        /* RECOMP_FRAME_DUMP=<n>: the first n frames the window is handed,
         * raw BGRA with a 16-byte header each, into framedump.raw beside the
         * exe -- a one-frame glitch can only be found by looking at every
         * frame. ~1.2 MB a frame at 640x480. */
        static int want = -1;
        static FILE *fd;
        static uint32_t wrote;
        if (want < 0) {
            const char *e = getenv("RECOMP_FRAME_DUMP");
            want = e ? atoi(e) : 0;
            if (want > 0)
                fd = fopen("framedump.raw", "wb");
        }
        if (fd && (int)wrote < want) {
            uint32_t hdr[4] = { 0x46445246u, (uint32_t)s_flips, fb_va,
                                (s_fb_width << 16) | s_fb_height };
            fwrite(hdr, 1, 16, fd);
            fwrite(s_present[next], 4, (size_t)s_fb_width * s_fb_height, fd);
            if ((int)++wrote == want) { fclose(fd); fd = NULL; fprintf(stderr, "[FBDUMP] %u frames written\n", wrote); }
        }
    }
    {
        /* RECOMP_WINFRAME_LOG (on unless 0): the frame the window is handed,
         * measured -- a frame much darker or brighter than the one before
         * it is a flicker, and the line says which buffer it came from. */
        static int on = -1;
        static uint32_t prev, prev_va, logged;
        if (on < 0) {
            const char *e = getenv("RECOMP_WINFRAME_LOG");
            on = !(e && *e == '0');
        }
        if (on) {
            const uint32_t *p = s_present[next];
            uint64_t sum = 0;
            uint32_t n = 0, i, mean;
            for (i = 0; i < s_fb_width * s_fb_height; i += 61) {
                uint32_t v = p[i];
                sum += ((v >> 16) & 0xFF) + ((v >> 8) & 0xFF) + (v & 0xFF);
                n += 3;
            }
            mean = n ? (uint32_t)(sum / n) : 0;
            if (prev && logged < 300
                && ((mean * 2 < prev && prev > 8) || (mean > prev * 2 && mean > prev + 12))) {
                logged++;
                fprintf(stderr, "[WINFRAME] flip %ld: brightness %u -> %u (buffer %08X, before %08X, pitch %u)\n",
                        (long)s_flips, prev, mean, fb_va, prev_va, pitch);
            }
            prev = mean ? mean : 1;
            prev_va = fb_va;
        }
    }
    ReleaseSRWLockExclusive(&s_present_lock);
}

static int fb_copy_presented_frame(uint32_t *dst)
{
    int copied = 0;

    AcquireSRWLockShared(&s_present_lock);
    if (s_present_idx >= 0) {
        memcpy(dst, s_present[s_present_idx],
               (size_t)s_fb_width * s_fb_height * 4);
        copied = 1;
    }
    ReleaseSRWLockShared(&s_present_lock);
    return copied;
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

/* Alt+Enter / F11: borderless fullscreen on the window's monitor, and back.
 * (DXGI's own Alt+Enter, exclusive mode at the frame's 640x480, is off --
 * gpu_present.c; both presenters letterbox to any size.) */

/* ---- graphics settings menu (F6) and GPU-present services -----------------
 *
 * The services gfx_config.h promises the D3D11 back end: the window handle
 * (which turns on the swap-chain present), the GPU-present flag (which turns
 * off the GDI blit below while the back end is presenting), the video
 * overlay plane (FMVs decode into guest memory, so the back end steps aside
 * while one plays), and the overlay image (this menu and the FPS counter,
 * composited by the back end over the frame).
 *
 * The menu itself: F6 opens it, arrows change settings, F6 or Esc closes.
 * Every change applies at once and is saved through xbox_GfxConfigSave().
 * Text is drawn with GDI into a DIB section; the image handed to the back
 * end carries straight (unpremultiplied) alpha. */
#include "gfx_config.h"

static volatile LONG s_gpu_present_on;
static volatile DWORD s_gpu_present_tick;
static HWND     s_hwnd_pub;
static int      s_fullscreen;

static SRWLOCK  s_ovl_img_lock = SRWLOCK_INIT;
static uint32_t *s_ovl_img;
static uint32_t s_ovl_img_w, s_ovl_img_h;
static int      s_ovl_img_seq;

static int      s_menu_open;
static int      s_menu_sel;
static float    s_fps_shown;

void *xbox_FramebufferWindowHwnd(void) { return (void *)s_hwnd_pub; }

void xbox_FramebufferWindowSetGpuPresent(int on)
{
    InterlockedExchange(&s_gpu_present_on, on);
    if (on)
        s_gpu_present_tick = GetTickCount();
}

int xbox_FramebufferWindowOverlayPlane(void)
{
    /* An FMV decodes into guest memory and is composited by fb_overlay:
     * while one plays the GDI path must draw the frames. */
    return s_pvideo_source != NULL;
}

const uint32_t *xbox_FramebufferWindowOverlayImage(uint32_t *w, uint32_t *h, int *seq)
{
    if (w) *w = s_ovl_img_w;
    if (h) *h = s_ovl_img_h;
    if (seq) *seq = s_ovl_img_seq;
    return s_ovl_img;
}

void xbox_FramebufferWindowOverlayLock(int lock)
{
    if (lock)
        AcquireSRWLockExclusive(&s_ovl_img_lock);
    else
        ReleaseSRWLockExclusive(&s_ovl_img_lock);
}

/* Window-size presets. The frame is 4:3 and "stretch to window" fills
 * whatever shape the window is, so these are only a starting size. */
static const struct { int w, h; } s_win_presets[] = {
    {  640,  480 },
    {  960,  720 },
    { 1280,  960 },
    { 1600, 1200 },
    { 1920, 1440 },
};
#define N_WIN_PRESETS (int)(sizeof s_win_presets / sizeof s_win_presets[0])
static int s_win_preset = 1;

static void menu_window_size(int preset)
{
    RECT r;
    XboxGfxConfig *g = xbox_GfxConfig();
    int cw = s_win_presets[preset].w;
    int ch = s_win_presets[preset].h;
    g->window_w = cw;
    g->window_h = ch;
    if (s_fullscreen || !s_hwnd_pub)
        return;
    r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
    AdjustWindowRect(&r, (DWORD)GetWindowLongPtrA(s_hwnd_pub, GWL_STYLE), FALSE);
    SetWindowPos(s_hwnd_pub, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

/* Draw `text` rows into a fresh BGRA image (straight alpha) and publish it
 * to the back end. The rows draw with the stock system font into a DIB. */
/* An optional extra line for the corner readout: the game sets this, and
 * whatever it writes is shown under the FPS. Used by the value scanner so a
 * key press is visibly doing something. */
static int (*s_status_cb)(char *, int);
void fb_set_status_cb(int (*cb)(char *, int)) { s_status_cb = cb; }

/* The CHEATS tab is driven by the game, which owns the cheat file; the
 * toolkit only draws the rows and forwards a toggle. */
typedef struct {
    int (*count)(void);
    const char *(*name)(int);
    int (*is_on)(int);
    void (*toggle)(int);
} FbCheatHooks;
static FbCheatHooks s_ch;
void fb_set_cheat_hooks(int (*count)(void), const char *(*name)(int),
                        int (*is_on)(int), void (*toggle)(int))
{
    s_ch.count = count; s_ch.name = name;
    s_ch.is_on = is_on; s_ch.toggle = toggle;
}
static int cheat_rows(void) { return s_ch.count ? s_ch.count() : 0; }

static void overlay_publish(uint32_t *px, uint32_t w, uint32_t h)
{
    xbox_FramebufferWindowOverlayLock(1);
    free(s_ovl_img);
    s_ovl_img = px;
    s_ovl_img_w = px ? w : 0;
    s_ovl_img_h = px ? h : 0;
    s_ovl_img_seq++;
    xbox_FramebufferWindowOverlayLock(0);
}

static int s_menu_tab;                /* 0 display, 1 effects, 2 costumes */

/* Order and names are the player's. Each row here lines up with the row at
 * the same index in cost_base[] in kernel_bridge.c -- the label and the
 * files it maps to are two halves of one table, so they move together or
 * not at all. The old names had the two body meshes the wrong way round:
 * m000_miku4 is the yukata, m000_miku2 is the outfit set. */
static const char *s_costumes[] = {
    "Default",
    "Jp Miku",
    "Gyaru Miku",
    "Goth Miku",
    "Adventure Miku",
    "Purple Miku",
    "Yukata Red",
    "Yukata Blue",
    "Yukata White",
    "Jp Yukata Red",
    "Jp Yukata Blue",
    "Jp Yukata White",
};
#define N_COSTUMES ((int)(sizeof s_costumes / sizeof s_costumes[0]))

static void game_restart(void);   /* defined below, used by menu_change */

static int menu_items_on(int tab)
{
    if (tab == 3) {
        int n = cheat_rows();
        return n ? n : 1;          /* one row to say the file is missing */
    }
    /* costumes: the light prop, every outfit, then Restart on the end --
     * the costume only takes hold when the model is next loaded, so the
     * thing that applies it belongs on the same page. */
    return tab == 0 ? 6 : tab == 1 ? 11 : N_COSTUMES + 2;
}

static void menu_line(char *out, size_t cap, int item)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    if (s_menu_tab == 3) {
        if (!cheat_rows())
            snprintf(out, cap, "no cheats yet - add them to fatalframe_cheats.ini");
        else
            snprintf(out, cap, "%-28.28s %s", s_ch.name(item),
                     s_ch.is_on(item) ? "On" : "Off");
        return;
    }
    if (s_menu_tab == 1) {
        switch (item) {
        case 0: snprintf(out, cap, "Bloom                  %d%%", g->fx_bloom); break;
        case 1: snprintf(out, cap, "Contrast               %d%%", g->fx_contrast); break;
        case 2: snprintf(out, cap, "Saturation             %d%%", g->fx_saturation); break;
        case 3: snprintf(out, cap, "Brightness             %d%%", g->fx_brightness); break;
        case 4: snprintf(out, cap, "Gamma                  %d%%", g->fx_gamma); break;
        case 5: snprintf(out, cap, "Vignette               %d%%", g->fx_vignette); break;
        case 6: snprintf(out, cap, "Film grain             %d%%", g->fx_grain); break;
        case 7: snprintf(out, cap, "Sharpen                %d%%", g->fx_sharpen); break;
        case 8: snprintf(out, cap, "Color temperature      %+d", g->fx_temp); break;
        case 9: snprintf(out, cap, "Scanlines              %d%%", g->fx_scanlines); break;
        case 10: snprintf(out, cap, "Chromatic aberration   %d%%", g->fx_ca); break;
        }
        return;
    }
    if (s_menu_tab == 2) {
        if (item == 0)
            snprintf(out, cap, "Light prop             %s",
                     g->light_prop ? "Lantern" : "Flashlight");
        else if (item == N_COSTUMES + 1)
            snprintf(out, cap, "Restart game   (unsaved progress is lost)");
        else {
            /* The model is read when the game loads it, not when this is
             * chosen, so picking a costume mid-session changes nothing on
             * screen until the next launch. Saying so here: without it the
             * menu claims one costume while another is plainly standing in
             * front of you, which reads as the costume being broken.
             *
             * The boot value is captured the first time this menu is drawn
             * -- the earliest moment a change could be made, so it is still
             * whatever the game actually loaded with. */
            static int boot = -1;
            if (boot < 0)
                boot = g->costume;
            snprintf(out, cap, "%s %s%s", g->costume == item - 1 ? "*" : " ",
                     item - 1 < N_COSTUMES ? s_costumes[item - 1] : "",
                     (g->costume == item - 1 && g->costume != boot)
                         ? "   (restart to apply)" : "");
        }
        return;
    }
    switch (item) {
    case 0:
        snprintf(out, cap, "Internal resolution    %dx  (%dx%d)",
                 g->scale, 640 * g->scale, 480 * g->scale);
        break;
    case 1:
        snprintf(out, cap, "Widescreen             %s", g->stretch ? "On" : "Off");
        break;
    case 2:
        snprintf(out, cap, "Window size            %dx%d",
                 s_win_presets[s_win_preset].w, s_win_presets[s_win_preset].h);
        break;
    case 3:
        snprintf(out, cap, "Fullscreen             %s", s_fullscreen ? "On" : "Off");
        break;
    case 4:
        snprintf(out, cap, "FPS counter            %s", g->show_fps ? "On" : "Off");
        break;
    case 5:
        snprintf(out, cap, "Afterimage filter      %s", g->afterimage ? "On" : "Off");
        break;
    }
}
#define MENU_ITEMS 6

static void menu_render(void)
{
    enum { PW = 560, ROWH = 26, PAD = 14, MX = 24, MY = 24 };
    int nitems = menu_items_on(s_menu_tab);
    int ph = PAD * 2 + ROWH * (nitems + 4);   /* +1 for the pad legend */
    uint32_t iw = PW + MX, ih = (uint32_t)(ph + MY);
    BITMAPINFO bi;
    void *bits = NULL;
    HDC mdc;
    HBITMAP bmp;
    HFONT font, of;
    uint32_t *out;
    int i;

    char st[64];
    int have_st = s_status_cb ? s_status_cb(st, (int)sizeof st) : 0;

    if (!s_menu_open && !xbox_GfxConfig()->show_fps && !have_st) {
        overlay_publish(NULL, 0, 0);
        return;
    }
    if (!s_menu_open) {
        /* The corner readout: FPS, and the scanner's line under it. */
        char t[32];
        int show_fps = xbox_GfxConfig()->show_fps;
        iw = 210; ih = have_st ? (show_fps ? 50 : 30) : 30;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = (LONG)iw;
        bi.bmiHeader.biHeight = -(LONG)ih;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        mdc = CreateCompatibleDC(NULL);
        bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!bmp) { DeleteDC(mdc); return; }
        SelectObject(mdc, bmp);
        memset(bits, 0, (size_t)iw * ih * 4);
        SetBkMode(mdc, TRANSPARENT);
        font = CreateFontA(18, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, "Segoe UI");
        of = (HFONT)SelectObject(mdc, font);
        if (show_fps) {
            snprintf(t, sizeof t, "FPS %.1f", s_fps_shown);
            SetTextColor(mdc, RGB(80, 255, 120));
            TextOutA(mdc, 8, 5, t, (int)strlen(t));
        }
        if (have_st) {
            SetTextColor(mdc, RGB(255, 190, 70));
            TextOutA(mdc, 8, show_fps ? 25 : 5, st, (int)strlen(st));
        }
        GdiFlush();
        out = (uint32_t *)malloc((size_t)iw * ih * 4);
        if (out) {
            const uint32_t *src = (const uint32_t *)bits;
            for (i = 0; i < (int)(iw * ih); i++)
                out[i] = src[i] ? (src[i] | 0xE0000000u) : 0x50000000u;
            overlay_publish(out, iw, ih);
        }
        SelectObject(mdc, of); DeleteObject(font); DeleteObject(bmp); DeleteDC(mdc);
        return;
    }

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)iw;
    bi.bmiHeader.biHeight = -(LONG)ih;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    mdc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { DeleteDC(mdc); return; }
    SelectObject(mdc, bmp);
    memset(bits, 0, (size_t)iw * ih * 4);
    {
        RECT pr = { MX, MY, MX + PW, MY + ph };
        HBRUSH panel = CreateSolidBrush(RGB(16, 12, 20));
        FillRect(mdc, &pr, panel);
        DeleteObject(panel);
        {
            HBRUSH edge = CreateSolidBrush(RGB(120, 40, 40));
            RECT top = { pr.left, pr.top, pr.right, pr.top + 2 };
            RECT bot = { pr.left, pr.bottom - 2, pr.right, pr.bottom };
            FillRect(mdc, &top, edge);
            FillRect(mdc, &bot, edge);
            DeleteObject(edge);
        }
    }
    SetBkMode(mdc, TRANSPARENT);
    font = CreateFontA(19, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    of = (HFONT)SelectObject(mdc, font);
    {
        static const char *tabs[] = { "DISPLAY", "EFFECTS", "COSTUMES",
                                      "CHEATS" };
        int tx = MX + PAD, t;
        for (t = 0; t < 4; t++) {
            char tt[24];
            snprintf(tt, sizeof tt, t == s_menu_tab ? "[%s]" : " %s ", tabs[t]);
            SetTextColor(mdc, t == s_menu_tab ? RGB(255, 235, 235) : RGB(140, 125, 140));
            TextOutA(mdc, tx, MY + PAD, tt, (int)strlen(tt));
            tx += (int)(strlen(tt) + 2) * 11;
        }
    }
    SetTextColor(mdc, RGB(200, 190, 200));
    for (i = 0; i < nitems; i++) {
        char line[96], row[112];
        int y = MY + PAD + ROWH + 6 + i * ROWH;
        menu_line(line, sizeof line, i);
        snprintf(row, sizeof row, "%s %s", i == s_menu_sel ? ">" : " ", line);
        if (i == s_menu_sel) {
            RECT hr = { MX + 6, y - 3, MX + PW - 6, y + ROWH - 7 };
            HBRUSH hb = CreateSolidBrush(RGB(70, 22, 26));
            FillRect(mdc, &hr, hb);
            DeleteObject(hb);
            SetTextColor(mdc, RGB(255, 235, 235));
        } else {
            SetTextColor(mdc, RGB(200, 190, 200));
        }
        TextOutA(mdc, MX + PAD, y, row, (int)strlen(row));
    }
    SetTextColor(mdc, RGB(140, 125, 140));
    {
        int hy = MY + PAD + ROWH + 10 + nitems * ROWH;
        const char *hint = s_menu_tab == 3
            ? "Keys  Tab page   arrows/Enter toggle   F6/Esc close"
            : "Keys  Tab page   arrows change   F6/Esc close";
        /* The pad legend, so the bindings are discoverable rather than
         * something a player has to be told. */
        const char *pad =
            "Pad   LB/RB page   D-pad move   A change   B/Back close";
        TextOutA(mdc, MX + PAD, hy, hint, (int)strlen(hint));
        SetTextColor(mdc, RGB(120, 150, 170));
        TextOutA(mdc, MX + PAD, hy + ROWH - 4, pad, (int)strlen(pad));
    }
    GdiFlush();

    out = (uint32_t *)malloc((size_t)iw * ih * 4);
    if (out) {
        const uint32_t *src = (const uint32_t *)bits;
        uint32_t x, y;
        for (y = 0; y < ih; y++)
            for (x = 0; x < iw; x++) {
                uint32_t c = src[y * iw + x];
                int inpanel = (int)x >= MX && (int)y >= MY;
                out[y * iw + x] = c ? (c | 0xFF000000u)
                                    : (inpanel ? 0xD8000000u : 0u);
            }
        /* panel body keeps its own colour at high alpha */
        for (y = (uint32_t)MY; y < ih; y++)
            for (x = (uint32_t)MX; x < iw; x++) {
                uint32_t c = out[y * iw + x];
                if ((c & 0x00FFFFFFu) == 0x00100C14u)   /* panel brush */
                    out[y * iw + x] = (c & 0x00FFFFFFu) | 0xD8000000u;
            }
        overlay_publish(out, iw, ih);
    }
    SelectObject(mdc, of); DeleteObject(font); DeleteObject(bmp); DeleteDC(mdc);
}

static void fb_toggle_fullscreen(HWND h);

static void menu_change(int item, int dir)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    if (s_menu_tab == 3) {
        if (cheat_rows() && s_ch.toggle)
            s_ch.toggle(item);
        xbox_GfxConfigSave();      /* the enabled set rides in the ini too */
        menu_render();
        return;
    }
    if (s_menu_tab == 1) {
        static volatile int *const fld[11] = { 0 };  /* placeholder, set below */
        struct { volatile int *v; int lo, hi, step; } e;
        (void)fld;
        switch (item) {
        case 0:  e.v = &g->fx_bloom;      e.lo = 0;   e.hi = 100; e.step = 5; break;
        case 1:  e.v = &g->fx_contrast;   e.lo = 50;  e.hi = 200; e.step = 5; break;
        case 2:  e.v = &g->fx_saturation; e.lo = 0;   e.hi = 200; e.step = 5; break;
        case 3:  e.v = &g->fx_brightness; e.lo = 50;  e.hi = 150; e.step = 5; break;
        case 4:  e.v = &g->fx_gamma;      e.lo = 50;  e.hi = 150; e.step = 5; break;
        case 5:  e.v = &g->fx_vignette;   e.lo = 0;   e.hi = 100; e.step = 5; break;
        case 6:  e.v = &g->fx_grain;      e.lo = 0;   e.hi = 100; e.step = 5; break;
        case 7:  e.v = &g->fx_sharpen;    e.lo = 0;   e.hi = 100; e.step = 5; break;
        case 8:  e.v = &g->fx_temp;       e.lo = -100; e.hi = 100; e.step = 10; break;
        case 9:  e.v = &g->fx_scanlines;  e.lo = 0;   e.hi = 100; e.step = 5; break;
        case 10: e.v = &g->fx_ca;         e.lo = 0;   e.hi = 100; e.step = 5; break;
        default: return;
        }
        *e.v += dir * e.step;
        if (*e.v < e.lo) *e.v = e.lo;
        if (*e.v > e.hi) *e.v = e.hi;
        xbox_GfxConfigSave();
        menu_render();
        return;
    }
    if (s_menu_tab == 2) {
        if (item == N_COSTUMES + 1) {
            xbox_GfxConfigSave();       /* before we go */
            game_restart();
            return;
        }
        if (item == 0)
            g->light_prop = !g->light_prop;
        else
            g->costume = item - 1;
        xbox_GfxConfigSave();
        menu_render();
        return;
    }
    switch (item) {
    case 0:
        g->scale += dir;
        if (g->scale < 1) g->scale = 1;
        if (g->scale > 4) g->scale = 4;
        break;
    case 1:
        g->stretch = !g->stretch;
        break;
    case 2:
        s_win_preset += dir;
        if (s_win_preset < 0) s_win_preset = 0;
        if (s_win_preset >= N_WIN_PRESETS) s_win_preset = N_WIN_PRESETS - 1;
        menu_window_size(s_win_preset);
        break;
    case 3:
        if (s_hwnd_pub)
            fb_toggle_fullscreen(s_hwnd_pub);
        break;
    case 4:
        g->show_fps = !g->show_fps;
        break;
    case 5:
        g->afterimage = !g->afterimage;
        break;
    }
    g->fullscreen = s_fullscreen;
    xbox_GfxConfigSave();
    menu_render();
}

static void menu_key(int vk)
{
    switch (vk) {
    case VK_TAB:   s_menu_tab = (s_menu_tab + 1) % 4; s_menu_sel = 0; menu_render(); break;
    case VK_UP:    s_menu_sel = (s_menu_sel + menu_items_on(s_menu_tab) - 1) % menu_items_on(s_menu_tab); menu_render(); break;
    case VK_DOWN:  s_menu_sel = (s_menu_sel + 1) % menu_items_on(s_menu_tab); menu_render(); break;
    case VK_LEFT:  menu_change(s_menu_sel, -1); break;
    case VK_RIGHT:
    case VK_RETURN: menu_change(s_menu_sel, 1); break;
    case VK_ESCAPE:
    case VK_F6:    s_menu_open = 0; menu_render(); break;
    }
}

/* Relaunch the game.
 *
 * Deliberately with no arguments. A disc image on the command line always
 * wins in ff_launcher.c -- that is how a player re-extracts after replacing
 * a bad image -- so carrying the original command line through made every
 * restart try to unpack 1.4 GB again, over a game folder that was already
 * good and while this process still held it open. Bare, the launcher finds
 * the extracted game folder, sees it is valid, and goes straight in.
 *
 * The costume is saved before this runs, so the new process reads it at
 * startup and comes up wearing it.
 */
static void game_restart(void)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 4];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return;
    _snwprintf(cmd, MAX_PATH + 3, L"\"%ls\"", exe);
    cmd[MAX_PATH + 3] = L'\0';

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
}

static void menu_toggle(void)
{
    s_menu_open = !s_menu_open;
    xbox_InputKeysClear();
    xbox_InputMenuCapture(s_menu_open);
    menu_render();
}

/* Driving the menu with the controller.
 *
 * Polled once a frame rather than event-driven, because a pad has no key
 * repeat of its own: holding a direction has to be turned into a stream of
 * steps here. Edges are tracked per button so a press counts once.
 *
 * Back opens and closes it, which the game does not use; A changes a value,
 * B closes, the shoulders page between tabs. The stick is read alongside the
 * d-pad so either works.
 */
static void menu_pad_poll(void)
{
    enum { DEAD = 14000, REP_FIRST = 360, REP_NEXT = 90,
           BTN_A = 0, BTN_B = 1, BTN_BLACK = 4, BTN_WHITE = 5 };
    XBOX_INPUT_STATE st;
    static WORD  prev_b;
    static BYTE  prev_a[8];
    static int   vdir, hdir;
    static DWORD vat, hat;
    DWORD now = GetTickCount();
    int nit, d;
    WORD b;

    if (xbox_InputGetStateRaw(0, &st) != ERROR_SUCCESS) {
        prev_b = 0;
        memset(prev_a, 0, sizeof prev_a);
        vdir = hdir = 0;
        return;
    }
    b = st.Gamepad.wButtons;

#define EDGE_BTN(bit)  (((b) & (bit)) && !(prev_b & (bit)))
#define EDGE_AN(ix)    (st.Gamepad.bAnalogButtons[ix] >= XBOX_ANALOG_BUTTON_THRESHOLD \
                        && prev_a[ix] < XBOX_ANALOG_BUTTON_THRESHOLD)

    if (EDGE_BTN(XBOX_GAMEPAD_BACK))
        menu_toggle();

    if (!s_menu_open) {
        vdir = hdir = 0;
        goto done;
    }
    nit = menu_items_on(s_menu_tab);

    d = 0;
    if ((b & XBOX_GAMEPAD_DPAD_UP)   || st.Gamepad.sThumbLY >  DEAD) d = -1;
    if ((b & XBOX_GAMEPAD_DPAD_DOWN) || st.Gamepad.sThumbLY < -DEAD) d =  1;
    if (!d) {
        vdir = 0;
    } else if (d != vdir) {
        vdir = d; vat = now + REP_FIRST;
        s_menu_sel = (s_menu_sel + (d > 0 ? 1 : nit - 1)) % nit;
        menu_render();
    } else if ((long)(now - vat) >= 0) {
        vat = now + REP_NEXT;
        s_menu_sel = (s_menu_sel + (d > 0 ? 1 : nit - 1)) % nit;
        menu_render();
    }

    d = 0;
    if ((b & XBOX_GAMEPAD_DPAD_LEFT)  || st.Gamepad.sThumbLX < -DEAD) d = -1;
    if ((b & XBOX_GAMEPAD_DPAD_RIGHT) || st.Gamepad.sThumbLX >  DEAD) d =  1;
    if (!d) {
        hdir = 0;
    } else if (d != hdir) {
        hdir = d; hat = now + REP_FIRST;
        menu_change(s_menu_sel, d);
    } else if ((long)(now - hat) >= 0) {
        hat = now + REP_NEXT;
        menu_change(s_menu_sel, d);
    }

    if (EDGE_AN(BTN_A))
        menu_change(s_menu_sel, 1);
    if (EDGE_AN(BTN_B))
        menu_toggle();
    if (EDGE_AN(BTN_WHITE)) {
        s_menu_tab = (s_menu_tab + 3) % 4; s_menu_sel = 0; menu_render();
    }
    if (EDGE_AN(BTN_BLACK)) {
        s_menu_tab = (s_menu_tab + 1) % 4; s_menu_sel = 0; menu_render();
    }

done:
#undef EDGE_BTN
#undef EDGE_AN
    prev_b = b;
    memcpy(prev_a, st.Gamepad.bAnalogButtons, sizeof prev_a);
}

static void fb_toggle_fullscreen(HWND h)
{
#define full s_fullscreen
    static RECT saved;
    static LONG_PTR saved_style;
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        full = 0;
    }
#undef full
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* First press only (bit 30: the key was already down), and with Alt held
     * (bit 29) for Enter; neither reaches the key table. */
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        fb_toggle_fullscreen(h);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;                       /* auto-repeat of Alt+Enter */
    if (m == WM_KEYDOWN && w == VK_F6 && !(l & (1 << 30))) {
        menu_toggle();
        return 0;
    }
    if (s_menu_open && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN)) {
        menu_key((int)w);               /* the menu eats the keyboard */
        return 0;
    }
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        /* Closing the window ends the run. Only stopping this thread left
         * every other one running -- audio played on, and the process had
         * to be ended from Task Manager. The guest has no exit path of its
         * own to ask. TerminateProcess rather than ExitProcess: the guest
         * threads are still running, and ExitProcess's cleanup can wait on
         * a lock one of them held. The exit hook does the reporting that
         * would otherwise be skipped. RECOMP_CLOSE_KEEPS_RUNNING=1 only
         * closes the window, as before. */
        InterlockedExchange(&s_fb_running, 0);
        if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
            fprintf(stderr, "  [FBWIN] window closed: exiting\n");
            recomp_exit_hook_run("window");
            fflush(stdout);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1);
        /* A Shift, Ctrl or Alt is also its side's key, for bindings to
         * "lshift" and the like; wParam stays the either-side one. */
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 1);
        }
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 0);
            /* With both Shifts down Windows sends one key-up, for the
             * last released, so the other side would stay held. Autorepeat
             * sets a Shift still down again. */
            if (w == VK_SHIFT) {
                xbox_InputKeySet(VK_LSHIFT, 0);
                xbox_InputKeySet(VK_RSHIFT, 0);
            }
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Mouse buttons go in the same table, for bindings that name them.
     * Capturing the mouse while one is down delivers its release even
     * outside the window. */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        {
            int down = 0;
            int vk = xbox_InputWin32MouseVk(m, (uint32_t)w, &down);
            if (vk)
                xbox_InputKeySet(vk, down);
            if (down)
                SetCapture(h);
            else if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)))
                ReleaseCapture();
        }
        return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch
                           * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* A 2x-wide anti-aliased 32-bit surface (pitch = 2 * width * 4):
             * take every other pixel. X-Men Legends renders its 640-wide
             * frame into a 1280-wide surface, which read as "8 bytes per
             * pixel" and showed black. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

/* GDI: the frame letterboxed into the client area, like the D3D11
 * presenter; the borders are cleared when the size changes. */
static void gdi_blit(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    static LONG lw, lh;
    RECT c;
    LONG cw, ch, w, h;
    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (cw != lw || ch != lh) {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        lw = cw; lh = ch;
    }
    if (xbox_GfxConfig()->stretch) {
        w = cw; h = ch;
    } else if ((long long)cw * s_fb_height <= (long long)ch * s_fb_width) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / s_fb_width);
    } else {
        h = ch; w = (LONG)((long long)ch * s_fb_width / s_fb_height);
    }
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);     /* HALFTONE wants it after the mode */
    StretchDIBits(hdc, (cw - w) / 2, (ch - h) / 2, w, h,
                  0, 0, (int)s_fb_width, (int)s_fb_height,
                  s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
}

/* The converted frame in s_rgb, on screen: D3D11 when it is up (which waits
 * for vsync, so returns 1 when the loop is already paced), else GDI. */
static int fb_show(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    if (gpu_present_active())
        return gpu_present_frame(s_rgb);
    gdi_blit(hwnd, hdc, bi);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    int fb_pinned;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        /* RECOMP_WINDOW_SCALE (1-8, default 2, as in the SDL window): the
         * client area is the frame times this, made smaller until the window
         * fits the desktop work area, and centred on it. */
        const char *e = getenv("RECOMP_WINDOW_SCALE");
        int scale = e ? atoi(e) : 2;
        RECT work;
        if (scale < 1 || scale > 8)
            scale = 2;
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
        }
        for (;; scale--) {
            r.left = 0; r.top = 0;
            r.right = (LONG)s_fb_width * scale; r.bottom = (LONG)s_fb_height * scale;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (scale <= 1 || (r.right - r.left <= work.right - work.left
                               && r.bottom - r.top <= work.bottom - work.top))
                break;
        }
        {
            /* A saved window size wins over the scale heuristic. */
            const char *ew = getenv("RECOMP_WINDOW_W");
            const char *eh = getenv("RECOMP_WINDOW_H");
            int cw = ew ? atoi(ew) : 0, chh = eh ? atoi(eh) : 0;
            if (cw >= 320 && chh >= 240 && cw <= 7680 && chh <= 4320) {
                int i;
                r.left = 0; r.top = 0; r.right = cw; r.bottom = chh;
                AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
                for (i = 0; i < N_WIN_PRESETS; i++)
                    if (s_win_presets[i].w == cw && s_win_presets[i].h == chh)
                        s_win_preset = i;
            }
        }
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                               work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    s_hwnd_pub = hwnd;
    {
        XboxGfxConfig *g = xbox_GfxConfig();
        if (g->show_fps)
            menu_render();
    }
    hdc = GetDC(hwnd);
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs && *fs != '0')
            fb_toggle_fullscreen(hwnd);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
    /* A window pinned to one address shows that memory and nothing else. */
    fb_pinned = getenv("RECOMP_FB_VA") != NULL;

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled())
        gpu_present_init(hwnd, s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        int paced = 0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        {
            /* While the back end presents into the swap chain, GDI stays
             * out of the client area; it comes back by itself (FMVs, a
             * lost device) when no GPU present lands for a while. */
            int gp = InterlockedCompareExchange(&s_gpu_present_on, 0, 0)
                     && GetTickCount() - s_gpu_present_tick < 250;
            if (gp) {
                MSG msg2;
                while (PeekMessageA(&msg2, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg2);
                    DispatchMessageA(&msg2);
                }
                if (s_rgb)
                    fb_overlay();       /* keeps FMV detection alive */
                goto titlebar;
            }
        }
        if (s_rgb && fb_copy_presented_frame(s_rgb)) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            if (!fb_pinned)
                fb_overlay();
            paced = fb_show(hwnd, hdc, &bi);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            if (!fb_pinned)
                fb_overlay();
            paced = fb_show(hwnd, hdc, &bi);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
    titlebar:
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                s_fps_shown = t0 ? (float)((f - f0) * 1000.0 / (now - t0)) : 0.0f;
                if ((xbox_GfxConfig()->show_fps || s_status_cb) && !s_menu_open)
                    menu_render();
                t0 = now; f0 = f;
            }
        }
        /* A pad has no auto-repeat of its own, so the menu has to sample it
         * rather than wait for events. Cheap: one read per frame. */
        menu_pad_poll();

        if (!paced)
            Sleep(16);                  /* D3D11's Present waited for vsync */
    }

    s_hwnd_pub = NULL;
    gpu_present_shutdown();
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    free(s_pvideo_source);
    s_pvideo_source = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#endif /* _WIN32: POSIX hosts get the SDL2 window in fb_present_sdl.c */
