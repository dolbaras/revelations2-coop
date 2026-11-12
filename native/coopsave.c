/*
 * The co-op save (0.2.1, notes\coopsave.md): a co-op game reads and writes its OWN save; the player's solo campaign save is never opened by it.
 *
 * Rev2 keeps its campaign save only through ISteamRemoteStorage ("DATA0.DAT": FileExists, GetFileSize, FileRead, FileWrite, FileDelete from sSavedata,
 * 0xC1D5C0 / 0xC1D750 / 0xC1D530); Steam stores it in userdata\<account>\287290\remote\ and Steam Cloud carries it. This file
 *   1. wraps those five slots of the game's ISteamRemoteStorage vtable (like steam.c wraps ISteamUserStats). While a co-op game runs (state ON), a call on
 *      the save name is served from the co-op file in the game's config folder (%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2\rev2_coop_save_<h>.dat,
 *      <h> = a hash of the Steam account, so two accounts of one Windows user keep two co-op saves); every other call, and every call while OFF, goes to
 *      Steam unchanged. Steam never sees the co-op file (no cloud slot, no quota, no conflict), and the solo DATA0.DAT is not touched during a co-op game.
 *   2. keeps the solo save image in memory: entering a co-op game copies the two story ranges the game saves (story +0x189A0 / 0x18978 and +0x31330 /
 *      0x10F020, FUN_00A9B640) and loads the co-op save through the game's own load and unpack (0xC1D5C0, 0xA9AF00); without a co-op save the co-op game
 *      starts from the solo image (settings, skills, unlocks). Back at the title menus after the co-op game, the copy goes back and the game's post-load
 *      steps run again (FUN_0097DFC0, FUN_00987D90, as after the boot load): the solo save in memory is the one from before, and the routing ends.
 *   3. writes the checkpoint record next to the co-op save at every co-op save write (coopsave_logic.h): the lobby's Continue reads it, the host sends it, the
 *      guest follows it.
 * The routing decision itself is cps_route (coopsave_logic.h, tested offline with a model of the Steam calls: tests/test_coopsave.c).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "r2.h"
#include "coop_proto.h"
#include "coopsave.h"
#include "coopsave_logic.h"

#define THISCALL __attribute__((thiscall))
#define CLOG(...) r2_log("coopsave: " __VA_ARGS__)
#define VT(o) (*(void ***)(o))

/* game addresses (build 3658995, notes\coopsave.md section 1) */
enum {
    CA_STORY = 0x157D120,       /* cell: the story block (cGameInfo at +0x31320) */
    CA_GAMEMAN = 0x157AE00,     /* cell: GameMan (+0x20 next state, +0x24, +0x30, +0x5C game type) */
    CA_SAVEMAN = 0x15810EC,     /* cell: sSaveManager (+0x20 mode: 0 idle, 1 load, 2 save) */
    CA_SAVEDATA = 0x15DE790,    /* cell: sSavedata (+0x24 busy, +0x28 result, +0xD40 mutex, +0xD44 storage thread op) */
    CA_STORAGE = 0x1581950,     /* cell: sStorage (staging buffer of the save) */
    CF_LOAD = 0xC1D5C0,         /* thiscall sSavedata: FileExists / GetFileSize / FileRead + parse */
    CF_UNPACK = 0xA9AF00,       /* thiscall sStorage: decrypt, check, copy into the story ranges + post-load steps; al = 1 ok */
    CF_STORAGE_USER = 0xAA8000, /* thiscall sStorage, u8: called with 0 before the unpack (a79db0) */
    CF_POST1 = 0x97DFC0,        /* thiscall story + 0x20 */
    CF_POST2 = 0x987D90,        /* thiscall story: slot [type] -> current, MtObject_vf13(stage, start pos) */
    CF_UNLOCKREC = 0x9881C0,    /* thiscall story, (type, part, flag) */
    CF_SET_GAMETYPE = 0x887AF0, /* thiscall GameMan, type */
    CF_SET_CONTINUE = 0x439D70  /* thiscall story, flag */
};
#define ST_R1 0x189A0u
#define ST_R1_LEN 0x18978u
#define ST_R2 0x31330u
#define ST_R2_LEN 0x10F020u
#define ST_GI 0x31320u          /* cGameInfo */
#define ST_SLOT_TYPE 0x15A960u  /* cGameInfo + 0x129640: the game data slot of the game type */
#define ST_SLOT0 0x31330u       /* cGameInfo + 0x10 */

static volatile LONG g_on;              /* CPS_ON: the save name goes to the co-op file */
static cps_sm g_sm;
static unsigned char *g_snap1, *g_snap2;
static wchar_t g_dir[MAX_PATH], g_dat[MAX_PATH], g_rec[MAX_PATH], g_tmp[MAX_PATH];
static unsigned g_hash;                 /* hash of the Steam account (the file names) */
static void *g_rs, **g_rs_vt;           /* the game's ISteamRemoteStorage and its vtable */
static void *g_orig[16];
static volatile LONG n_routed_w, n_routed_r, n_routed_x, n_pass_w, n_pass_r;
static CRITICAL_SECTION g_lock;
static int g_lock_init;
static cps_rx g_rx;                     /* guest: the host's record (fed from the session's note callback) */
static unsigned g_pflags;
static int g_disabled;                  /* REV2_COOPSAVE=0 (lab): never route (the 0.2.0 behaviour, to compare) */
static char g_last[160];                /* the last routed operation, for the console */

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r2_safe_read((void *)a, &v, 4);
    return v;
}
static int readable(unsigned a, unsigned n)
{
    MEMORY_BASIC_INFORMATION mi;
    unsigned end = a + n;
    while (a < end) {
        if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        a = (unsigned)mi.BaseAddress + (unsigned)mi.RegionSize;
    }
    return 1;
}
static void lock_init(void)
{
    if (!g_lock_init) InitializeCriticalSection(&g_lock), g_lock_init = 1;
}

/* ------------------------------------------------------------------ files */
static void paths(void)
{
    if (g_dat[0]) return;
    wchar_t base[MAX_PATH];
    char lab[MAX_PATH];
    if (r2_env("REV2_COOPSAVE_DIR", lab, sizeof lab)) {   /* lab: a second copy on the same Windows user and Steam account (the N100 pair) keeps its own co-op save */
        MultiByteToWideChar(CP_ACP, 0, lab, -1, g_dir, MAX_PATH);
    } else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (!n || n >= MAX_PATH - 80) return;
        _snwprintf(g_dir, MAX_PATH, L"%ls\\CAPCOM", base);
        CreateDirectoryW(g_dir, NULL);
        _snwprintf(g_dir, MAX_PATH, L"%ls\\CAPCOM\\RESIDENT EVIL REVELATIONS2", base);
    }
    g_dir[MAX_PATH - 1] = 0;
    CreateDirectoryW(g_dir, NULL);
    _snwprintf(g_dat, MAX_PATH, L"%ls\\rev2_coop_save_%08x.dat", g_dir, g_hash);
    _snwprintf(g_rec, MAX_PATH, L"%ls\\rev2_coop_save_%08x.rec", g_dir, g_hash);
    _snwprintf(g_tmp, MAX_PATH, L"%ls\\rev2_coop_save_%08x.tmp", g_dir, g_hash);
}
static int file_write(const wchar_t *path, const void *a, unsigned na)
{
    /* atomic: a temporary file, flushed, then moved over the old one */
    HANDLE h = CreateFileW(g_tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    int ok = WriteFile(h, a, na, &w, NULL) && w == na;
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(g_tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(g_tmp);
    return ok;
}
static int file_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fa.nFileSizeHigh) return -1;
    return (int)fa.nFileSizeLow;
}
static int file_read(const wchar_t *path, void *buf, unsigned cb)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD r = 0;
    if (!ReadFile(h, buf, cb, &r, NULL)) r = 0;
    CloseHandle(h);
    return (int)r;
}

/* the checkpoint record of the campaign slot (the one the game just saved) next to the co-op save */
static void write_record(void)
{
    unsigned story = rd32(CA_STORY);
    if (!story) return;
    unsigned k = rd32(story + ST_SLOT_TYPE);
    if (k > 5) return;
    unsigned slot = story + ST_SLOT0 + k * CPS_SLOT_BYTES;
    if (!readable(slot, CPS_SLOT_BYTES)) return;
    static cps_rec r;   /* (23 KB: not on the stack of the storage thread) */
    cps_rec_from_slot(&r, (const uint8_t *)slot);
    int part = -1, diff = -1, st = 0, sp = 0;
    int cont = cps_rec_continuable(&r, &part, &diff, &st, &sp);
    if (file_write(g_rec, &r, sizeof r)) {
        g_pflags = cps_save_pflags(&r, CP_PF_SAVE0, CP_PF_SAVE1) | (cps_save_key(&r) << 8);   /* 0.3.1: the per-episode key in bits 8..15 */
        CLOG("record written: slot %u, stage %d start %d difficulty %d (%s)", k, (int)cps_rec_field(&r, CPS_S_STAGE), (int)cps_rec_field(&r, CPS_S_STARTPOS),
             (int)cps_rec_field(&r, CPS_S_DIFF), cont ? (part ? "Barry's half: continuable" : "Claire's half / bonus: continuable") : "not a co-op checkpoint");
    } else
        CLOG("record NOT written (error %lu)", GetLastError());
}

static int read_record(cps_rec *r)
{
    paths();
    if (!g_rec[0] || file_size(g_dat) <= 0 || file_size(g_rec) != (int)sizeof *r) return 0;
    if (file_read(g_rec, r, sizeof *r) != (int)sizeof *r) return 0;
    return cps_rec_valid(r);
}

/* ------------------------------------------------------------------ the wrapped ISteamRemoteStorage slots (thiscall, signatures from the game's call sites) */
static int THISCALL w_FileWrite(void *self, const char *name, const void *data, int cb)
{
    if (!cps_route(g_on && !g_disabled, name)) {
        if (cps_is_save_name(name)) {
            InterlockedIncrement(&n_pass_w);
            CLOG("FileWrite(%s, %d bytes) -> Steam (the solo save; no co-op game runs)", name, cb);
        }
        return ((int (THISCALL *)(void *, const char *, const void *, int))g_orig[0])(self, name, data, cb);
    }
    int ok = cb > 0 && data && file_write(g_dat, data, (unsigned)cb);
    InterlockedIncrement(&n_routed_w);
    snprintf(g_last, sizeof g_last, "FileWrite %d bytes -> co-op file: %s", cb, ok ? "ok" : "FAILED");
    CLOG("FileWrite(%s, %d bytes) -> the co-op save: %s", name, cb, ok ? "written" : "FAILED");
    if (ok) write_record();
    return ok;
}
static int THISCALL w_FileRead(void *self, const char *name, void *buf, int cb)
{
    if (!cps_route(g_on && !g_disabled, name)) {
        if (cps_is_save_name(name)) InterlockedIncrement(&n_pass_r), CLOG("FileRead(%s) -> Steam (the solo save)", name);
        return ((int (THISCALL *)(void *, const char *, void *, int))g_orig[1])(self, name, buf, cb);
    }
    int r = cb > 0 && buf ? file_read(g_dat, buf, (unsigned)cb) : 0;
    InterlockedIncrement(&n_routed_r);
    snprintf(g_last, sizeof g_last, "FileRead -> co-op file: %d bytes", r);
    CLOG("FileRead(%s) -> the co-op save: %d bytes", name, r);
    return r;
}
static int THISCALL w_FileDelete(void *self, const char *name)
{
    if (!cps_route(g_on && !g_disabled, name)) return ((int (THISCALL *)(void *, const char *))g_orig[6])(self, name);
    int had = file_size(g_dat) >= 0;
    DeleteFileW(g_dat), DeleteFileW(g_rec);
    InterlockedIncrement(&n_routed_x);
    g_pflags = 0;
    CLOG("FileDelete(%s) -> the co-op save %s", name, had ? "deleted" : "did not exist");
    return had;
}
static int THISCALL w_FileExists(void *self, const char *name)
{
    if (!cps_route(g_on && !g_disabled, name)) return ((int (THISCALL *)(void *, const char *))g_orig[13])(self, name);
    int e = file_size(g_dat) > 0;
    CLOG("FileExists(%s) -> the co-op save: %s", name, e ? "yes" : "no");
    return e;
}
static int THISCALL w_GetFileSize(void *self, const char *name)
{
    if (!cps_route(g_on && !g_disabled, name)) return ((int (THISCALL *)(void *, const char *))g_orig[15])(self, name);
    int n = file_size(g_dat);
    return n < 0 ? 0 : n;
}

void coopsave_on_context(void *ctx)
{
    static void *last_ctx, *last_rs;
    if (!ctx) return;
    void *rs = ((void **)ctx)[9];   /* +0x24 (the game calls SteamInternal_ContextInit for every Steam use: this is the fast path, no syscall) */
    if (ctx == last_ctx && rs == last_rs && g_rs_vt) return;
    last_ctx = ctx, last_rs = rs;
    if (!rs || !r2_exe_ok()) return;
    void *user = ((void **)ctx)[1];
    void **vt = NULL;
    r2_safe_read(rs, &vt, 4);
    if (!vt || vt == g_rs_vt) return;
    lock_init();
    {
        char v[8];
        g_disabled = r2_env("REV2_COOPSAVE", v, sizeof v) && v[0] == '0';
    }
    if (user && !g_hash) {   /* the Steam account (the low word of ISteamUser::GetSteamID): a hash of it names the files */
        unsigned long long id = 0;
        ((void *(THISCALL *)(void *, unsigned long long *))VT(user)[2])(user, &id);
        g_hash = cps_account_hash((uint32_t)id);
    }
    static const int slots[] = {0, 1, 6, 13, 15};
    void *hooks[] = {(void *)w_FileWrite, (void *)w_FileRead, (void *)w_FileDelete, (void *)w_FileExists, (void *)w_GetFileSize};
    DWORD old;
    if (!VirtualProtect(&vt[0], 16 * sizeof(void *), PAGE_READWRITE, &old)) {
        CLOG("ISteamRemoteStorage vtable %p not writable: co-op save routing OFF (error %lu)", (void *)vt, GetLastError());
        return;
    }
    for (int i = 0; i < 5; i++) g_orig[slots[i]] = vt[slots[i]], vt[slots[i]] = hooks[i];
    VirtualProtect(&vt[0], 16 * sizeof(void *), old, &old);
    g_rs = rs, g_rs_vt = vt;
    paths();
    cps_rec r;
    if (read_record(&r)) g_pflags = cps_save_pflags(&r, CP_PF_SAVE0, CP_PF_SAVE1) | (cps_save_key(&r) << 8);
    CLOG("ISteamRemoteStorage %p: FileWrite / FileRead / FileDelete / FileExists / GetFileSize wrapped%s; co-op save %s, record %s", rs, g_disabled ? " (REV2_COOPSAVE=0: never routed)" : "",
         file_size(g_dat) > 0 ? "present" : "none yet", g_pflags & CP_PF_SAVE1 ? "Barry's half" : g_pflags & CP_PF_SAVE0 ? "Claire's half" : "none");
}

/* ------------------------------------------------------------------ entering and leaving a co-op game */
int coopsave_active(void) { return g_on != 0; }

static int save_idle(void)
{
    unsigned sm = rd32(CA_SAVEMAN), sd = rd32(CA_SAVEDATA);
    if (!sm || !sd) return 0;
    return rd32(sm + 0x20) == 0 && rd32(sd + 0x24) == 0 && rd32(sd + 0xD44) == 0;
}

/* the game's own load of "DATA0.DAT" (routed: the co-op file) and its unpack; 1 = the co-op save is in memory now */
static int load_coop(void)
{
    unsigned sd = rd32(CA_SAVEDATA), stor = rd32(CA_STORAGE);
    if (!sd || !stor) return 0;
    HANDLE mtx = (HANDLE)rd32(sd + 0xD40);
    DWORD w = mtx ? WaitForSingleObject(mtx, 5000) : WAIT_FAILED;
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
        CLOG("load: the save thread's lock was not free (%lu)", w);
        return 0;
    }
    unsigned one = 1, zero = 0;
    memcpy((void *)(sd + 0x24), &one, 4), memcpy((void *)(sd + 0x28), &zero, 4);
    ((void(THISCALL *)(void *))CF_LOAD)((void *)sd);
    ReleaseMutex(mtx);
    unsigned res = rd32(sd + 0x28);
    if (res != 0) {
        CLOG("load: the game's read of the co-op save answered %u (3 = no file, 4..7 = not a save of this version)", res);
        return 0;
    }
    ((void(THISCALL *)(void *, int))CF_STORAGE_USER)((void *)stor, 0);
    unsigned char ok = ((unsigned char(THISCALL *)(void *))CF_UNPACK)((void *)stor);
    CLOG("load: the game's unpack of the co-op save: %s", ok == 1 ? "ok (the co-op save is the game's save now)" : "REFUSED (corrupt or another account's): the co-op game starts from the solo image");
    return ok == 1;
}

int coopsave_enter(const char *why)
{
    lock_init();
    if (g_on) return 1;
    if (!g_rs_vt || g_disabled) {
        CLOG("enter (%s): %s", why, g_disabled ? "REV2_COOPSAVE=0: the co-op game uses the solo save (0.2.0 behaviour)" : "ISteamRemoteStorage not wrapped: NOT entered");
        return -1;
    }
    if (!save_idle()) return 0;
    unsigned story = rd32(CA_STORY);
    if (!story || !readable(story + ST_R1, ST_R1_LEN) || !readable(story + ST_R2, ST_R2_LEN)) {
        CLOG("enter (%s): no story block: NOT entered", why);
        return -1;
    }
    if (!g_snap1) g_snap1 = malloc(ST_R1_LEN);
    if (!g_snap2) g_snap2 = malloc(ST_R2_LEN);
    if (!g_snap1 || !g_snap2) {
        CLOG("enter (%s): out of memory: NOT entered", why);
        return -1;
    }
    memcpy(g_snap1, (void *)(story + ST_R1), ST_R1_LEN);
    memcpy(g_snap2, (void *)(story + ST_R2), ST_R2_LEN);
    paths();
    unsigned c1 = cps_crc32(0, g_snap1, ST_R1_LEN), c2 = cps_crc32(0, g_snap2, ST_R2_LEN);
    memset(&g_sm, 0, sizeof g_sm);
    g_sm.state = CPS_ON, g_sm.enter_tick = GetTickCount();
    InterlockedExchange(&g_on, 1);
    int have = file_size(g_dat) > 0;
    unsigned k = rd32(story + ST_SLOT_TYPE), slot = k <= 5 ? story + ST_SLOT0 + k * CPS_SLOT_BYTES : 0;
    CLOG("ON (%s): solo save image kept (crc %08x / %08x; its campaign slot %u: stage %u start %u), the save name now goes to the co-op save (%s)", why, c1, c2, k,
         slot ? rd32(slot + CPS_S_STAGE) : 0, slot ? rd32(slot + CPS_S_STARTPOS) : 0, have ? "present: loading it" : "none yet: the co-op game starts from a copy of the solo image");
    if (have && load_coop() && slot)
        CLOG("the co-op save in memory: campaign slot %u stage %u start %u difficulty %u", k, rd32(slot + CPS_S_STAGE), rd32(slot + CPS_S_STARTPOS), rd32(slot + CPS_S_DIFF));
    return 1;
}

static void leave(const char *why)
{
    unsigned story = rd32(CA_STORY);
    if (story && g_snap1 && g_snap2 && readable(story + ST_R1, ST_R1_LEN) && readable(story + ST_R2, ST_R2_LEN)) {
        memcpy((void *)(story + ST_R1), g_snap1, ST_R1_LEN);
        memcpy((void *)(story + ST_R2), g_snap2, ST_R2_LEN);
        ((void(THISCALL *)(void *))CF_POST1)((void *)(story + 0x20));
        ((void(THISCALL *)(void *))CF_POST2)((void *)story);
        CLOG("OFF (%s): the solo save image is back (crc %08x / %08x), the save name goes to Steam again", why, cps_crc32(0, (void *)(story + ST_R1), ST_R1_LEN),
             cps_crc32(0, (void *)(story + ST_R2), ST_R2_LEN));
    } else
        CLOG("OFF (%s): no story block to put the solo image back into", why);
    g_sm.state = CPS_OFF;
    InterlockedExchange(&g_on, 0);
    EnterCriticalSection(&g_lock);
    cps_rx_reset(&g_rx);   /* a record belongs to one start */
    LeaveCriticalSection(&g_lock);
    cps_rec r;
    g_pflags = read_record(&r) ? cps_save_pflags(&r, CP_PF_SAVE0, CP_PF_SAVE1) | (cps_save_key(&r) << 8) : 0;
}

void coopsave_frame(int in_menus, int title_menus, int session_running)
{
    if (!g_on) return;
    if (cps_leave_due(&g_sm, !in_menus, title_menus, save_idle(), session_running, GetTickCount())) leave(g_sm.level_seen ? "back at the title after the co-op game" : "the co-op start did not reach a level");
}

/* ------------------------------------------------------------------ Continue */
int coopsave_continue_start(int follow, int game_type)
{
    unsigned story = rd32(CA_STORY), gm = rd32(CA_GAMEMAN);
    if (!story || !gm) return 0;
    if (game_type != 0 && game_type != 3 && game_type != 4) game_type = 0;
    ((void(THISCALL *)(void *, int))CF_SET_GAMETYPE)((void *)gm, game_type);   /* 0.3.1: the bonus episodes continue their own slot (game types 3 / 4) */
    unsigned k = rd32(story + ST_SLOT_TYPE);
    if (k > 5 || (game_type && k != (unsigned)game_type)) {   /* SetGameType maps type 0..4 to slot 0..4 (FUN_009756F0); slot 5 = saving off (static g2c_bc*: research of 2026-11-12) */
        CLOG("continue: game type %d, game data slot %u: refused", game_type, k);
        return 0;
    }
    unsigned slot = story + ST_SLOT0 + k * CPS_SLOT_BYTES;
    if (game_type) {   /* 0.3.1: a bonus checkpoint: the slot holds a stage of that bonus episode (cps_room_episode on the host's record and on this PC's own) */
        int sst = (int)rd32(slot + CPS_S_STAGE);
        if (sst < (game_type == 3 ? 600 : 700) || sst > (game_type == 3 ? 699 : 799)) {
            CLOG("continue: game type %d, slot %u holds stage %d: refused", game_type, k, sst);
            return 0;
        }
    }
    if (follow) {
        lock_init();
        EnterCriticalSection(&g_lock);
        int ok = g_rx.complete;
        if (ok) cps_rec_apply_slot(&g_rx.rec, (uint8_t *)slot);
        LeaveCriticalSection(&g_lock);
        CLOG("continue: %s", ok ? "the host's checkpoint record applied to my campaign slot (the guest follows the host)" : "NO complete record from the host: my own co-op checkpoint");
    }
    ((void(THISCALL *)(void *))CF_POST1)((void *)(story + 0x20));
    ((void(THISCALL *)(void *))CF_POST2)((void *)story);
    unsigned char one = 1;
    memcpy((void *)(story + 0x15C485), &one, 1);
    int stage = (int)rd32(story + 0x140380), part = cps_stage_part(stage);
    if (game_type) {   /* the stock bonus Continue (cDemoCampaignMenu_OnSelect case 0x11, types 3 / 4): the unlock records of that type (records 6 / 7), no part byte, no done check */
        ((void(THISCALL *)(void *, int, int, int))CF_UNLOCKREC)((void *)story, game_type, 0, 0);
        ((void(THISCALL *)(void *, int, int, int))CF_UNLOCKREC)((void *)story, game_type, 1, 0);
        unsigned d = 0xD, m1 = 0xFFFFFFFFu, z = 0;
        memcpy((void *)(gm + 0x20), &d, 4), memcpy((void *)(gm + 0x24), &m1, 4);
        ((void(THISCALL *)(void *, int))CF_SET_CONTINUE)((void *)story, 1);
        memcpy((void *)(gm + 0x30), &z, 4);
        CLOG("continue: the stock bonus Continue (game type %d) of slot %u: chapter %u stage %d start position %u difficulty %u", game_type, k, rd32(story + 0x140378), stage,
             rd32(story + 0x140384), rd32(story + 0x140374));
        return 1;
    }
    unsigned up = part > 0 ? 1u : 0u;
    memcpy((void *)(story + 0x15C4A8), &up, 4);
    unsigned char done = 0;
    r2_safe_read((void *)(story + ST_GI + 0x10F0B8), &done, 1);
    if (done) {
        ((void(THISCALL *)(void *, int, int, int))CF_UNLOCKREC)((void *)story, 0, 0, 1);
        ((void(THISCALL *)(void *, int, int, int))CF_UNLOCKREC)((void *)story, 0, 1, 1);
    } else
        ((void(THISCALL *)(void *, int, int, int))CF_UNLOCKREC)((void *)story, 0, part < 0 ? 0 : part, 0);
    unsigned d = 0xD, m1 = 0xFFFFFFFFu, z = 0;
    memcpy((void *)(gm + 0x20), &d, 4), memcpy((void *)(gm + 0x24), &m1, 4);
    ((void(THISCALL *)(void *, int))CF_SET_CONTINUE)((void *)story, 1);
    memcpy((void *)(gm + 0x30), &z, 4);
    CLOG("continue: the stock Continue of slot %u: stage %d start position %u difficulty %u (half %d)", k, stage, rd32(story + 0x140384), rd32(story + 0x140374), part);
    return 1;
}

/* ------------------------------------------------------------------ the lobby's view and the record on the wire */
static cps_rec g_sum_rec;
int coopsave_summary(int *part, int *difficulty, int *stage, int *startpos)
{
    static DWORD t;
    static int ok;
    DWORD now = GetTickCount();
    if (!t || now - t > 1500) {   /* the lobby asks every frame: read the 23 KB file at most every 1.5 s */
        t = now | 1u;
        ok = read_record(&g_sum_rec);
    }
    return ok && cps_rec_continuable(&g_sum_rec, part, difficulty, stage, startpos);
}
/* 0.3.1 (C1): the same plus the room episode of the checkpoint (1-4, 5 The Struggle, 9 Little Miss) */
int coopsave_summary_ep(int *episode, int *part, int *difficulty)
{
    if (!coopsave_summary(part, difficulty, NULL, NULL)) return 0;
    if (episode) *episode = cps_rec_room_episode(&g_sum_rec);
    return 1;
}

unsigned coopsave_pflags(void) { return g_disabled ? 0 : g_pflags; }

static int emit_event(void *user, const uint8_t *body, unsigned len) { return cs_send_event((cs_t *)user, COOPSAVE_EV_KIND, body, len); }

int coopsave_send_record(cs_t *s, char *err, int esz)
{
    static cps_rec r;
    if (!read_record(&r) || !cps_rec_continuable(&r, NULL, NULL, NULL, NULL)) {
        snprintf(err, (size_t)esz, "no co-op save to continue");
        return 0;
    }
    int n = cps_wire_send(&r, emit_event, s);
    CLOG("host: checkpoint record sent in %d events (stage %d start %d, %u flag words)", n, (int)cps_rec_field(&r, CPS_S_STAGE), (int)cps_rec_field(&r, CPS_S_STARTPOS),
         cps_rec_pairs(&r));
    if (n <= 0) snprintf(err, (size_t)esz, "the record could not be sent");
    return n > 0;
}

void coopsave_on_event(const void *body, unsigned len)
{
    lock_init();
    EnterCriticalSection(&g_lock);
    int done = cps_rx_feed(&g_rx, (const uint8_t *)body, len);
    LeaveCriticalSection(&g_lock);
    if (done) CLOG("guest: the host's checkpoint record arrived (stage %d start %d, %u flag words)", (int)cps_rec_field(&g_rx.rec, CPS_S_STAGE),
                   (int)cps_rec_field(&g_rx.rec, CPS_S_STARTPOS), g_rx.pairs_got);
}

int coopsave_follow_ready(void) { return g_rx.complete; }

void coopsave_status(char *out, int size)
{
    int part = -1, diff = -1, st = 0, sp = 0;
    int c = coopsave_summary(&part, &diff, &st, &sp);
    char dir[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, g_dir, -1, dir, sizeof dir, NULL, NULL);
    const char *tail = strrchr(dir, '\\');
    unsigned story = rd32(CA_STORY), k = story ? rd32(story + ST_SLOT_TYPE) : 99, slot = k <= 5 ? story + ST_SLOT0 + k * CPS_SLOT_BYTES : 0;
    snprintf(out, (size_t)size,
             "coopsave %s%s | wrapped %s | co-op save %d bytes, record %s | continue %s (half %d stage %d start %d difficulty %d) | routed write %ld read %ld delete %ld, solo write %ld read %ld | "
             "host record %s (%u words) | in memory: slot %u stage %u start %u difficulty %u episode %u, current stage %u start %u | folder ...%s | last: %s",
             g_on ? "ON" : "off", g_disabled ? " (REV2_COOPSAVE=0)" : "", g_rs_vt ? "yes" : "no", file_size(g_dat), file_size(g_rec) > 0 ? "present" : "none", c ? "yes" : "no", part, st,
             sp, diff, n_routed_w, n_routed_r, n_routed_x, n_pass_w, n_pass_r, g_rx.complete ? "complete" : g_rx.have_head ? "partial" : "none", g_rx.pairs_got, k,
             slot ? rd32(slot + CPS_S_STAGE) : 0, slot ? rd32(slot + CPS_S_STARTPOS) : 0, slot ? rd32(slot + CPS_S_DIFF) : 0, slot ? rd32(slot + CPS_S_EPISODE) : 0,
             story ? rd32(story + 0x140380) : 0, story ? rd32(story + 0x140384) : 0, tail ? tail : "", g_last[0] ? g_last : "-");
}
