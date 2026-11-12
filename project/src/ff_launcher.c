/**
 * ff_launcher.c - Fatal Frame (USA) launcher glue
 *
 * The port ships no game data. On first run the player points it at their own
 * disc image (an XISO / redump ISO of Fatal Frame, USA); the files are
 * extracted once into game\ beside the executable, and default.xbe is checked
 * against the exact build this code was recompiled from.
 *
 * How the image is found, in order:
 *   1. a path given on the command line            fatalframe.exe "D:\FF.iso"
 *   2. any *.iso / *.xiso file beside the executable
 *   3. a file-open dialog
 */

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <io.h>

#include "ff_launcher.h"

/* The build the generated code was lifted from. A different XBE (another
 * region, a patched or trainer build) has different code at the same
 * addresses and would crash in confusing ways, so refuse it up front. */
#define FF_XBE_SIZE   1773568u
#define FF_XBE_CRC32  0x8C4A65ABu

#define SECTOR 2048u
static const char XMAGIC[20] = "MICROSOFT*XBOX*MEDIA";
static const uint64_t BASES[] = {
    0x00000000ull, 0x0000FD90ull, 0x00030600ull, 0x0FD90000ull, 0x18300000ull
};

char g_ff_root[MAX_PATH];        /* folder holding the .exe, no trailing \ */

/* ── small helpers ─────────────────────────────────────────── */

static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void fatal(const char *msg)
{
    fprintf(stderr, "\n[LAUNCHER] %s\n", msg);
    MessageBoxA(NULL, msg, "Fatal Frame", MB_ICONERROR | MB_OK);
    exit(1);
}

/* 0 = missing, 1 = matches, -1 = present but wrong build */
static int check_xbe(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t buf[65536];
    uint32_t crc = 0;
    uint64_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        crc = crc_update(crc, buf, n);
        total += n;
    }
    fclose(f);
    if (total == FF_XBE_SIZE && crc == FF_XBE_CRC32) return 1;
    fprintf(stderr, "[LAUNCHER] %s: size %llu crc %08X (expected %u / %08X)\n",
            path, (unsigned long long)total, crc, FF_XBE_SIZE, FF_XBE_CRC32);
    return -1;
}

/* ── XDVDFS reader ─────────────────────────────────────────── */

typedef struct {
    HANDLE   h;
    uint64_t base;
    uint64_t done, total;   /* progress */
    DWORD    last_tick;
} Iso;

static int iso_read(Iso *iso, uint64_t off, void *dst, uint32_t len)
{
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(iso->h, li, NULL, FILE_BEGIN)) return 0;
    uint8_t *p = dst;
    while (len) {
        DWORD got = 0, want = len > (1u << 24) ? (1u << 24) : len;
        if (!ReadFile(iso->h, p, want, &got, NULL) || got == 0) return 0;
        p += got; len -= got;
    }
    return 1;
}

static void progress(Iso *iso, const char *name)
{
    DWORD now = GetTickCount();
    if (now - iso->last_tick < 250 && iso->done < iso->total) return;
    iso->last_tick = now;
    double pct = iso->total ? 100.0 * (double)iso->done / (double)iso->total : 0;
    fprintf(stdout, "\r  extracting %5.1f%%  %-50.50s", pct, name);
    fflush(stdout);
}

static int extract_file(Iso *iso, uint32_t sector, uint32_t size, const char *dest)
{
    HANDLE out = CreateFileA(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) return 0;
    static uint8_t buf[1 << 20];
    uint64_t off = iso->base + (uint64_t)sector * SECTOR;
    uint32_t left = size;
    while (left) {
        uint32_t chunk = left > sizeof buf ? (uint32_t)sizeof buf : left;
        DWORD wr = 0;
        if (!iso_read(iso, off, buf, chunk) ||
            !WriteFile(out, buf, chunk, &wr, NULL) || wr != chunk) {
            CloseHandle(out);
            return 0;
        }
        off += chunk; left -= chunk;
        iso->done += chunk;
        progress(iso, dest);
    }
    CloseHandle(out);
    return 1;
}

/* Walk one directory. Entries sit contiguously in each 2 KB sector (the tree
 * links are not needed), terminated by 0xFF padding. pass 0 sums sizes, pass
 * 1 extracts. */
static int walk_dir(Iso *iso, uint32_t sector, uint32_t size,
                    const char *dest_dir, int pass)
{
    if (size == 0) return 1;
    uint8_t *data = malloc(size);
    if (!data) return 0;
    if (!iso_read(iso, iso->base + (uint64_t)sector * SECTOR, data, size)) {
        free(data);
        return 0;
    }
    int ok = 1;
    for (uint32_t sb = 0; sb < size && ok; sb += SECTOR) {
        uint32_t lim = size - sb < SECTOR ? size - sb : SECTOR;
        uint32_t off = 0;
        while (off + 0x0E <= lim) {
            const uint8_t *e = data + sb + off;
            uint16_t l = (uint16_t)(e[0] | e[1] << 8), r = (uint16_t)(e[2] | e[3] << 8);
            uint32_t esec = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
            uint32_t esz  = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
            uint8_t attr = e[12], nlen = e[13];
            if ((l == 0xFFFF && r == 0xFFFF) || nlen == 0) break;
            if (off + 0x0E + nlen > lim) break;
            char name[256];
            memcpy(name, e + 0x0E, nlen);
            name[nlen] = 0;
            char path[MAX_PATH * 2];
            snprintf(path, sizeof path, "%s\\%s", dest_dir, name);
            if (attr & 0x10) {
                if (pass == 1) CreateDirectoryA(path, NULL);
                ok = walk_dir(iso, esec, esz, path, pass);
            } else if (pass == 0) {
                iso->total += esz;
            } else {
                ok = extract_file(iso, esec, esz, path);
                if (!ok) fprintf(stderr, "\n[LAUNCHER] failed writing %s\n", path);
            }
            if (!ok) break;
            off += (0x0E + nlen + 3) & ~3u;
        }
    }
    free(data);
    return ok;
}

static int extract_iso(const char *iso_path, const char *game_dir)
{
    Iso iso = {0};
    iso.h = CreateFileA(iso_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (iso.h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[LAUNCHER] cannot open %s\n", iso_path);
        return 0;
    }
    uint8_t desc[SECTOR];
    int found = 0;
    for (size_t i = 0; i < sizeof BASES / sizeof BASES[0]; i++) {
        if (iso_read(&iso, BASES[i] + 32 * SECTOR, desc, SECTOR) &&
            memcmp(desc, XMAGIC, 20) == 0) {
            iso.base = BASES[i];
            found = 1;
            break;
        }
    }
    if (!found) {
        CloseHandle(iso.h);
        fprintf(stderr, "[LAUNCHER] %s is not an Xbox disc image\n", iso_path);
        return 0;
    }
    uint32_t root_sec = desc[0x14] | desc[0x15] << 8 | desc[0x16] << 16 | (uint32_t)desc[0x17] << 24;
    uint32_t root_sz  = desc[0x18] | desc[0x19] << 8 | desc[0x1A] << 16 | (uint32_t)desc[0x1B] << 24;

    CreateDirectoryA(game_dir, NULL);
    printf("[LAUNCHER] Extracting %s\n           to %s\n", iso_path, game_dir);
    int ok = walk_dir(&iso, root_sec, root_sz, game_dir, 0) &&
             walk_dir(&iso, root_sec, root_sz, game_dir, 1);
    printf("\n");
    CloseHandle(iso.h);
    if (ok)
        printf("[LAUNCHER] Extracted %.1f MB\n", iso.total / 1048576.0);
    return ok;
}

/* ── finding the player's image ────────────────────────────── */

static int find_iso_in(const char *dir, char *out, size_t cap)
{
    const char *pats[] = { "*.iso", "*.xiso" };
    for (int i = 0; i < 2; i++) {
        char pat[MAX_PATH];
        snprintf(pat, sizeof pat, "%s\\%s", dir, pats[i]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            snprintf(out, cap, "%s\\%s", dir, fd.cFileName);
            FindClose(h);
            return 1;
        }
    }
    return 0;
}

/* Beside the exe, then the folder above it (the port's folder dropped next
 * to the image). */
static int find_iso_beside_exe(char *out, size_t cap)
{
    char parent[MAX_PATH];
    if (find_iso_in(g_ff_root, out, cap))
        return 1;
    snprintf(parent, sizeof parent, "%s", g_ff_root);
    char *slash = strrchr(parent, '\\');
    if (slash) {
        *slash = 0;
        return find_iso_in(parent, out, cap);
    }
    return 0;
}

static int ask_for_iso(char *out, size_t cap)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    out[0] = 0;
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "Xbox disc image (*.iso;*.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    ofn.lpstrFile = out;
    ofn.nMaxFile = (DWORD)cap;
    ofn.lpstrTitle = "Select your Fatal Frame (USA) Xbox disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) && out[0];
}

/* ── runtime defaults ──────────────────────────────────────── */

static void default_env(const char *name, const char *value)
{
    if (!getenv(name)) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s=%s", name, value);
        _putenv(buf);
    }
}

/* fatalframe.ini beside the exe: one NAME=VALUE per line ('#' or ';'
 * starts a comment). Each sets an environment variable before the defaults
 * below, so a player can switch a runtime option without a command prompt. */
static void load_ini(void)
{
    char path[MAX_PATH], line[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s\\fatalframe.ini", g_ff_root);
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *e;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || !strchr(p, '='))
            continue;
        e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!getenv(p)) {               /* a real environment variable wins */
            char name[256];
            size_t n = (size_t)(strchr(p, '=') - p);
            if (n >= sizeof name) continue;
            memcpy(name, p, n); name[n] = 0;
            if (getenv(name)) continue;
            _putenv(p);
            printf("[LAUNCHER] fatalframe.ini: %s\n", p);
        }
    }
    fclose(f);
}

static void set_runtime_defaults(void)
{
    load_ini();
    /* Fatal Frame statically links XDK 4721 D3D, DirectSound and XAPI, so it
     * talks to the GPU, APU and USB controllers directly; these switch the
     * toolkit's hardware models on. Every one can be overridden by setting
     * the variable before launching (set it to 0 / empty to disable). */
    default_env("RECOMP_PB_EXEC", "1");        /* execute the NV2A pushbuffer */
    default_env("RECOMP_PB_WALK", "segment");  /* segment scan: the DMA walk needs
                                                 * the software-method IRQ */
    default_env("RECOMP_VBLANK", "paced");     /* 60 Hz vblank interrupts */
    default_env("RECOMP_FB_WINDOW", "1");      /* show the frame buffer */
    default_env("RECOMP_AC97_READY", "1");     /* emulated APU for audio */
    default_env("RECOMP_USB", "1");            /* USB gamepad model (XInput) */
    default_env("RECOMP_KEYBOARD", "1");       /* keyboard as controller 1 */
    default_env("RECOMP_CONTIG_AVOID_IMAGE", "1"); /* keep GPU/USB/APU blocks off
                                                     * the image's physical pages */
    default_env("RECOMP_TITLE_KEVENTS", "1");  /* D3D builds its fence event inline */
    default_env("RECOMP_FLIP_PACING", "1");    /* hold flips to the 60 Hz vblank */
    default_env("RECOMP_DISPATCH_GATE", "1");  /* one CPU at DISPATCH_LEVEL, as on the
                                                 * console. DirectSoundDoWork (0x00103D17)
                                                 * raises IRQL and moves the object's
                                                 * pending list (+0x758) while the DSound
                                                 * DPC, on another core without the gate,
                                                 * edits the same list: torn, the move
                                                 * loop never ends (the white-screen hang
                                                 * when a cutscene is skipped). The
                                                 * crashes seen with it on in v9/v10 were
                                                 * the renderer's copy-back writing
                                                 * pixels into DirectSound's memory,
                                                 * fixed since. */
    default_env("RECOMP_FLASH_LOG", "1");      /* diagnostic: log the glare/full-screen
                                                 * draws of any frame whose brightness
                                                 * suddenly jumps (the white flashes);
                                                 * 0 turns it off */
    default_env("RECOMP_VM_FIXED", "0x01000000:0x153A000");
                                                /* the game reserves its 21 MB working
                                                 * pool at 0x01000000 and uses that
                                                 * address whatever it is given; keep
                                                 * the range free for it (the scene1031
                                                 * crash: its scene load overwrote the
                                                 * USB driver placed there) */
    default_env("RECOMP_GUEST_LOCK", "1");     /* and one guest thread running at a
                                                 * time, as on the console's one CPU:
                                                 * the WMV decoder, DirectSound and the
                                                 * game's threads assume it, and with
                                                 * the gate alone the end of a cutscene
                                                 * still left garbage in DirectSound's
                                                 * voice lists (crash in its DPC) */

    {
        /* Every session's pad input, frame by frame, beside the exe
         * (padrec.txt; RECOMP_PAD_RECORD=0 turns it off): a route to a bug
         * can then be replayed on another machine (RECOMP_PAD_REPLAY). */
        char rec[MAX_PATH];
        const char *e = getenv("RECOMP_PAD_RECORD");
        if (e && !strcmp(e, "0"))
            _putenv("RECOMP_PAD_RECORD=");
        else if (!e) {
            snprintf(rec, sizeof rec, "%s\\padrec.txt", g_ff_root);
            default_env("RECOMP_PAD_RECORD", rec);
        }
    }

    char saves[MAX_PATH];
    snprintf(saves, sizeof saves, "%s\\saves", g_ff_root);
    CreateDirectoryA(saves, NULL);
    default_env("RECOMP_SAVE_DIR", saves);
}

/* ── entry ─────────────────────────────────────────────────── */

void ff_prepare(const char *cmdline)
{
    crc_init();

    GetModuleFileNameA(NULL, g_ff_root, sizeof g_ff_root);
    char *slash = strrchr(g_ff_root, '\\');
    if (slash) *slash = 0;
    SetCurrentDirectoryA(g_ff_root);

    char game_dir[MAX_PATH], xbe[MAX_PATH];
    snprintf(game_dir, sizeof game_dir, "%s\\game", g_ff_root);
    snprintf(xbe, sizeof xbe, "%s\\default.xbe", game_dir);

    /* A path on the command line always wins: it is how a player re-extracts
     * after replacing a bad image. */
    char iso[MAX_PATH * 2] = {0};
    if (cmdline && *cmdline) {
        const char *s = cmdline;
        while (*s == ' ') s++;
        if (*s == '"') { s++; }
        snprintf(iso, sizeof iso, "%s", s);
        char *q = strchr(iso, '"'); if (q) *q = 0;
        size_t n = strlen(iso);
        while (n && iso[n - 1] == ' ') iso[--n] = 0;
        if (iso[0] && !file_exists(iso)) iso[0] = 0;
    }

    int state = check_xbe(xbe);
    if (state != 1 || iso[0]) {
        if (!iso[0] && !find_iso_beside_exe(iso, sizeof iso)) {
            MessageBoxA(NULL,
                "Fatal Frame needs your own copy of the game.\n\n"
                "Select your Fatal Frame (USA) Xbox disc image (.iso) in the "
                "next window. It is extracted once into the 'game' folder "
                "beside this program (about 1.4 GB).",
                "Fatal Frame - first run", MB_ICONINFORMATION | MB_OK);
            if (!ask_for_iso(iso, sizeof iso))
                fatal("No disc image selected.");
        }
        if (!extract_iso(iso, game_dir))
            fatal("Could not extract the disc image. Is it a valid Xbox ISO, "
                  "and is there ~1.5 GB free beside the program?");
        state = check_xbe(xbe);
        if (state == 0)
            fatal("The disc image has no default.xbe. Is it the Xbox game disc?");
        if (state < 0)
            fatal("This disc image is not Fatal Frame (USA, Xbox).\n\n"
                  "This port was recompiled from that exact release and cannot "
                  "run other versions. See the console output for details.");
    }
    printf("[LAUNCHER] default.xbe verified (Fatal Frame USA, CRC %08X)\n", FF_XBE_CRC32);

    /* The runtime is chatty, and a Windows console is slow enough to stall
     * the threads that print. Log to a file beside the exe instead, unless
     * FF_LOG_CONSOLE=1. */
    if (!getenv("FF_LOG_CONSOLE")) {
        char logp[MAX_PATH];
        snprintf(logp, sizeof logp, "%s\\fatalframe.log", g_ff_root);
        printf("Fatal Frame (USA) - native Windows port\n"
               "Runtime log: %s\n\n", logp);
        fflush(stdout);
        if (freopen(logp, "w", stderr)) {
            setvbuf(stderr, NULL, _IONBF, 0);
            _dup2(_fileno(stderr), _fileno(stdout));
        }
    }
    set_runtime_defaults();
}
