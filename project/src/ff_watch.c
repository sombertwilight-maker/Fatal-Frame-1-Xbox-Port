/* ff_watch.c -- find who overwrites one guest dword.
 *
 * RECOMP_DRWATCH=<guest va>[,<guest va>] (hex). Off when unset or 0.
 *
 * Two ways a dword can change here, and each needs its own catcher:
 *
 *   - guest code (or a runtime helper on a guest thread) stores to it. A
 *     hardware data breakpoint (DR0/DR1, write, 4 bytes) set on every thread
 *     in the process traps right after the store; the handler names the
 *     thread, the host RIP, the guest registers and the guest call chain.
 *
 *   - something outside the CPU writes it: a file read the kernel completes
 *     (ReadFile into guest memory does not trip a debug register), or a
 *     device model's host thread that this watcher has not armed yet. A poll
 *     every millisecond catches those, says old -> new, and the [READ] lines
 *     around it (which now carry buf=) say whether a read covered it.
 *
 * Threads created later are armed on the next sweep (every 100 ms). A
 * thread is suspended only while its context is read and written, and
 * nothing is printed while any thread is suspended. */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __thread uint32_t g_ebx, g_esi, g_edi;

#define MAXW 2
static uint32_t s_va[MAXW];
static int s_nw;
static volatile LONG s_hits;
static DWORD s_self;

#define MAXARM 512
static DWORD s_armed[MAXARM];
static int s_narmed;

static volatile uint32_t *host_of(uint32_t va)
{
    return (volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + va);
}

static LONG CALLBACK watch_veh(PEXCEPTION_POINTERS ep)
{
    CONTEXT *c = ep->ContextRecord;
    int k;

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!(c->Dr6 & 3u))
        return EXCEPTION_CONTINUE_SEARCH;
    for (k = 0; k < s_nw; k++) {
        if (!(c->Dr6 & (1u << k)))
            continue;
        if (InterlockedIncrement(&s_hits) <= 60) {
            uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
            const uint32_t *sp = g_esp ? (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp) : NULL;
            int i, n = 0;
            fprintf(stderr, "[DRWATCH] write to 0x%08X = 0x%08X by tid %lu, host rip +%llX\n"
                            "  regs eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n"
                            "  guest chain:",
                    s_va[k], *host_of(s_va[k]), GetCurrentThreadId(),
                    (unsigned long long)(c->Rip - base),
                    g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
            for (i = 0; sp && i < 256 && n < 12; i++)
                if (sp[i] > g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
                    fprintf(stderr, " %08X", sp[i]);
                    n++;
                }
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int was_armed(DWORD tid)
{
    int i;
    for (i = 0; i < s_narmed; i++)
        if (s_armed[i] == tid) return 1;
    return 0;
}

/* Arm every thread not armed yet; returns how many were. */
static int arm_sweep(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    int armed = 0;

    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            HANDLE th;
            CONTEXT ctx;
            int k;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == s_self
                || was_armed(te.th32ThreadID) || s_narmed >= MAXARM)
                continue;
            th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                            FALSE, te.th32ThreadID);
            if (!th)
                continue;
            if (SuspendThread(th) != (DWORD)-1) {
                memset(&ctx, 0, sizeof ctx);
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(th, &ctx)) {
                    for (k = 0; k < s_nw; k++) {
                        (&ctx.Dr0)[k] = (DWORD64)(uintptr_t)host_of(s_va[k]);
                        ctx.Dr7 |= (1ull << (2 * k));                 /* local enable  */
                        ctx.Dr7 &= ~(0xFull << (16 + 4 * k));
                        ctx.Dr7 |= (0xDull << (16 + 4 * k));          /* RW=01 write, LEN=11 4 bytes */
                    }
                    if (SetThreadContext(th, &ctx)) {
                        s_armed[s_narmed++] = te.th32ThreadID;
                        armed++;
                    }
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return armed;
}

extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
static int s_auto;

static DWORD WINAPI watch_thread(LPVOID unused)
{
    uint32_t last[MAXW];
    unsigned tick = 0;
    int k, polls_logged = 0;

    (void)unused;
    s_self = GetCurrentThreadId();
    while (!g_xbox_mem_offset)
        Sleep(10);
    if (s_auto) {
        /* "usb": the USB host driver's own object, the ServiceContext it
         * connected its ISR with (vector 1). Its first dword is the
         * controller's register base, which the scene1031 crash found
         * overwritten. Where the pool puts it differs between machines. */
        uint32_t ki, ctx;
        while (!(ki = xbox_GetConnectedInterrupt(1)))
            Sleep(5);
        ctx = *host_of(ki + 4);
        s_va[0] = ctx;
        s_va[1] = ctx + 4;
        s_nw = 2;
    }
    for (k = 0; k < s_nw; k++)
        last[k] = *host_of(s_va[k]);
    fprintf(stderr, "[DRWATCH] watching");
    for (k = 0; k < s_nw; k++)
        fprintf(stderr, " 0x%08X (now 0x%08X)", s_va[k], last[k]);
    fprintf(stderr, "\n");
    for (;;) {
        if (tick++ % 100 == 0) {
            int n = arm_sweep();
            if (n && tick < 2000)
                fprintf(stderr, "[DRWATCH] armed %d more threads (%d total)\n", n, s_narmed);
        }
        for (k = 0; k < s_nw; k++) {
            uint32_t v = *host_of(s_va[k]);
            if (v != last[k]) {
                if (polls_logged++ < 60)
                    fprintf(stderr, "[DRWATCH] poll: 0x%08X changed 0x%08X -> 0x%08X\n",
                            s_va[k], last[k], v);
                last[k] = v;
            }
        }
        Sleep(1);
    }
    return 0;
}

void ff_watch_start(void)
{
    const char *e = getenv("RECOMP_DRWATCH");
    char *p;

    fprintf(stderr, "[DRWATCH] spec %s\n", e ? e : "(unset)");
    if (!e || !*e || (e[0] == '0' && !e[1]))
        return;
    p = (char *)e;
    if (!strncmp(p, "usb", 3)) {
        s_auto = 1;
        s_nw = 2;
        p += 3;
    }
    while (!s_auto && *p && s_nw < MAXW) {
        uint32_t va = (uint32_t)strtoul(p, &p, 16) & ~3u;
        if (va)
            s_va[s_nw++] = va;
        while (*p == ',' || *p == ' ')
            p++;
        if (*p && (*p < '0' || *p > 'f'))
            break;
    }
    if (!s_nw)
        return;
    AddVectoredExceptionHandler(1, watch_veh);
    CloseHandle(CreateThread(NULL, 0, watch_thread, NULL, 0, NULL));
}
