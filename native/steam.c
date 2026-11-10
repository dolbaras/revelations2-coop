/*
 * The game's own Steam calls (import table of steam_api.dll plus the interface vtables behind it). Logging only: every hook
 * calls the original with the original arguments and returns its result unchanged. Nothing is faked and nothing is stored by
 * the mod.
 *
 * Test safety (stage 8b, docs\achievements.md): with the block switch on (REV2_ACHBLOCK=1, and the DEFAULT whenever the test console is
 * enabled, REV2_CONSOLE set, which every lab script does) SetAchievement / ClearAchievement / SetStat / UpdateAvgRateStat / StoreStats /
 * IndicateAchievementProgress / ResetAllStats are logged and NOT passed to Steam: the accounts of the testers never receive an unlock from a
 * teleport or cheat run. The call returns success, so the game's own bookkeeping continues exactly as after a real unlock, and every blocked
 * unlock is listed as "WOULD UNLOCK" (log and console `net w ach`). A player's normal start (no console) passes everything through; the manual
 * switch to verify a real unlock in a console run is REV2_ACHBLOCK=0.
 *
 *   - SteamAPI_Init / SteamAPI_RunCallbacks timing (console "steam")
 *   - SteamAPI_RegisterCallback / RegisterCallResult: which callbacks the game waits for, and every time one of them runs
 *     (the Run slots of the game's callback objects are wrapped; the slot that takes (param) and the one that takes
 *     (param, failure, call handle) are told apart by their `ret` immediate)
 *   - SteamInternal_CreateInterface("SteamClientNNN") -> ISteamClient::GetISteamUserStats -> the ISteamUserStats vtable:
 *     RequestCurrentStats, GetStat, SetStat, UpdateAvgRateStat, GetAchievement, SetAchievement, ClearAchievement,
 *     GetAchievementAndUnlockTime, StoreStats, IndicateAchievementProgress, RequestUserStats, ResetAllStats
 *     with names and values, in the mod's log as "ach: ..." lines (notes\achievements.md)
 *
 * Slot numbers are those of Valve's interface headers (SDK 1.3x, 2015: ISteamClient::GetISteamUserStats = 13;
 * ISteamUserStats = RequestCurrentStats 0, GetStat 1-2, SetStat 3-4, UpdateAvgRateStat 5, GetAchievement 6, SetAchievement 7,
 * ClearAchievement 8, GetAchievementAndUnlockTime 9, StoreStats 10, ..., IndicateAchievementProgress 13, GetNumAchievements 14,
 * GetAchievementName 15, RequestUserStats 16, ..., ResetAllStats 21). The overloaded GetStat/SetStat pairs may be swapped by the
 * compiler that built steamclient: the log prints both readings of a value.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "r2.h"
#include "coop_steam.h"
#include "ach_logic.h"
#include "coopsave.h"

#define THISCALL __attribute__((thiscall))

extern int coop_net_command(const char *arg, char *out, int size);   /* coop3.c / coop_game.inc: the console entry of the co-op glue */

typedef int (__cdecl *SteamAPI_Init_t)(void);
typedef void (__cdecl *SteamAPI_RunCallbacks_t)(void);
typedef void (__cdecl *RegisterCallback_t)(void *cb, int id);
typedef void (__cdecl *UnregisterCallback_t)(void *cb);
typedef void (__cdecl *RegisterCallResult_t)(void *cb, unsigned long long call);
typedef void (__cdecl *UnregisterCallResult_t)(void *cb, unsigned long long call);
typedef void *(__cdecl *CreateInterface_t)(const char *ver);
typedef void *(__cdecl *ContextInit_t)(void *data);
static SteamAPI_Init_t real_Init;
static SteamAPI_RunCallbacks_t real_RunCallbacks;
static RegisterCallback_t real_RegisterCallback;
static UnregisterCallback_t real_UnregisterCallback;
static RegisterCallResult_t real_RegisterCallResult;
static UnregisterCallResult_t real_UnregisterCallResult;
static CreateInterface_t real_CreateInterface;
static ContextInit_t real_ContextInit;
static volatile LONG rc_calls, rc_slow, rc_max_ms;
static volatile LONG rc_total_ms;
static DWORD t_start;
static int log_stats;
static int ach_block;                   /* see the header: SetAchievement and the other writers are not passed to Steam */
static volatile LONG ach_blocked_n;     /* writer calls swallowed so far */
static char ach_would[48][40];          /* block mode: names a real run would have unlocked (SetAchievement), unique, in order */
static DWORD ach_would_ms[48];
static int ach_would_n;
static char ach_done[48][40];           /* real mode: unlocks that were passed to Steam and accepted */
static int ach_done_n;

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

static const char *who(const void *addr, char *buf, int size)
{
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, '\\');
        snprintf(buf, size, "%s+%lx", b ? b + 1 : name, (unsigned long)((const char *)addr - (const char *)m));
    } else
        snprintf(buf, size, "%p", addr);
    return buf;
}

static int __cdecl hook_Init(void)
{
    double t = now_ms();
    int r = real_Init();
    r2_log("steam: SteamAPI_Init -> %d in %.0f ms", r, now_ms() - t);
    return r;
}

static void __cdecl hook_RunCallbacks(void)
{
    cst_game_tick();   /* co-op Steam transport on the game thread: registers its callbacks, pumps its events, starts an invited join */
    double t = now_ms();
    real_RunCallbacks();
    LONG ms = (LONG)(now_ms() - t);
    InterlockedIncrement(&rc_calls);
    InterlockedAdd(&rc_total_ms, ms);
    if (ms > rc_max_ms)
        rc_max_ms = ms;
    if (ms >= 100 && InterlockedIncrement(&rc_slow) <= 30)
        r2_log("steam: SteamAPI_RunCallbacks took %ld ms (call %ld)", ms, rc_calls);
}

void steam_stats(char *out, int size)
{
    snprintf(out, size, "steam: %ld RunCallbacks calls, %ld slow (>=100 ms), longest %ld ms, %ld ms in total", rc_calls, rc_slow, rc_max_ms, rc_total_ms);
}

/* console `net w ach`: block mode and the unlocks that would (block mode) or did (real mode) happen */
static void ach_note(char list[][40], int *n, const char *name)
{
    if (!name) return;
    for (int i = 0; i < *n; i++)
        if (!strncmp(list[i], name, 39)) return;
    if (*n >= 48) return;
    snprintf(list[*n], 40, "%s", name);
    if (list == ach_would) ach_would_ms[*n] = GetTickCount() - t_start;
    (*n)++;
}

int steam_ach_blocking(void)
{
    return ach_block;
}

void steam_ach_status(char *out, int size)
{
    int o = snprintf(out, size, "steam achievements: %s (REV2_ACHBLOCK), %ld writer call(s) blocked\n", ach_block ? "BLOCKED, nothing reaches Steam" : "REAL, calls reach Steam", (long)ach_blocked_n);
    for (int i = 0; i < ach_would_n && o < size - 80; i++)
        o += snprintf(out + o, size - o, "  WOULD UNLOCK %s (+%lu ms)\n", ach_would[i], (unsigned long)ach_would_ms[i]);
    for (int i = 0; i < ach_done_n && o < size - 80; i++)
        o += snprintf(out + o, size - o, "  UNLOCKED %s\n", ach_done[i]);
    if (!ach_would_n && !ach_done_n) snprintf(out + o, size - o, "  (no unlock so far)\n");
}

/* ---- callbacks the game registers, and when they run ---- */
typedef void (THISCALL *Run1_t)(void *self, void *pv);
typedef void (THISCALL *Run3_t)(void *self, void *pv, int fail, unsigned long long call);
static struct { void **vtbl; void *orig[2]; int kind[2]; int count; int size; } cbv[160];   /* kind: 1 = Run(pv), 3 = Run(pv, fail, call) */
static int n_cbv;
static volatile LONG n_runs;

static int ret_kind(const unsigned char *fn)
{
    unsigned char c[96];
    if (!r2_safe_read(fn, c, sizeof c))
        return 0;
    for (int i = 0; i + 2 < (int)sizeof c; i++)
        if (c[i] == 0xC2 && c[i + 2] == 0) {
            if (c[i + 1] == 0x04)
                return 1;
            if (c[i + 1] == 0x10)
                return 3;
        }
    return 0;
}

static int cb_find(void *self)
{
    void **vt = *(void ***)self;
    for (int i = 0; i < n_cbv; i++)
        if (cbv[i].vtbl == vt)
            return i;
    return -1;
}

static void THISCALL hook_run1(void *self, void *pv)
{
    int i = cb_find(self);
    if (i < 0)
        return;
    int id = *(int *)((char *)self + 8);
    int slot = cbv[i].kind[0] == 1 ? 0 : 1;
    cbv[i].count++;
    if (InterlockedIncrement(&n_runs) <= 200) {
        char b[80];
        r2_log("steam: callback %d runs (Run(param), handler vtable %s, +%lu ms)", id, who(cbv[i].vtbl, b, sizeof b), GetTickCount() - t_start);
    }
    net_on_callback(id, self, pv, cbv[i].size, 0, 0);
    if (cst_swallow_game_callback(id, pv)) {   /* a callback about OUR lobby (coop_steam.c): the game's Raid handler never gets it */
        r2_log("steam: callback %d about our lobby kept from the game's handler", id);
        return;
    }
    ((Run1_t)cbv[i].orig[slot])(self, pv);
}

static void THISCALL hook_run3(void *self, void *pv, int fail, unsigned long long call)
{
    int i = cb_find(self);
    if (i < 0)
        return;
    int id = *(int *)((char *)self + 8);
    int slot = cbv[i].kind[0] == 3 ? 0 : 1;
    cbv[i].count++;
    if (InterlockedIncrement(&n_runs) <= 200) {
        char b[80];
        r2_log("steam: callback %d runs (Run(param, failure=%d, call=%llx), handler vtable %s, +%lu ms)", id, fail, call, who(cbv[i].vtbl, b, sizeof b),
               GetTickCount() - t_start);
    }
    net_on_callback(id, self, pv, cbv[i].size, fail, call);
    if (!call && cst_swallow_game_callback(id, pv)) {
        r2_log("steam: callback %d about our lobby kept from the game's handler", id);
        return;
    }
    ((Run3_t)cbv[i].orig[slot])(self, pv, fail, call);
}

static void wrap_callback(void *cb, int size)
{
    if (!cb || cb_find(cb) >= 0 || n_cbv >= 160)
        return;
    void **vt = *(void ***)cb;
    int k0 = ret_kind((unsigned char *)vt[0]), k1 = ret_kind((unsigned char *)vt[1]);
    if (k0 && !k1)
        k1 = k0 == 1 ? 3 : 1;   /* a callback object has exactly Run(param) and Run(param, failure, call) in slots 0 and 1 */
    else if (k1 && !k0)
        k0 = k1 == 1 ? 3 : 1;
    int i = n_cbv++;
    cbv[i].vtbl = vt;
    cbv[i].orig[0] = vt[0], cbv[i].orig[1] = vt[1];
    cbv[i].kind[0] = k0, cbv[i].kind[1] = k1, cbv[i].size = size;
    char b[80];
    r2_log("steam: handler vtable %s Run slots: [0]=%s [1]=%s", who(vt, b, sizeof b), k0 == 1 ? "Run(param)" : k0 == 3 ? "Run(param,failure,call)" : "?",
           k1 == 1 ? "Run(param)" : k1 == 3 ? "Run(param,failure,call)" : "?");
    if (!((k0 == 1 && k1 == 3) || (k0 == 3 && k1 == 1)))
        return;
    cst_note_callback_layout(k0);   /* our own callback objects (coop_steam_real.c) use the same slot order */
    DWORD old;
    if (!VirtualProtect(&vt[0], 2 * sizeof(void *), PAGE_READWRITE, &old))
        return;
    vt[0] = k0 == 1 ? (void *)hook_run1 : (void *)hook_run3;
    vt[1] = k1 == 1 ? (void *)hook_run1 : (void *)hook_run3;
    VirtualProtect(&vt[0], 2 * sizeof(void *), old, &old);
}

static void __cdecl hook_RegisterCallback(void *cb, int id)
{
    char b[80], c[80];
    unsigned char sz[8];
    void **vt = cb ? *(void ***)cb : NULL;
    int size = -1;
    if (vt && r2_safe_read(vt[2], sz, 6) && sz[0] == 0xB8)
        size = *(int *)(sz + 1);
    r2_log("steam: RegisterCallback id=%d (callback struct %d bytes) object %p vtable %s caller %s (+%lu ms)", id, size, cb,
           vt ? who(vt, b, sizeof b) : "?", who(__builtin_return_address(0), c, sizeof c), GetTickCount() - t_start);
    real_RegisterCallback(cb, id);
    wrap_callback(cb, size);
}

static void __cdecl hook_UnregisterCallback(void *cb)
{
    real_UnregisterCallback(cb);
}

static void __cdecl hook_RegisterCallResult(void *cb, unsigned long long call)
{
    char b[80], c[80];
    void **vt = cb ? *(void ***)cb : NULL;
    unsigned char sz[8];
    int size = -1;
    if (vt && r2_safe_read(vt[2], sz, 6) && sz[0] == 0xB8)
        size = *(int *)(sz + 1);
    r2_log("steam: RegisterCallResult call=%llx object %p vtable %s caller %s (+%lu ms)", call, cb, vt ? who(vt, b, sizeof b) : "?",
           who(__builtin_return_address(0), c, sizeof c), GetTickCount() - t_start);
    real_RegisterCallResult(cb, call);
    wrap_callback(cb, size);
}

static void __cdecl hook_UnregisterCallResult(void *cb, unsigned long long call)
{
    real_UnregisterCallResult(cb, call);
}

/* ---- ISteamUserStats ---- */
static int caller_is_exe(const void *ret)
{
    HMODULE exe = GetModuleHandleA(NULL);
    return ret >= (const void *)exe && ret < (const void *)((char *)exe + 0x1300000);
}

static void *stat_orig[32];
static void **stats_vtbl;

#define CALLER_TAG(r) (caller_is_exe(r) ? "exe" : "other")

static char caller_buf[8][80];
static const char *caller_str(const void *ret, int k)
{
    return who(ret, caller_buf[k & 7], sizeof caller_buf[0]);
}

static int THISCALL s_RequestCurrentStats(void *self)
{
    const void *ret = __builtin_return_address(0);
    int r = ((int (THISCALL *)(void *))stat_orig[0])(self);
    r2_log("ach: RequestCurrentStats() -> %d (caller %s, +%lu ms)", r & 0xff, caller_str(ret, 0), GetTickCount() - t_start);
    return r;
}

#define GETSTAT(N) \
    static int THISCALL s_GetStat##N(void *self, const char *name, void *out) \
    { \
        const void *ret = __builtin_return_address(0); \
        int r = ((int (THISCALL *)(void *, const char *, void *))stat_orig[N])(self, name, out); \
        unsigned v = 0; \
        r2_safe_read(out, &v, 4); \
        float f; memcpy(&f, &v, 4); \
        r2_log("ach: GetStat[%d](\"%s\") -> %d value int=%d float=%g (caller %s)", N, name ? name : "(null)", r & 0xff, (int)v, (double)f, caller_str(ret, N)); \
        return r; \
    }
GETSTAT(1)
GETSTAT(2)

#define SETSTAT(N) \
    static int THISCALL s_SetStat##N(void *self, const char *name, unsigned v) \
    { \
        const void *ret = __builtin_return_address(0); \
        float f; memcpy(&f, &v, 4); \
        if (ach_block) { InterlockedIncrement(&ach_blocked_n); r2_log("ach: SetStat[%d](\"%s\") BLOCKED (REV2_ACHBLOCK)", N, name ? name : "(null)"); return 1; } \
        int r = ((int (THISCALL *)(void *, const char *, unsigned))stat_orig[N])(self, name, v); \
        r2_log("ach: SetStat[%d](\"%s\", int=%d float=%g) -> %d (caller %s, +%lu ms)", N, name ? name : "(null)", (int)v, (double)f, r & 0xff, caller_str(ret, N), GetTickCount() - t_start); \
        return r; \
    }
SETSTAT(3)
SETSTAT(4)

static int THISCALL s_UpdateAvgRateStat(void *self, const char *name, float count, double length)
{
    const void *ret = __builtin_return_address(0);
    if (ach_block) { InterlockedIncrement(&ach_blocked_n); r2_log("ach: UpdateAvgRateStat(\"%s\") BLOCKED (REV2_ACHBLOCK)", name ? name : "(null)"); return 1; }
    int r = ((int (THISCALL *)(void *, const char *, float, double))stat_orig[5])(self, name, count, length);
    r2_log("ach: UpdateAvgRateStat(\"%s\", %g, %g) -> %d (caller %s)", name ? name : "(null)", (double)count, length, r & 0xff, caller_str(ret, 5));
    return r;
}

static int THISCALL s_GetAchievement(void *self, const char *name, unsigned char *got)
{
    const void *ret = __builtin_return_address(0);
    int r = ((int (THISCALL *)(void *, const char *, unsigned char *))stat_orig[6])(self, name, got);
    unsigned char g = 0xff;
    r2_safe_read(got, &g, 1);
    r2_log("ach: GetAchievement(\"%s\") -> %d achieved=%d (caller %s)", name ? name : "(null)", r & 0xff, g, caller_str(ret, 6));
    return r;
}

static int THISCALL s_SetAchievement(void *self, const char *name)
{
    const void *ret = __builtin_return_address(0);
    if (ach_block) {
        InterlockedIncrement(&ach_blocked_n);
        ach_note(ach_would, &ach_would_n, name);
        r2_log("ach: SetAchievement(\"%s\") BLOCKED (REV2_ACHBLOCK): WOULD UNLOCK, not passed to Steam (caller %s, +%lu ms)", name ? name : "(null)", caller_str(ret, 7), GetTickCount() - t_start);
        return 1;   /* success: the game's own bookkeeping goes on as after a real unlock */
    }
    int r = ((int (THISCALL *)(void *, const char *))stat_orig[7])(self, name);
    if (r & 0xff) ach_note(ach_done, &ach_done_n, name);
    r2_log("ach: SetAchievement(\"%s\") -> %d (caller %s, +%lu ms)", name ? name : "(null)", r & 0xff, caller_str(ret, 7), GetTickCount() - t_start);
    return r;
}

static int THISCALL s_ClearAchievement(void *self, const char *name)
{
    const void *ret = __builtin_return_address(0);
    if (ach_block) { InterlockedIncrement(&ach_blocked_n); r2_log("ach: ClearAchievement(\"%s\") BLOCKED (REV2_ACHBLOCK)", name ? name : "(null)"); return 1; }
    int r = ((int (THISCALL *)(void *, const char *))stat_orig[8])(self, name);
    r2_log("ach: ClearAchievement(\"%s\") -> %d (caller %s)", name ? name : "(null)", r & 0xff, caller_str(ret, 8));
    return r;
}

static int THISCALL s_GetAchievementAndUnlockTime(void *self, const char *name, unsigned char *got, unsigned *when)
{
    const void *ret = __builtin_return_address(0);
    int r = ((int (THISCALL *)(void *, const char *, unsigned char *, unsigned *))stat_orig[9])(self, name, got, when);
    unsigned char g = 0xff;
    unsigned w = 0;
    r2_safe_read(got, &g, 1);
    r2_safe_read(when, &w, 4);
    r2_log("ach: GetAchievementAndUnlockTime(\"%s\") -> %d achieved=%d time=%u (caller %s)", name ? name : "(null)", r & 0xff, g, w, caller_str(ret, 9));
    return r;
}

static int THISCALL s_StoreStats(void *self)
{
    const void *ret = __builtin_return_address(0);
    if (ach_block) { InterlockedIncrement(&ach_blocked_n); r2_log("ach: StoreStats() BLOCKED (REV2_ACHBLOCK) (caller %s)", caller_str(ret, 10)); return 1; }
    int r = ((int (THISCALL *)(void *))stat_orig[10])(self);
    r2_log("ach: StoreStats() -> %d (caller %s, +%lu ms)", r & 0xff, caller_str(ret, 10), GetTickCount() - t_start);
    return r;
}

static int THISCALL s_IndicateAchievementProgress(void *self, const char *name, unsigned cur, unsigned max)
{
    const void *ret = __builtin_return_address(0);
    if (ach_block) { InterlockedIncrement(&ach_blocked_n); r2_log("ach: IndicateAchievementProgress(\"%s\") BLOCKED (REV2_ACHBLOCK)", name ? name : "(null)"); return 1; }
    int r = ((int (THISCALL *)(void *, const char *, unsigned, unsigned))stat_orig[13])(self, name, cur, max);
    r2_log("ach: IndicateAchievementProgress(\"%s\", %u, %u) -> %d (caller %s)", name ? name : "(null)", cur, max, r & 0xff, caller_str(ret, 13));
    return r;
}

static unsigned long long THISCALL s_RequestUserStats(void *self, unsigned lo, unsigned hi)
{
    const void *ret = __builtin_return_address(0);
    unsigned long long r = ((unsigned long long (THISCALL *)(void *, unsigned, unsigned))stat_orig[16])(self, lo, hi);
    r2_log("ach: RequestUserStats(steamid %08x%08x) -> call %llx (caller %s)", hi, lo, r, caller_str(ret, 16));
    return r;
}

static int THISCALL s_ResetAllStats(void *self, int ach)
{
    const void *ret = __builtin_return_address(0);
    if (ach_block) { InterlockedIncrement(&ach_blocked_n); r2_log("ach: ResetAllStats(achievements too=%d) BLOCKED (REV2_ACHBLOCK)", ach & 0xff); return 1; }
    int r = ((int (THISCALL *)(void *, int))stat_orig[21])(self, ach);
    r2_log("ach: ResetAllStats(achievements too=%d) -> %d (caller %s)", ach & 0xff, r & 0xff, caller_str(ret, 21));
    return r;
}

static void wrap_stats(void *iface, const char *ver)
{
    void **vt = *(void ***)iface;
    if (vt == stats_vtbl)
        return;
    stats_vtbl = vt;
    static const struct { int slot; void *fn; } map[] = {
        {0, (void *)s_RequestCurrentStats}, {1, (void *)s_GetStat1}, {2, (void *)s_GetStat2}, {3, (void *)s_SetStat3}, {4, (void *)s_SetStat4},
        {5, (void *)s_UpdateAvgRateStat}, {6, (void *)s_GetAchievement}, {7, (void *)s_SetAchievement}, {8, (void *)s_ClearAchievement},
        {9, (void *)s_GetAchievementAndUnlockTime}, {10, (void *)s_StoreStats}, {13, (void *)s_IndicateAchievementProgress},
        {16, (void *)s_RequestUserStats}, {21, (void *)s_ResetAllStats}};
    r2_log("ach: ISteamUserStats %s at %p, vtable %p: wrapping %d slots (logging only)", ver, iface, (void *)vt, (int)(sizeof map / sizeof map[0]));
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        DWORD old;
        if (!VirtualProtect(&vt[map[i].slot], sizeof(void *), PAGE_READWRITE, &old))
            continue;
        stat_orig[map[i].slot] = vt[map[i].slot];
        vt[map[i].slot] = map[i].fn;
        VirtualProtect(&vt[map[i].slot], sizeof(void *), old, &old);
    }
}

/* ISteamClient: GetISteamUserStats (13) and GetISteamGenericInterface (12) */
typedef void *(THISCALL *GetIface_t)(void *self, int hUser, int hPipe, const char *ver);
static GetIface_t orig_GetUserStats, orig_GetGeneric;

static void *THISCALL hook_GetISteamUserStats(void *self, int hUser, int hPipe, const char *ver)
{
    void *p = orig_GetUserStats(self, hUser, hPipe, ver);
    r2_log("steam: ISteamClient::GetISteamUserStats(\"%s\") -> %p", ver ? ver : "(null)", p);
    if (p && log_stats)
        wrap_stats(p, ver ? ver : "?");
    return p;
}

static void *THISCALL hook_GetISteamGenericInterface(void *self, int hUser, int hPipe, const char *ver)
{
    void *p = orig_GetGeneric(self, hUser, hPipe, ver);
    r2_log("steam: ISteamClient::GetISteamGenericInterface(\"%s\") -> %p", ver ? ver : "(null)", p);
    if (p && log_stats && ver && strncmp(ver, "STEAMUSERSTATS_INTERFACE_VERSION", 32) == 0)
        wrap_stats(p, ver);
    else if (p && ver)
        net_on_iface(p, ver);
    return p;
}

static void *__cdecl hook_CreateInterface(const char *ver)
{
    void *p = real_CreateInterface(ver);
    char c[80];
    r2_log("steam: SteamInternal_CreateInterface(\"%s\") -> %p caller %s", ver ? ver : "(null)", p, who(__builtin_return_address(0), c, sizeof c));
    if (p && ver && strncmp(ver, "SteamClient", 11) == 0) {
        void **vt = *(void ***)p;
        static void **done;
        if (vt != done) {
            DWORD old;
            if (VirtualProtect(&vt[12], 2 * sizeof(void *), PAGE_READWRITE, &old)) {
                orig_GetGeneric = (GetIface_t)vt[12];
                orig_GetUserStats = (GetIface_t)vt[13];
                vt[12] = (void *)hook_GetISteamGenericInterface;
                vt[13] = (void *)hook_GetISteamUserStats;
                VirtualProtect(&vt[12], 2 * sizeof(void *), old, &old);
                done = vt;
                net_on_client(p);
            }
        }
    }
    return p;
}

static void *__cdecl hook_ContextInit(void *data)
{
    void *r = real_ContextInit(data);
    if (r) cst_note_context(r);   /* the CSteamAPIContext of the game: the co-op transport uses the interfaces the game itself holds */
    if (r) coopsave_on_context(r);   /* 0.2.1: the save calls of a co-op game go to the co-op save (coopsave.c) */
    static int n;
    if (++n <= 4) {
        unsigned d[8];
        r2_safe_read(r, d, sizeof d);
        r2_log("steam: SteamInternal_ContextInit(%p) -> %p [%08x %08x %08x %08x %08x %08x %08x %08x]", data, r, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    }
    return r;
}

void steam_install(void)
{
    char v[8];
    t_start = GetTickCount();
    cst_set_runner(coop_net_command);
    log_stats = !(r2_env("REV2_NOACH", v, sizeof v) && v[0] == '1');
    /* test safety: blocked when asked (REV2_ACHBLOCK=1), and by default whenever the test console is on (every lab script sets REV2_CONSOLE, a player never does);
     * REV2_ACHBLOCK=0 is the explicit "let a console run reach Steam" (to verify a real unlock by hand). */
    {
        char b[8], c[8];
        int has_b = r2_env("REV2_ACHBLOCK", b, sizeof b), has_c = r2_env("REV2_CONSOLE", c, sizeof c);
        ach_block = ach_block_default(has_b, b[0], has_c);
        if (ach_block) log_stats = 1;   /* the wrappers are what blocks: they must be installed whatever REV2_NOACH says */
        r2_log("ach: writer calls %s (REV2_ACHBLOCK=%s, test console %s)", ach_block ? "BLOCKED" : "pass through to Steam", has_b ? b : "unset", has_c ? "on" : "off");
    }
    real_Init = (SteamAPI_Init_t)r2_patch_iat("steam_api.dll", "SteamAPI_Init", (void *)hook_Init);
    real_RunCallbacks = (SteamAPI_RunCallbacks_t)r2_patch_iat("steam_api.dll", "SteamAPI_RunCallbacks", (void *)hook_RunCallbacks);
    real_RegisterCallback = (RegisterCallback_t)r2_patch_iat("steam_api.dll", "SteamAPI_RegisterCallback", (void *)hook_RegisterCallback);
    real_UnregisterCallback = (UnregisterCallback_t)r2_patch_iat("steam_api.dll", "SteamAPI_UnregisterCallback", (void *)hook_UnregisterCallback);
    real_RegisterCallResult = (RegisterCallResult_t)r2_patch_iat("steam_api.dll", "SteamAPI_RegisterCallResult", (void *)hook_RegisterCallResult);
    real_UnregisterCallResult = (UnregisterCallResult_t)r2_patch_iat("steam_api.dll", "SteamAPI_UnregisterCallResult", (void *)hook_UnregisterCallResult);
    real_CreateInterface = (CreateInterface_t)r2_patch_iat("steam_api.dll", "SteamInternal_CreateInterface", (void *)hook_CreateInterface);
    real_ContextInit = (ContextInit_t)r2_patch_iat("steam_api.dll", "SteamInternal_ContextInit", (void *)hook_ContextInit);
}
