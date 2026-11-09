/*
 * Test console, REV2_CONSOLE=<port>: UDP on 127.0.0.1 only (never another interface). One text command per datagram, one
 * text datagram back. Runs on its own thread: commands that read game memory use ReadProcessMemory on ourselves
 * (a bad address answers "unreadable" instead of crashing the game); nothing here calls into game code yet.
 *
 *   ping | version | help | base | stub | frames | iat | threads | stacks (every thread: where it is, likely return addresses)
 *   readmem <hexaddr> [len<=256]   hex dump          readstr <hexaddr> [max<=200]   ascii string
 *   dword <hexaddr>               one 32-bit value   classname <hexaddr>   (stage 2: object -> vtable[4] -> type info +4)
 *   shot                          save the next presented frame      log <text>   a line in the mod's log
 *   quit                          WM_CLOSE to the game window (the game's own exit)
 * Client: tools\r2console.py
 */
#include <winsock2.h>
#include <windows.h>
#include <xinput.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r2.h"

static unsigned long hex(const char *s) { return strtoul(s, NULL, 16); }

static void hexdump(const unsigned char *p, int n, char *out, int size)
{
    int o = 0;
    for (int i = 0; i < n && o + 4 < size; i++)
        o += snprintf(out + o, size - o, "%02x%s", p[i], (i & 15) == 15 && i + 1 < n ? "\n" : " ");
    if (n == 0)
        snprintf(out, size, "(empty)");
}


/* One thread: suspended only while its registers and stack are copied, then likely return addresses (a dword into any module's
 * code that follows a call) are printed as module+offset. Nothing allocates while it is suspended. */
static void where(DWORD addr, char *out, int size)
{
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, '\\');
        snprintf(out, size, "%s+%lx", b ? b + 1 : name, (unsigned long)(addr - (DWORD)m));
    } else
        snprintf(out, size, "%08lx", (unsigned long)addr);
}

static int thread_stack(DWORD tid, char *out, int size, int returns)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h)
        return snprintf(out, size, "thread %lu: cannot open\n", tid);
    CONTEXT c;
    DWORD ret[48], n = 0;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(h) == (DWORD)-1) {
        CloseHandle(h);
        return snprintf(out, size, "thread %lu: cannot suspend\n", tid);
    }
    int ok = GetThreadContext(h, &c);
    if (ok && returns) {
        DWORD stack[1024];
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), (void *)c.Esp, stack, sizeof stack, &got);
        for (DWORD i = 0; i < got / 4 && n < 48; i++) {
            const unsigned char *p = (const unsigned char *)stack[i];
            unsigned char code[6];
            if (stack[i] < 0x10000 || !r2_safe_read(p - 6, code, 6))
                continue;
            if (code[1] == 0xE8 || (code[4] == 0xFF && (code[5] & 0x38) == 0x10) || (code[3] == 0xFF && (code[4] & 0x38) == 0x10) || (code[0] == 0xFF && (code[1] & 0x38) == 0x10))
                ret[n++] = stack[i];
        }
    }
    ResumeThread(h);
    CloseHandle(h);
    if (!ok)
        return snprintf(out, size, "thread %lu: no context\n", tid);
    char w[300];
    where(c.Eip, w, sizeof w);
    int o = snprintf(out, size, "thread %lu eip=%s esp=%08lx\n", tid, w, c.Esp);
    for (DWORD i = 0; i < n && o + 80 < size; i++) {
        where(ret[i], w, sizeof w);
        o += snprintf(out + o, size - o, "    ret %s\n", w);
    }
    return o;
}

static void all_threads(char *out, int size, int returns)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 t = {sizeof t};
    int o = 0;
    DWORD me = GetCurrentThreadId();
    if (snap == INVALID_HANDLE_VALUE) {
        snprintf(out, size, "no snapshot");
        return;
    }
    for (BOOL ok = Thread32First(snap, &t); ok && o + 200 < size; ok = Thread32Next(snap, &t))
        if (t.th32OwnerProcessID == GetCurrentProcessId() && t.th32ThreadID != me)
            o += thread_stack(t.th32ThreadID, out + o, size - o, returns);
    CloseHandle(snap);
}

static void run(const char *cmd, char *out, int size);
static unsigned shake_until, shake_t, shake_half = 150;
static int shake_user, shake_dir;
void console_exec(const char *cmd, char *out, int size) { run(cmd, out, size); }

static int is_s3(const char *n)
{
    static const char *const list[] = {"call", "w32", "w8", "wf", "rf", "pls", "pads3", "area", "ltinfo", "join", "leave", "joinnow", "slottype", "view", "vp",
                                       "viewguard", "replace", "rec", "stoprec", "play", "stopplay", "pupinfo", "pset", "warp", NULL};
    for (int i = 0; list[i]; i++)
        if (!strcmp(n, list[i]))
            return 1;
    return 0;
}

static void run(const char *cmd, char *out, int size)
{
    char arg[300] = "";
    char name[32] = "";
    sscanf(cmd, "%31s %299[^\r\n]", name, arg);
    if (!strcmp(name, "L")) {   /* L <command>: run any console command on the game's logic thread */
        lt_post(arg, out, size);
        return;
    }
    if (is_s3(name)) {   /* these commands touch game state: always on the game thread */
        if (lt_on_game_thread())
            s3_run(cmd, out, size);
        else
            lt_post(cmd, out, size);
        return;
    }
    if (!strcmp(name, "ping")) {
        snprintf(out, size, "pong");
    } else if (!strcmp(name, "version")) {
        snprintf(out, size, "rev2-coop %s pid=%lu exe_ok=%d sha=%s", R2_VERSION, GetCurrentProcessId(), r2_exe_ok(), r2_exe_sha());
    } else if (!strcmp(name, "help")) {
        snprintf(out, size, "ping version base stub frames window iat threads stacks readmem <a> [n] readstr <a> [n] dword <a> dump <a> <len> <file> classname <a> dtiname <a> objscan <file> [filter] pad/padtap/padhold key/keydown/keyup/wmkey mouse mbtn cursor dinput pads steam diag shot log <t> quit menu ...");
    } else if (!strcmp(name, "base")) {
        BYTE *b = (BYTE *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
        int o = snprintf(out, size, "exe base %p size 0x%lx entry +0x%lx\n", b, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.AddressOfEntryPoint);
        IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
            o += snprintf(out + o, size - o, "  %-8.8s %p +0x%lx\n", (char *)s->Name, b + s->VirtualAddress, s->Misc.VirtualSize);
    } else if (!strcmp(name, "stub")) {
        char st[160];
        r2_text_state(st, sizeof st);
        snprintf(out, size, "%s", st);
    } else if (!strcmp(name, "frames")) {
        snprintf(out, size, "%u frames presented", d3d_frames());
    } else if (!strcmp(name, "window")) {
        HWND w = testmode_window();
        RECT r = {0}, c = {0};
        if (!w) {
            snprintf(out, size, "no game window known");
        } else {
            GetWindowRect(w, &r);
            GetClientRect(w, &c);
            snprintf(out, size, "window %p rect %ld,%ld %ldx%ld client %ldx%ld visible=%d iconic=%d foreground=%d style=%08lx",
                     (void *)w, r.left, r.top, r.right - r.left, r.bottom - r.top, c.right, c.bottom, IsWindowVisible(w), IsIconic(w),
                     GetForegroundWindow() == w, (unsigned long)GetWindowLongA(w, GWL_STYLE));
        }
    } else if (!strcmp(name, "pad") || !strcmp(name, "padtap")) {
        /* pad <slot> off | pad <slot> <hexbuttons> [lx ly rx ry lt rt]   padtap <slot> <hexbuttons> [ms=250]: press, then release
         * buttons: XInput bit mask (up 1 down 2 left 4 right 8 start 10 back 20 ls 40 rs 80 lb 100 rb 200 a 1000 b 2000 x 4000 y 8000) */
        int slot = -1;
        char b[16] = "0";
        int v[6] = {0, 0, 0, 0, 0, 0};
        if (sscanf(arg, "%d %15s %d %d %d %d %d %d", &slot, b, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) < 2 || slot < 0 || slot > 3) {
            snprintf(out, size, "usage: pad <slot 0-3> off|<hexbuttons> [lx ly rx ry lt rt] ; padtap <slot> <hexbuttons> [ms]");
        } else if (!strcmp(b, "off")) {
            testmode_pad_set(slot, 0, NULL);
            snprintf(out, size, "pad %d: real pad again", slot);
        } else {
            XINPUT_GAMEPAD g;
            memset(&g, 0, sizeof g);
            g.wButtons = (WORD)hex(b);
            if (!strcmp(name, "padtap")) {
                int ms = v[0] > 0 ? v[0] : 250;
                testmode_pad_set(slot, 1, &g);
                Sleep((DWORD)ms);
                memset(&g, 0, sizeof g);
                testmode_pad_set(slot, 1, &g);
                snprintf(out, size, "pad %d: %s pressed for %d ms", slot, b, ms);
            } else {
                g.sThumbLX = (SHORT)v[0], g.sThumbLY = (SHORT)v[1], g.sThumbRX = (SHORT)v[2], g.sThumbRY = (SHORT)v[3];
                g.bLeftTrigger = (BYTE)v[4], g.bRightTrigger = (BYTE)v[5];
                testmode_pad_set(slot, 1, &g);
                snprintf(out, size, "pad %d: buttons %04x set (until 'pad %d off')", slot, g.wButtons, slot);
            }
        }
    } else if (!strcmp(name, "padhold")) {
        /* padhold <slot> <hexbuttons> <ms> [lx ly rx ry lt rt]: set now, neutral (still connected) after ms; no blocking */
        int slot = -1, ms = 250;
        char b[16] = "0";
        int v[6] = {0, 0, 0, 0, 0, 0};
        if (sscanf(arg, "%d %15s %d %d %d %d %d %d %d", &slot, b, &ms, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) < 2 || slot < 0 || slot > 3) {
            snprintf(out, size, "usage: padhold <slot 0-3> <hexbuttons> <ms> [lx ly rx ry lt rt]");
        } else {
            XINPUT_GAMEPAD g;
            memset(&g, 0, sizeof g);
            g.wButtons = (WORD)hex(b);
            g.sThumbLX = (SHORT)v[0], g.sThumbLY = (SHORT)v[1], g.sThumbRX = (SHORT)v[2], g.sThumbRY = (SHORT)v[3];
            g.bLeftTrigger = (BYTE)v[4], g.bRightTrigger = (BYTE)v[5];
            testmode_pad_hold(slot, &g, ms);
            snprintf(out, size, "pad %d: buttons %04x for %d ms", slot, g.wButtons, ms);
        }
    } else if (!strcmp(name, "rpad")) {
        /* rpad <user 0-3> off | <hexbuttons> [lx ly rx ry lt rt]: pretend that THIS PC has a real pad (stage 5 test of the co-op pass-through: the glue maps it to the
         * local human's game user; `pad`/`padhold` instead replace a game user outright). Stays until `rpad <user> off`. */
        int user = -1;
        char b[16] = "0";
        int v[6] = {0, 0, 0, 0, 0, 0};
        if (sscanf(arg, "%d %15s %d %d %d %d %d %d", &user, b, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) < 2 || user < 0 || user > 3) {
            snprintf(out, size, "usage: rpad <user 0-3> off|<hexbuttons> [lx ly rx ry lt rt]");
        } else if (!strcmp(b, "off")) {
            testmode_pad_pretend(user, 0, NULL);
            snprintf(out, size, "rpad %d: off", user);
        } else {
            XINPUT_GAMEPAD g;
            memset(&g, 0, sizeof g);
            g.wButtons = (WORD)hex(b);
            g.sThumbLX = (SHORT)v[0], g.sThumbLY = (SHORT)v[1], g.sThumbRX = (SHORT)v[2], g.sThumbRY = (SHORT)v[3];
            g.bLeftTrigger = (BYTE)v[4], g.bRightTrigger = (BYTE)v[5];
            testmode_pad_pretend(user, 1, &g);
            snprintf(out, size, "rpad %d: buttons %04x stick %d,%d (a pretended real pad until 'rpad %d off')", user, g.wButtons, v[0], v[1], user);
        }
    } else if (!strcmp(name, "rpadhold")) {   /* combat4 LAB: rpadhold <user 0-3> <hexbuttons> <ms> [lx ly rx ry lt rt]: rpad for ms, then off by itself (one console round trip per press) */
        int user = -1, ms = 250;
        char b[16] = "0";
        int v[6] = {0, 0, 0, 0, 0, 0};
        if (sscanf(arg, "%d %15s %d %d %d %d %d %d %d", &user, b, &ms, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) < 3 || user < 0 || user > 3) {
            snprintf(out, size, "usage: rpadhold <user 0-3> <hexbuttons> <ms> [lx ly rx ry lt rt]");
        } else {
            XINPUT_GAMEPAD g;
            memset(&g, 0, sizeof g);
            g.wButtons = (WORD)hex(b);
            g.sThumbLX = (SHORT)v[0], g.sThumbLY = (SHORT)v[1], g.sThumbRX = (SHORT)v[2], g.sThumbRY = (SHORT)v[3];
            g.bLeftTrigger = (BYTE)v[4], g.bRightTrigger = (BYTE)v[5];
            testmode_pad_pretend_for(user, &g, (unsigned)(ms < 20 ? 20 : ms));
            snprintf(out, size, "rpad %d: buttons %04x lt %d rt %d for %d ms (a pretended real pad)", user, g.wButtons, v[4], v[5], ms);
        }
    } else if (!strcmp(name, "shake")) {   /* shake <user> <ms>: the pretended pad of that user shakes its left stick (+-30000 every 70 ms) for <ms>: the break-free of a grab (stage 6f); a console round trip to the N100 is too slow to do it by hand */
        int user = 0, ms = 3000, half = 150;
        sscanf(arg, "%d %d %d", &user, &ms, &half);
        shake_user = user < 0 || user > 3 ? 0 : user, shake_until = GetTickCount() + (unsigned)(ms < 100 ? 100 : ms), shake_t = 0, shake_half = (unsigned)(half < 30 ? 30 : half > 1000 ? 1000 : half);
        snprintf(out, size, "shake: the pretended pad %d shakes its left stick for %d ms (flip every %u ms)", shake_user, ms, shake_half);
    } else if (!strcmp(name, "key") || !strcmp(name, "keydown") || !strcmp(name, "keyup")) {
        /* key <name|0xDIK> [ms=150]: press and release by itself; keydown <name>: hold; keyup <name>: release */
        char k[32] = "";
        int ms = 150, dik, vk;
        sscanf(arg, "%31s %d", k, &ms);
        if (!inject_key_lookup(k, &dik, &vk)) {
            snprintf(out, size, "unknown key %s", k);
        } else if (!strcmp(name, "keyup")) {
            inject_key_up(dik);
            snprintf(out, size, "key %s (dik %02x) released", k, dik);
        } else {
            inject_key(dik, !strcmp(name, "keydown") ? 0 : ms);
            snprintf(out, size, "key %s (dik %02x) down%s", k, dik, !strcmp(name, "keydown") ? " until keyup" : "");
        }
    } else if (!strcmp(name, "wmkey")) {   /* wmkey <name> [ms]: WM_KEYDOWN / WM_KEYUP posted to the game window */
        char k[32] = "";
        int ms = 100, dik, vk;
        sscanf(arg, "%31s %d", k, &ms);
        HWND w = testmode_window();
        if (!w || !inject_key_lookup(k, &dik, &vk) || !vk) {
            snprintf(out, size, "no window or unknown key");
        } else {
            LPARAM lp = 1 | ((LPARAM)dik << 16);
            PostMessageA(w, WM_KEYDOWN, (WPARAM)vk, lp);
            Sleep((DWORD)ms);
            PostMessageA(w, WM_KEYUP, (WPARAM)vk, lp | 0xC0000000);
            snprintf(out, size, "WM_KEYDOWN/UP %s vk %02x", k, vk);
        }
    } else if (!strcmp(name, "mouse")) {   /* mouse <dx> <dy> [wheel]: relative movement delivered with the next reads */
        int dx = 0, dy = 0, wh = 0;
        sscanf(arg, "%d %d %d", &dx, &dy, &wh);
        inject_mouse_move(dx, dy, wh);
        snprintf(out, size, "mouse moved %d %d %d", dx, dy, wh);
    } else if (!strcmp(name, "mbtn")) {    /* mbtn <0 left|1 right|2 middle> [ms=120] */
        int n = 0, ms = 120;
        sscanf(arg, "%d %d", &n, &ms);
        inject_mouse_button(n, ms);
        snprintf(out, size, "mouse button %d down for %d ms", n, ms);
    } else if (!strcmp(name, "cursor")) {  /* cursor <x> <y> (client pixels, what GetCursorPos reports for the game) | cursor off */
        int x = 0, y = 0;
        if (!strncmp(arg, "off", 3)) {
            inject_cursor(0, 0, 0);
            snprintf(out, size, "fake cursor off");
        } else {
            sscanf(arg, "%d %d", &x, &y);
            inject_cursor(1, x, y);
            snprintf(out, size, "fake cursor at client %d,%d", x, y);
        }
    } else if (!strcmp(name, "trace")) {
        trace_install();
        snprintf(out, size, "trace hooks requested (see the log)");
    } else if (!strcmp(name, "gm")) {
        trace_gm_state(out, size);
    } else if (!strcmp(name, "setmode") || !strcmp(name, "setslot") || !strcmp(name, "setcoop")) {
        int a = 0, b = 1;
        sscanf(arg, "%d %d", &a, &b);
        trace_call(name, a, b, out, size);
    } else if (!strcmp(name, "dinput")) {
        testmode_dinput_stats(out, size);
    } else if (!strcmp(name, "steam")) {
        steam_stats(out, size);
    } else if (!strcmp(name, "net")) {
        extern int coop_net_command(const char *, char *, int);   /* coop3.c / coop_game.inc (stage 4): co-op session; bare `net` = capture counters */
        if (!coop_net_command(arg, out, size)) net_stats(out, size);
    } else if (!strcmp(name, "menu")) {   /* menu status|open|view|ev|text|start|set ...: the co-op lobby screen (stage 8, menu.c) */
        menu_command(arg, out, size);
    } else if (!strcmp(name, "netmark")) {
        net_mark(arg);
        snprintf(out, size, net_enabled() ? "marked" : "net capture is off (REV2_NETCAP)");
    } else if (!strcmp(name, "dump")) {   /* dump <hexaddr> <hexlen> <file>: raw bytes of readable memory (unreadable pages as zeros) */
        char a[32], l[32], f[260];
        if (sscanf(arg, "%31s %31s %259[^\r\n]", a, l, f) != 3) {
            snprintf(out, size, "usage: dump <hexaddr> <hexlen> <file>");
        } else {
            unsigned long addr = hex(a), len = hex(l), done = 0, bad = 0;
            FILE *fp = fopen(f, "wb");
            static unsigned char buf[65536];
            if (!fp) {
                snprintf(out, size, "cannot open %s", f);
            } else {
                while (done < len) {
                    unsigned long k = len - done < sizeof buf ? len - done : sizeof buf;
                    if (!r2_safe_read((void *)(addr + done), buf, k)) {
                        for (unsigned long q = 0; q < k; q += 4096) {
                            unsigned long kk = k - q < 4096 ? k - q : 4096;
                            if (!r2_safe_read((void *)(addr + done + q), buf + q, kk))
                                memset(buf + q, 0, kk), bad += kk;
                        }
                    }
                    fwrite(buf, 1, k, fp);
                    done += k;
                }
                fclose(fp);
                snprintf(out, size, "dumped %lu bytes from %lx to %s (%lu unreadable)", done, addr, f, bad);
            }
        }
    } else if (!strcmp(name, "diag")) {
        diag_report(out, size);
    } else if (!strcmp(name, "pads")) {
        testmode_xinput_stats(out, size);
    } else if (!strcmp(name, "post")) {   /* post <hexmsg> [hexwparam] [hexlparam] to the game window (lab: what wakes a waiting start) */
        char m[16] = "0", wp[16] = "0", lp[16] = "0";
        sscanf(arg, "%15s %15s %15s", m, wp, lp);
        HWND w = testmode_window();
        if (w)
            PostMessageA(w, (UINT)hex(m), (WPARAM)hex(wp), (LPARAM)hex(lp));
        snprintf(out, size, w ? "posted %s" : "no game window known", m);
    } else if (!strcmp(name, "mainstack")) {
        HWND w = testmode_window();
        DWORD tid = w ? GetWindowThreadProcessId(w, NULL) : 0;
        if (tid)
            thread_stack(tid, out, size, 1);
        else
            snprintf(out, size, "no game window known");
    } else if (!strcmp(name, "threads")) {
        all_threads(out, size, 0);
    } else if (!strcmp(name, "stacks")) {
        all_threads(out, size, 1);
    } else if (!strcmp(name, "iat")) {
        r2_iat_check("console");
        snprintf(out, size, "see the log");
    } else if (!strcmp(name, "readmem")) {
        char a[32];
        int n = 16;
        sscanf(arg, "%31s %d", a, &n);
        if (n < 1 || n > 256)
            n = 16;
        unsigned char buf[256];
        if (r2_safe_read((void *)hex(a), buf, (unsigned)n))
            hexdump(buf, n, out, size);
        else
            snprintf(out, size, "unreadable %s", a);
    } else if (!strcmp(name, "dword")) {
        DWORD v;
        if (r2_safe_read((void *)hex(arg), &v, 4))
            snprintf(out, size, "%08lx", v);
        else
            snprintf(out, size, "unreadable %s", arg);
    } else if (!strcmp(name, "readstr")) {
        char a[32];
        int n = 100;
        sscanf(arg, "%31s %d", a, &n);
        if (n < 1 || n > 200)
            n = 100;
        char buf[201];
        int i = 0;
        for (; i < n; i++) {
            if (!r2_safe_read((char *)hex(a) + i, &buf[i], 1))
                break;
            if (!buf[i])
                break;
            if ((unsigned char)buf[i] < 32 || (unsigned char)buf[i] > 126)
                buf[i] = '.';
        }
        buf[i] = 0;
        snprintf(out, size, i ? "\"%s\"" : "unreadable or empty %s", i ? buf : a);
    } else if (!strcmp(name, "classname")) {   /* classname <hexaddr>: object -> vtable[4] -> DTI -> name : parents */
        classes_name((unsigned)hex(arg), out, size);
    } else if (!strcmp(name, "dtiname")) {
        classes_dtiname((unsigned)hex(arg), out, size);
    } else if (!strcmp(name, "objscan")) {   /* objscan <name filter or -> <file (rest of the line, may contain spaces)> */
        char flt[80] = "", f[260] = "", lo[16] = "0", hi[16] = "0";
        if (sscanf(arg, "%79s %15s %15s %259[^\r\n]", flt, lo, hi, f) < 4)
            snprintf(out, size, "usage: objscan <filter|-> <hexlo> <hexhi> <file> (0 0 = everything; a big range crashed the game, see stage2a.md)");
        else
            classes_scan(f, !strcmp(flt, "-") ? "" : flt, (unsigned)hex(lo), (unsigned)hex(hi), out, size);
    } else if (!strcmp(name, "shot")) {
        int bn = 0, bms = 150;
        if (sscanf(arg, "%d %d", &bn, &bms) >= 1 && bn > 1) {   /* shot <n> [ms]: a burst taken inside the Present hook */
            d3d_request_burst(bn, bms);
            snprintf(out, size, "burst of %d shots, one per %d ms, requested", bn, bms);
        } else {
            d3d_request_shot();
            snprintf(out, size, "shot requested (next presented frame)");
        }
    } else if (!strcmp(name, "devreset")) {   /* devreset <n>: the next Present resets with n lost answers (the wait in d3d.c hook_Reset) */
        int dn = atoi(arg);
        if (dn < 1 || dn > 50)
            snprintf(out, size, "usage: devreset <1-50>");
        else {
            d3d_devreset_request(dn);
            snprintf(out, size, "devreset %d requested: the next presented frame resets the device with %d lost answers", dn, dn);
        }
    } else if (!strcmp(name, "log")) {
        r2_log("console: %s", arg);
        snprintf(out, size, "logged");
    } else if (!strcmp(name, "quit")) {
        HWND w = testmode_window();
        if (w)
            PostMessageA(w, WM_CLOSE, 0, 0);
        snprintf(out, size, w ? "WM_CLOSE posted" : "no game window known");
    } else {
        snprintf(out, size, "unknown command: %s (help)", name);
    }
}

static char port_text[16];

/* The socket is set up on the thread itself: nothing but CreateThread runs under the loader lock. */
static DWORD WINAPI console_thread(LPVOID arg)
{
    (void)arg;
    char buf[512], out[8000];
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(port_text));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        r2_log("console: cannot listen on 127.0.0.1:%s", port_text);
        return 0;
    }
    r2_log("console: listening on 127.0.0.1:%s", port_text);
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            if (WSAGetLastError() == WSAECONNRESET)   /* (an ICMP port-unreachable for an earlier reply) */
                continue;
            return 0;
        }
        buf[n] = 0;
        out[0] = 0;
        run(buf, out, sizeof out);
        sendto(s, out, (int)strlen(out), 0, (struct sockaddr *)&from, fl);
    }
}

void console_install(void)
{
    if (r2_env("REV2_CONSOLE", port_text, sizeof port_text))
        CreateThread(NULL, 0, console_thread, NULL, 0, NULL);
}

/* called every game frame (coop3.c frame_callbacks) */
void console_shake_tick(void)
{
    if (!shake_until) return;
    unsigned now = GetTickCount();
    if ((int)(now - shake_until) >= 0) {
        shake_until = 0;
        testmode_pad_pretend(shake_user, 0, NULL);
        return;
    }
    if (now - shake_t >= shake_half) {
        XINPUT_GAMEPAD g;
        memset(&g, 0, sizeof g);
        shake_t = now, shake_dir = !shake_dir;
        g.sThumbLX = (SHORT)(shake_dir ? 30000 : -30000);
        testmode_pad_pretend(shake_user, 1, &g);
    }
}
