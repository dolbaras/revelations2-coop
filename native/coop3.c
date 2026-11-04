/*
 * Stage 3: run code on the game's logic loop thread, and a toolbox of console commands that call/poke the game safely.
 *
 * The game's main thread loop (exe 0xC754C0, the thread that also runs the area update `aGame_Update`) does per pass
 *     render step 0xCCAD80 (this = sBioRender),  then app->vf[6]() = the frame update.
 * At 0xC75578 the loop loads `mov ecx,[sBioRender]` (6 bytes, ecx/eax/edx dead there). We replace those 6 bytes by `call stub; nop`
 * where the stub calls lt_hook() and then does the original `mov ecx,[sBioRender]; ret`. lt_hook runs once per loop pass ON THE
 * GAME THREAD, between two updates, so calls into game code from here are as safe as the game's own. Console threads post
 * a task (`L <cmd>` or any command that needs the game thread) and wait for it.
 *
 * Only for the supported build (bytes are checked). Everything is console driven; nothing happens unless a command asks.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "r2.h"

#define LOOP_PATCH 0xC75578u

static unsigned rd32(unsigned a);
static int wr(unsigned a, const void *p, unsigned n);

static volatile LONG lt_ready;
static volatile DWORD lt_tid;
static volatile unsigned lt_frames;
static volatile LONG lt_hits_other;

/* ---- task hand-over: one task at a time (the console is one thread) ---- */
static struct {
    volatile LONG state;   /* 0 idle, 1 posted, 2 running, 3 done */
    char cmd[400];
    char out[7000];
    int outsize;
} task;
static CRITICAL_SECTION task_lock;
static HANDLE task_done;

static void frame_callbacks(void);
static void coop_game_frame(void);   /* coop_game.inc (stage 4) */
static void bot_frame(void);         /* bot.inc (stage 6c test aid) */
void s3_run(const char *cmd, char *out, int size);

static void lt_hook(void)
{
    lt_tid = GetCurrentThreadId();
    lt_frames++;
    if (task.state == 1 && InterlockedCompareExchange(&task.state, 2, 1) == 1) {
        task.out[0] = 0;
        s3_run(task.cmd, task.out, task.outsize);
        InterlockedExchange(&task.state, 3);
        SetEvent(task_done);
    }
    frame_callbacks();
}

/* Test windows (REV2_BACKGROUND=1) are never really activated; any WM_ACTIVATE that Windows sends because another program took the focus makes the game's
 * loop idle in Sleep(1) (app+0x2551 = 0, see stage2a.md section 1), which looked like a random 6 s stall of every game-thread command. This watchdog keeps the
 * "window active" byte set while the game runs (only with REV2_BACKGROUND=1). */
static DWORD WINAPI keepalive_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(50);
        unsigned app = rd32(0x15DF9CC);
        if (app && !(rd32(app + 0x2660) & 0xFF)) {
            unsigned char one = 1, cur = 0;
            r2_safe_read((void *)(app + 0x2551), &cur, 1);
            if (!cur)
                wr(app + 0x2551, &one, 1);
        }
    }
    return 0;
}

int lt_on_game_thread(void) { return lt_ready && GetCurrentThreadId() == lt_tid; }

/* Run cmd on the game thread; the answer is copied to out. */
void lt_post(const char *cmd, char *out, int size)
{
    if (!lt_ready) {
        snprintf(out, size, "logic hook not installed (game not rendering yet?)");
        return;
    }
    if (lt_on_game_thread()) {
        s3_run(cmd, out, size);
        return;
    }
    EnterCriticalSection(&task_lock);
    snprintf(task.cmd, sizeof task.cmd, "%s", cmd);
    task.outsize = sizeof task.out;
    ResetEvent(task_done);
    InterlockedExchange(&task.state, 1);
    DWORD w = WaitForSingleObject(task_done, 6000);
    if (w != WAIT_OBJECT_0) {
        if (InterlockedCompareExchange(&task.state, 0, 1) == 1) {
            snprintf(out, size, "timeout: the game loop did not run the task in 6 s (paused/minimised?); cancelled");
            LeaveCriticalSection(&task_lock);
            return;
        }
        WaitForSingleObject(task_done, 20000);   /* it is running: wait for it */
    }
    snprintf(out, size, "%s", task.out);
    InterlockedExchange(&task.state, 0);
    LeaveCriticalSection(&task_lock);
}

void lt_install(void)
{
    static int done;
    if (done || !r2_text_decrypted())
        return;
    done = 1;
    static const unsigned char expect[] = {0x8B, 0x0D, 0x88, 0x03, 0x5E, 0x01, 0xE8};
    unsigned char cur[7];
    if (!r2_safe_read((void *)LOOP_PATCH, cur, 7) || memcmp(cur, expect, 7) != 0) {
        r2_log("coop3: loop bytes at %08x differ from the supported build, logic hook NOT installed", LOOP_PATCH);
        return;
    }
    InitializeCriticalSection(&task_lock);
    task_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    unsigned char *stub = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub)
        return;
    stub[0] = 0xE8;
    *(int *)(stub + 1) = (int)((unsigned)lt_hook - (unsigned)(stub + 5));
    static const unsigned char tail[] = {0x8B, 0x0D, 0x88, 0x03, 0x5E, 0x01, 0xC3};
    memcpy(stub + 5, tail, sizeof tail);
    DWORD old;
    if (!VirtualProtect((void *)LOOP_PATCH, 6, PAGE_EXECUTE_READWRITE, &old))
        return;
    unsigned char *p = (unsigned char *)LOOP_PATCH;
    p[0] = 0xE8;
    *(int *)(p + 1) = (int)((unsigned)stub - (LOOP_PATCH + 5));
    p[5] = 0x90;
    VirtualProtect((void *)LOOP_PATCH, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)LOOP_PATCH, 6);
    InterlockedExchange(&lt_ready, 1);
    if (testmode_background())
        CreateThread(NULL, 0, keepalive_thread, NULL, 0, NULL);
    r2_log("coop3: logic-thread hook installed at %08x (stub %p)", LOOP_PATCH, stub);
}

/* ---- memory helpers ---- */
static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r2_safe_read((void *)a, &v, 4);
    return v;
}
static float rdf(unsigned a)
{
    float v = 0;
    r2_safe_read((void *)a, &v, 4);
    return v;
}
static int wr(unsigned a, const void *p, unsigned n)
{
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)a, p, n, &got) && got == n;
}
static unsigned hx(const char *s) { return (unsigned)strtoul(s, NULL, 16); }
static float arg_float(const char *s) { return (float)atof(s); }

/* ---- generic call: call <cdecl|stdcall|thiscall|fastcall> <hexaddr> [args]; arg "f:1.5" = float bits; thiscall arg0 = ecx, fastcall arg0/1 = ecx/edx ---- */
#define TC __attribute__((thiscall))
#define FC __attribute__((fastcall))
#define SC __attribute__((stdcall))
#define CD __attribute__((cdecl))
typedef unsigned u;
static unsigned do_call(int conv, unsigned fn, const unsigned *a, int n)
{
    switch (conv) {
    case 0:   /* cdecl */
        switch (n) {
        case 0: return ((u(CD *)(void))fn)();
        case 1: return ((u(CD *)(u))fn)(a[0]);
        case 2: return ((u(CD *)(u, u))fn)(a[0], a[1]);
        case 3: return ((u(CD *)(u, u, u))fn)(a[0], a[1], a[2]);
        case 4: return ((u(CD *)(u, u, u, u))fn)(a[0], a[1], a[2], a[3]);
        case 5: return ((u(CD *)(u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4]);
        default: return ((u(CD *)(u, u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    case 1:   /* stdcall */
        switch (n) {
        case 0: return ((u(SC *)(void))fn)();
        case 1: return ((u(SC *)(u))fn)(a[0]);
        case 2: return ((u(SC *)(u, u))fn)(a[0], a[1]);
        case 3: return ((u(SC *)(u, u, u))fn)(a[0], a[1], a[2]);
        case 4: return ((u(SC *)(u, u, u, u))fn)(a[0], a[1], a[2], a[3]);
        case 5: return ((u(SC *)(u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4]);
        default: return ((u(SC *)(u, u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    case 2:   /* thiscall: a[0] = ecx */
        switch (n) {
        case 0:
        case 1: return ((u(TC *)(u))fn)(a[0]);
        case 2: return ((u(TC *)(u, u))fn)(a[0], a[1]);
        case 3: return ((u(TC *)(u, u, u))fn)(a[0], a[1], a[2]);
        case 4: return ((u(TC *)(u, u, u, u))fn)(a[0], a[1], a[2], a[3]);
        case 5: return ((u(TC *)(u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4]);
        default: return ((u(TC *)(u, u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    default:  /* fastcall: a[0] = ecx, a[1] = edx */
        switch (n) {
        case 0:
        case 1: return ((u(FC *)(u))fn)(a[0]);
        case 2: return ((u(FC *)(u, u))fn)(a[0], a[1]);
        case 3: return ((u(FC *)(u, u, u))fn)(a[0], a[1], a[2]);
        case 4: return ((u(FC *)(u, u, u, u))fn)(a[0], a[1], a[2], a[3]);
        case 5: return ((u(FC *)(u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4]);
        default: return ((u(FC *)(u, u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    }
}

static unsigned game_area(void)   /* sArea_GetGameArea: thiscall ecx = [0x15DE720] */
{
    unsigned sarea = rd32(0x15DE720);
    if (!sarea)
        return 0;
    unsigned a[1] = {sarea};
    return do_call(2, 0x44D690, a, 1);
}

static unsigned player_obj(int slot)
{
    unsigned sp = rd32(0x1567EAC);
    return sp && slot >= 0 && slot < 8 ? rd32(sp + 0x20 + 4 * (unsigned)slot) : 0;
}

/* ---- puppet recording / replay (see below) ---- */
typedef struct { float t; float v[16]; } sample_t;
void puppet_cmd(const char *name, const char *arg, char *out, int size);
static void puppet_frame(void);

/* viewguard <slot>|off: keep "this PC shows only <slot>'s player full screen" (mode 0, viewport 0 = slot) after anything the game does (join/leave, section
 * change, checkpoint load, end of a cutscene). Cutscene modes 2/3 are left alone. Does not use the camera byte +0x1270 (the game uses that one itself). */
static volatile int vg_slot = -1;
static unsigned vg_fixes;
static void view_guard(void)
{
    int want = vg_slot;
    if (want < 0)
        return;
    unsigned gm = rd32(0x157AE00);
    if (!gm)
        return;
    unsigned mode = rd32(gm + 0x8F4);
    if (mode == 2 || mode == 3)
        return;
    static unsigned stable;
    if (mode != 0 || (int)rd32(gm + 0x8F8) != want)
        stable++;
    else
        stable = 0;
    if (stable >= 20) {   /* the wrong view has lasted 20 passes: not a transition (load, cutscene change) */
        stable = 0;
        unsigned a[3] = {gm, 0, (unsigned)want};
        do_call(2, 0x8882D0, a, 3);   /* GameMan_SetSlot(0, want) */
        unsigned m[3] = {gm, 0, 1};
        do_call(2, 0x8882F0, m, 3);   /* GameMan_SetMode(0, 1) */
        unsigned cam = rd32(0x15DE88C), c[1] = {cam};
        do_call(2, 0x4AE570, c, 1);   /* Camera_RefreshViewports */
        vg_fixes++;
    }
}

static void frame_callbacks(void)
{
    view_guard();
    console_shake_tick();
    puppet_frame();
    coop_game_frame();
    bot_frame();
    menu_frame();
}

static void players_summary(char *out, int size)
{
    int o = 0;
    unsigned gm = rd32(0x157AE00), sp = rd32(0x1567EAC);
    o += snprintf(out + o, size - o, "gm %08x type5c=%u flag8f0=%u mode8f4=%u vp8f8=%d,%d main7a8=%u exist7ac=%x human7b0=%x types7b4=", gm, rd32(gm + 0x5C),
                  rd32(gm + 0x8F0), rd32(gm + 0x8F4), (int)rd32(gm + 0x8F8), (int)rd32(gm + 0x8FC), rd32(gm + 0x7A8), rd32(gm + 0x7AC), rd32(gm + 0x7B0));
    for (int i = 0; i < 2; i++)
        o += snprintf(out + o, size - o, "%u,", rd32(gm + 0x7B4 + 4 * (unsigned)i));
    o += snprintf(out + o, size - o, " seats7d4=%d,%d b900=%u\nsPlayer %08x count=%u\n", (int)rd32(gm + 0x7D4), (int)rd32(gm + 0x7D8), rd32(gm + 0x900) & 0xFF, sp, rd32(sp + 0x40));
    for (int i = 0; i < 2; i++) {
        unsigned p = player_obj(i);
        if (!p) {
            o += snprintf(out + o, size - o, " slot%d: none\n", i);
            continue;
        }
        char cn[200];
        classes_name(p, cn, sizeof cn);
        char *nl = strchr(cn, '\n');
        if (nl)
            *nl = 0;
        unsigned b = rd32(p + 0x790C);
        o += snprintf(out + o, size - o, " slot%d: %08x %.40s slotidx=%u type7920=%u seat7924=%u aux=%u hp=%d/%d flags+c=%08x pos=(%.1f,%.1f,%.1f) in5940=%08x\n", i, p, cn,
                      b & 0xFF, rd32(p + 0x7920), rd32(p + 0x7924) & 0xFF, (rd32(p + 0x7924) >> 8) & 0xFF, (int)rd32(p + 0x1A08), (int)rd32(p + 0x1A0C), rd32(p + 0xC),
                      rdf(p + 0x40), rdf(p + 0x44), rdf(p + 0x48), rd32(p + 0x5940));
    }
    unsigned ar = game_area();
    o += snprintf(out + o, size - o, "aGame %08x req6dc=%u pad6e0=%d st6e4=%u 6e8=%x", ar, rd32(ar + 0x6DC), (int)rd32(ar + 0x6E0), rd32(ar + 0x6E4), rd32(ar + 0x6E8));
}

static void pads_summary(char *out, int size)
{
    unsigned bp = rd32(0x15DE5DC);
    int o = snprintf(out, size, "sBioPad %08x logical->physical: p0=%d p1=%d ; input-off byte %02x\n", bp, (int)rd32(bp + 0xBA0), (int)rd32(bp + 0xBA4), rd32(bp + 0xBC0) & 0xFF);
    for (int i = 0; i < 4; i++) {
        unsigned s = bp + 0x2CC * (unsigned)i;
        unsigned char f[0x10];
        r2_safe_read((void *)(s + 0x40), f, 0x10);
        o += snprintf(out + o, size - o, " pad%d: act=%u xin=%u idx=%u conn=%u cap=%08x user=%d held=%08x pressed=%08x\n", i, f[0], f[1], f[2], f[3], rd32(s + 0x48), (int)rd32(s + 0x4C),
                      rd32(s + 0x198), rd32(s + 0x1A0));
    }
}

/* ---- the command set ---- */
void s3_run(const char *cmd, char *out, int size)
{
    char name[32] = "", arg[360] = "";
    sscanf(cmd, "%31s %359[^\r\n]", name, arg);
    out[0] = 0;
    if (!strcmp(name, "call")) {
        char cv[16], fa[16];
        unsigned a[8] = {0};
        int n = 0;
        char *rest = arg;
        int consumed = 0;
        if (sscanf(rest, "%15s %15s%n", cv, fa, &consumed) < 2) {
            snprintf(out, size, "usage: call <cdecl|stdcall|thiscall|fastcall> <hexaddr> [hex args or f:float...] (up to 6)");
            return;
        }
        rest += consumed;
        char tok[40];
        int c2;
        while (n < 6 && sscanf(rest, "%39s%n", tok, &c2) == 1) {
            a[n++] = tok[0] == 'f' && tok[1] == ':' ? ({ float f = arg_float(tok + 2); unsigned b; memcpy(&b, &f, 4); b; }) : hx(tok);
            rest += c2;
        }
        int conv = !strcmp(cv, "cdecl") ? 0 : !strcmp(cv, "stdcall") ? 1 : !strcmp(cv, "thiscall") ? 2 : 3;
        unsigned r = do_call(conv, hx(fa), a, n);
        snprintf(out, size, "returned %08x (%d)", r, (int)r);
    } else if (!strcmp(name, "w32") || !strcmp(name, "w8") || !strcmp(name, "wf")) {
        char as[20], vs[40];
        if (sscanf(arg, "%19s %39s", as, vs) != 2) {
            snprintf(out, size, "usage: %s <hexaddr> <value>", name);
            return;
        }
        int ok;
        if (!strcmp(name, "w32")) {
            unsigned v = hx(vs);
            ok = wr(hx(as), &v, 4);
        } else if (!strcmp(name, "w8")) {
            unsigned char v = (unsigned char)hx(vs);
            ok = wr(hx(as), &v, 1);
        } else {
            float v = arg_float(vs);
            ok = wr(hx(as), &v, 4);
        }
        snprintf(out, size, ok ? "written" : "write failed");
    } else if (!strcmp(name, "rf")) {
        char as[20];
        int n = 4;
        sscanf(arg, "%19s %d", as, &n);
        int o = 0;
        for (int i = 0; i < n && i < 64 && o + 24 < size; i++)
            o += snprintf(out + o, size - o, "%.3f ", rdf(hx(as) + 4 * (unsigned)i));
    } else if (!strcmp(name, "pls")) {
        players_summary(out, size);
    } else if (!strcmp(name, "pads3")) {
        pads_summary(out, size);
    } else if (!strcmp(name, "area")) {
        unsigned ar = game_area();
        snprintf(out, size, "aGame %08x", ar);
    } else if (!strcmp(name, "ltinfo")) {
        snprintf(out, size, "logic hook %s, tid %lu, passes %u", lt_ready ? "installed" : "NOT installed", (unsigned long)lt_tid, lt_frames);
    } else if (!strcmp(name, "join") || !strcmp(name, "leave")) {
        /* join <pad>: what Coop_PollStartOnOtherPad writes; the game's own state machine then does the join on its next update.
         * leave: the "4" request of a lost pad (shows the game's confirmation popup) ; leave now: ApplyJoinLeave directly */
        unsigned ar = game_area();
        if (!ar) {
            snprintf(out, size, "no aGame (not in an episode)");
            return;
        }
        if (!strcmp(name, "join")) {
            unsigned pad = hx(arg), z = 0, one = 1;
            wr(ar + 0x6E0, &pad, 4), wr(ar + 0x6E4, &z, 4), wr(ar + 0x6DC, &one, 4);
            snprintf(out, size, "join requested: aGame %08x 6dc=1 6e0=%u", ar, pad);
        } else {
            unsigned four = !strncmp(arg, "now", 3) ? 0 : 4;
            if (four == 0) {
                static unsigned char buf[0x800];
                memset(buf, 0, sizeof buf);
                unsigned a[1] = {(unsigned)buf};
                do_call(3, 0x4287B0, a, 1);
                snprintf(out, size, "ApplyJoinLeave(leave) called directly");
            } else {
                unsigned z = 0;
                wr(ar + 0x6E4, &z, 4), wr(ar + 0x6DC, &four, 4);
                snprintf(out, size, "leave requested (6dc=4)");
            }
        }
    } else if (!strcmp(name, "joinnow")) {
        static unsigned char buf[0x800];
        memset(buf, 0, sizeof buf);
        *(unsigned *)(buf + 0x6DC) = 1;
        *(unsigned *)(buf + 0x6E0) = hx(arg);
        unsigned a[1] = {(unsigned)buf};
        do_call(3, 0x4287B0, a, 1);
        snprintf(out, size, "ApplyJoinLeave(join, pad %u) called directly", hx(arg));
    } else if (!strcmp(name, "slottype")) {   /* slottype <slot> <type>: GameMan_SetSlotType + sPlayer_RefreshSlotFromGameMan */
        unsigned sl, ty;
        if (sscanf(arg, "%u %u", &sl, &ty) != 2) {
            snprintf(out, size, "usage: slottype <slot> <0 human|1 AI|2 remote>");
            return;
        }
        unsigned gm = rd32(0x157AE00), a[3] = {gm, sl, ty};
        do_call(2, 0x887F50, a, 3);
        unsigned b[2] = {rd32(0x1567EAC), sl};
        do_call(2, 0x6E6110, b, 2);
        snprintf(out, size, "slot %u type -> %u", sl, ty);
    } else if (!strcmp(name, "view")) {   /* view <mode> [lock 0|1]: GameMan_SetMode(mode,1) and the camera byte that keeps it */
        unsigned m = 0, lock = 1;
        sscanf(arg, "%u %u", &m, &lock);
        unsigned cam = rd32(0x15DE88C);
        unsigned char lb = (unsigned char)lock;
        wr(cam + 0x1270, &lb, 1);
        unsigned a[3] = {rd32(0x157AE00), m, 1};
        do_call(2, 0x8882F0, a, 3);
        unsigned c[1] = {cam};
        do_call(2, 0x4AE570, c, 1);
        snprintf(out, size, "mode %u set, camera lock byte %u", m, lock);
    } else if (!strcmp(name, "replace")) {   /* replace <slot> <hexclasshash>: sPlayer_ReplaceCharacter + restore the input flags the new object lacks (+0x7900) */
        unsigned sl = 0, hash = 0;
        char hs[20];
        if (sscanf(arg, "%u %19s", &sl, hs) != 2) {
            snprintf(out, size, "usage: replace <slot> <classhash hex: 1001601 Claire uPl2210, 1001701 Moira uPl2310>");
            return;
        }
        hash = hx(hs);
        unsigned old = player_obj((int)sl);
        if (!old) {
            snprintf(out, size, "no player in slot %u", sl);
            return;
        }
        unsigned flags = rd32(old + 0x7900), ac = rd32(0x1567EAC);
        unsigned a[3] = {ac, hash, sl};
        do_call(2, 0x6E8E00, a, 3);
        unsigned n = player_obj((int)sl);
        if (n && n != old)
            wr(n + 0x7900, &flags, 4);
        snprintf(out, size, "slot %u: %08x -> %08x (class hash %x), flags +7900 restored to %08x", sl, old, n, hash, flags);
    } else if (!strcmp(name, "viewguard")) {
        if (!strncmp(arg, "off", 3))
            vg_slot = -1;
        else
            vg_slot = atoi(arg);
        snprintf(out, size, "viewguard %d (corrections so far %u)", vg_slot, vg_fixes);
    } else if (!strcmp(name, "vp")) {   /* vp <viewport> <player slot|-1>: GameMan_SetSlot */
        int v = 0, p = 0;
        sscanf(arg, "%d %d", &v, &p);
        unsigned a[3] = {rd32(0x157AE00), (unsigned)v, (unsigned)p};
        do_call(2, 0x8882D0, a, 3);
        unsigned cam = rd32(0x15DE88C), c[1] = {cam};
        do_call(2, 0x4AE570, c, 1);
        snprintf(out, size, "viewport %d shows player %d", v, p);
    } else if (!strcmp(name, "rec") || !strcmp(name, "play") || !strcmp(name, "stopplay") || !strcmp(name, "pupinfo") || !strcmp(name, "stoprec") || !strcmp(name, "pset")) {
        puppet_cmd(name, arg, out, size);
    } else {
        extern void console_exec(const char *, char *, int);
        console_exec(cmd, out, size);
    }
}

/* ========================= puppet recording and replay =========================
 * State of one character (the "network packet"): see puppet_state_* below; filled in after the field survey. */
#include "puppet.inc"
#include "coop_game.inc"
#include "world.inc"
#include "ach.inc"
#include "bot.inc"
