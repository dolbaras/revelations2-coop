/* world.inc: stage 6, the shared world (enemies, items, damage). Included at the end of coop3.c after coop_game.inc (same translation unit:
 * rd32/rdf/wr/do_call/player_obj/game_area, g_cs, g_my_slot, g_rem_slot, g_state come from there). Design and evidence: notes\stage6-design.md (private),
 * public summary docs\world.md.
 *
 * Principle (mirror model, host authority): both PCs run the engine and the story FSM; the host's enemies / items are the truth. This file holds
 *   1. detours into the game (each one checked byte for byte, `net w on` / REV2_WORLD=1 only, nothing is patched otherwise):
 *        AddItem        0x6EA6A0  stdcall (player, item id, count, uItem object, flag)  - THE place where an item enters a personal inventory
 *        CreateEnemy    0x5FD7C0  thiscall sEnemy (LayoutID by value: vtbl, a, b|c<<16, flag, flag), ret 0x14 - every enemy that the FSM creates
 *        ApplyDamage    0x604CE0  thiscall uEnemy (DamageInfo *, amount), ret 8 - every hit on an enemy (attacker object at DamageInfo+0xDC)
 *   2. diagnostics (`net w list`, `net w obj <addr>`) for the live object lists of sEnemy (+0x30 count/+0x3C array, +0x44/+0x50) and sItem,
 *   3. the events: SPAWN / HIT / DEATH / PICKUP (reliable) and the enemy record batch (WORLD, unreliable, 10-20 Hz),
 *   4. the receivers that apply them on the other PC (client creates by LayoutID, client damage becomes HIT, host DEATH kills the replica).
 * The layout identity of a placed object is its embedded LayoutID at +0x19CC (i32 a), +0x19D0 (u16 b), +0x19D2 (u16 c).
 */
#define W_LOG "world: "

/* ---------------------------------------------------------------- detour helper (same method as trace.c: trampoline with the stolen prologue bytes) */
static void *w_tramp(unsigned target, const unsigned char *expect, int n)
{
    unsigned char cur[16];
    if (n < 5 || n > 12 || !r2_safe_read((void *)target, cur, (unsigned)n) || memcmp(cur, expect, (size_t)n) != 0) {
        r2_log(W_LOG "bytes at %08x are not the expected ones, hook NOT installed", target);
        return NULL;
    }
    unsigned char *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, cur, (size_t)n);
    t[n] = 0xE9;
    *(int *)(t + n + 1) = (int)((target + (unsigned)n) - (unsigned)(t + n + 5));
    return t;
}

static int w_jmp(unsigned target, int n, void *hook)
{
    DWORD old;
    if (!VirtualProtect((void *)target, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    unsigned char *p = (unsigned char *)target;
    for (int i = 5; i < n; i++) p[i] = 0x90;
    *(int *)(p + 1) = (int)((unsigned)hook - (target + 5));
    p[0] = 0xE9;
    VirtualProtect((void *)target, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)target, (SIZE_T)n);
    return 1;
}

#include "probe.inc"

/* ---------------------------------------------------------------- state */
enum { WK_ENEMIES = 1 };           /* WORLD batch kinds */
enum { EV_W_SPAWN = 0x120, EV_W_HIT, EV_W_DEATH, EV_W_PICKUP, EV_W_ITEM_GONE, EV_W_PDMG, EV_W_FLAG, EV_W_SCENE, EV_W_GRANT, EV_W_DOOR, EV_W_BARRIER, EV_W_SNAP, EV_W_SF, EV_W_BGL };   /* CP_EV_USER + n: world events (docs\world.md) */

static unsigned g_w_put_tx;
static int g_w_in_additem;            /* >0 while AddItem runs (its PutItem calls are part of the pickup, not separate grants) */
static int g_w_installed;              /* hooks installed (g_w_on, world sync on, is declared in coop_game.inc) */
static int g_w_log_calls = 60;             /* log the first N calls of each hook */
static unsigned g_w_n_additem, g_w_n_create, g_w_n_damage, g_w_n_remote_additem;
static int g_w_replay;                     /* >0 while an event of the other PC is being replayed here (the hooks do not announce it again) */
static int g_w_from_host;                  /* >0 while the client creates an enemy the host announced (the creator guard lets it through) */
static int g_w_grab_attach = 1;             /* 6f: `net w grab 0|1` */
static int g_w_replica_free = 1;              /* 6g: `net w grab free 0|1`: a replica inside the grab chain is not re-forced (glue_replica_force_ok) */
static unsigned g_w_chain_skips;
static unsigned g_w_grab_n, g_w_grab_last_tag;
static glue_demand_t g_w_demand[GLUE_DEMAND_SLOTS];
static int g_w_demand_on = 1;               /* 6f: `net w demand 0|1` */
static unsigned g_w_demand_allowed;
static int g_w_guard = 1;                  /* client: enemies the FSM wants to create on its own are refused (the host's SPAWN creates them) */
static unsigned g_w_refused;
static FILE *g_w_trace;                       /* `net w trace <file>`: 10 Hz CSV of enemies and humans */
static unsigned g_w_trace_t, g_w_trace_seen[64];
static double g_w_err_sum, g_w_err_max; static unsigned g_w_err_n;   /* distance between the replica and the host's record when the record is applied (xz, units) */

#define SENEMY_CELL 0x1561E70u
#define SITEM_CELL 0x1562E10u
#define LAYOUT_VTBL 0x1259070u
#define VTBL_SLOT_APPLYDAMAGE_OFF 0x3A8u

typedef struct { int a; unsigned b, c; } layout_t;

static int layout_of(unsigned obj, layout_t *l)
{
    if (!obj) return 0;
    l->a = (int)rd32(obj + 0x19CC);
    unsigned w = rd32(obj + 0x19D0);
    l->b = w & 0xFFFF, l->c = w >> 16;
    return 1;
}

static const char *w_class(unsigned obj, char *buf, int size)
{
    static char big[4096];   /* classes_name writes "vtable exe+..., DTI ...: <chain>" and does not bound the chain by the caller's size: give it room, then keep the first class name */
    classes_name(obj, big, sizeof big - 1);
    big[sizeof big - 1] = 0;
    char *p = strstr(big, ": ");
    p = p ? p + 2 : big;
    int i = 0;
    for (; p[i] && p[i] != ' ' && p[i] != '\n' && i < size - 1; i++) buf[i] = p[i];
    buf[i] = 0;
    return buf;
}

/* the player slot of an uPlayer object (+0x790C), -1 when it is not one */
static int w_slot_of_player(unsigned p)
{
    if (!p) return -1;
    for (int i = 0; i < 2; i++)
        if (player_obj(i) == p) return i;
    return -1;
}

/* ---------------------------------------------------------------- hooks */
typedef int(__stdcall *additem_t)(unsigned player, unsigned item, int count, unsigned itemobj, int flag);
typedef void *(__attribute__((thiscall)) *create_t)(void *self, unsigned vtbl, int a, unsigned bc, unsigned p5, unsigned p6);
typedef void(__attribute__((thiscall)) *damage_t)(void *self, void *dmg, int amount);
typedef void(__attribute__((thiscall)) *psub_t)(void *self, int amount);
static psub_t o_psub;
static unsigned g_w_pdmg_sent, g_w_pdmg_applied;
static int g_w_pdmg_mode;   /* 0: damage to the puppet is ignored (the replica enemy on the owner's PC attacks the real body, as the Raid design does), 1: forward it (PDMG) */
static additem_t o_additem;
static create_t o_create;
/* ApplyDamage is a virtual (slot 234, +0x3A8) that most enemy classes override (0x604CE0 base, 0x5D5AF0 for Em1100/1130/1140/5100/5400, 0x582210 Em0100, 0x57C190 Em0200, 0x560610 Em0300, ...),
 * so it is intercepted in the vtables: every enemy class vtable gets h_damage in that slot and the original pointer is kept per vtable. */
static const struct { unsigned vt, orig; } w_vts[] = {
    {0x1283DD8, 0x582210}, {0x1284990, 0x604CE0}, {0x1284470, 0x604CE0}, {0x1282950, 0x57C190},
    {0x1282E80, 0x604CE0}, {0x127FD80, 0x560610}, {0x12802B0, 0x604CE0}, {0x128F6E0, 0x5D5AF0},
    {0x128DEA0, 0x604CE0}, {0x12919F8, 0x5F0200}, {0x1289D28, 0x5D5AF0}, {0x128FC28, 0x5D5AF0},
    {0x127EF68, 0x604CE0}, {0x1295D28, 0x62F7A0}, {0x12975D8, 0x604CE0}, {0x12947B0, 0x604CE0},
    {0x126DF28, 0x5066B0}, {0x126E968, 0x604CE0}, {0x126E448, 0x5066B0}, {0x1277548, 0x5368C0},
    {0x1284EB0, 0x604CE0}, {0x1277A68, 0x5368C0}, {0x126B0E8, 0x604CE0}, {0x1279890, 0x604CE0},
    {0x127D0A8, 0x604CE0}, {0x127A748, 0x604CE0}, {0x127B488, 0x604CE0}, {0x127C1C0, 0x604CE0},
    {0x1271290, 0x604CE0}, {0x12717F8, 0x604CE0}, {0x1274298, 0x604CE0}, {0x1272C18, 0x604CE0},
    {0x12735A0, 0x604CE0}, {0x1273AE8, 0x604CE0}, {0x1270D70, 0x604CE0}, {0x12747C0, 0x604CE0},
    {0x12721E0, 0x604CE0}, {0x12726F8, 0x604CE0}, {0x1267030, 0x5D5AF0}, {0x1269320, 0x5D5AF0},
    {0x12755E8, 0x604CE0}, {0x12682D8, 0x5D5AF0}, {0x1265CF8, 0x604CE0}, {0x1286218, 0x604CE0},
    {0x1286740, 0x604CE0}, {0x1286C68, 0x604CE0}, {0x1287190, 0x604CE0}, {0x12876B8, 0x604CE0},
    {0x1287BE0, 0x604CE0}, {0x1288108, 0x604CE0}, {0x1288630, 0x604CE0}, {0x1288B58, 0x604CE0},
    {0x1289080, 0x604CE0}, {0x127D8E0, 0x604CE0}, {0x1285CF0, 0x604CE0}, {0x1292E70, 0x604CE0},
};
#define W_NVT ((int)(sizeof w_vts / sizeof w_vts[0]))
static int g_w_vt_patched;
static damage_t w_orig_for(unsigned obj)
{
    unsigned vt = rd32(obj);
    for (int i = 0; i < W_NVT; i++)
        if (w_vts[i].vt == vt) return (damage_t)w_vts[i].orig;
    return NULL;
}

static void w_ev_send(unsigned kind, const void *body, unsigned len)
{
    if (g_cs && cs_active(g_cs)) cs_send_event(g_cs, kind, body, len);
}

static int __stdcall h_additem(unsigned player, unsigned item, int count, unsigned itemobj, int flag)
{
    const void *ret = __builtin_return_address(0);
    g_w_in_additem++;
    int r = o_additem(player, item, count, itemobj, flag);
    g_w_in_additem--;
    g_w_n_additem++;
    int slot = w_slot_of_player(player);
    if (g_w_n_additem <= (unsigned)g_w_log_calls) {
        layout_t l = {0, 0, 0};
        layout_of(itemobj, &l);
        unsigned type = player ? rd32(player + 0x7920) : 9;
        r2_log(W_LOG "AddItem(player %08x slot %d type %u, item %04x, count %d, obj %08x layout (%d,%u,%u), flag %d) -> %d, caller exe+%x%s", player, slot, type, item, count, itemobj, l.a, l.b, l.c, flag, r,
               (unsigned)ret - 0x400000, g_w_replay ? " [replay]" : "");
    }
    /* an item the local human took from the world (the uItem object is consumed inside AddItem): the other PC does the same with its copy of the object */
    if (g_w_on && !g_w_replay && itemobj && slot >= 0 && slot == g_my_slot && g_cs && cs_active(g_cs)) {
        layout_t l;
        layout_of(itemobj, &l);
        unsigned char b[24];
        memcpy(b, &item, 4), memcpy(b + 4, &count, 4), memcpy(b + 8, &l.a, 4), memcpy(b + 12, &l.b, 2), memcpy(b + 14, &l.c, 2);
        b[16] = (unsigned char)slot;
        memcpy(b + 17, &flag, 4);
        w_ev_send(EV_W_PICKUP, b, 21);
        g_w_put_tx++;
    }
    return r;
}

/* player HP subtract (0x715020 is slot 44 of every uPl* vtable): on the host the damage an enemy does to the puppet (the other human's body) is sent to that human's PC instead; there it is applied to the real body */
static __attribute__((thiscall)) void h_psub(void *self, int amount)
{
    unsigned p = (unsigned)self;
    int slot = w_slot_of_player(p);
    if (g_w_on && g_w_pdmg_mode == 1 && !g_w_replay && g_cs && cs_active(g_cs) && slot >= 0 && slot == g_rem_slot && amount > 0 && rd32(p + 0x7920) == 2) {
        unsigned char b[8];
        b[0] = (unsigned char)slot;
        memcpy(b + 1, &amount, 4);
        w_ev_send(EV_W_PDMG, b, 5);
        if (++g_w_pdmg_sent <= (unsigned)g_w_log_calls) r2_log(W_LOG "PDMG sent: damage %d to slot %d (the puppet), caller exe+%x", amount, slot, (unsigned)__builtin_return_address(0) - 0x400000);
        return;
    }
    o_psub(self, amount);
}

/* ---- replica AI off (client): the Intel (the enemy's brain, class uEm*Intel, owner at +0x30) runs as a job of the game's task runner (0xC19540 calls the entry of the object's update, the common one is
 * uEmIntel_vf46 = 0x600F50; most uEm*Intel vtables point at a thunk `jmp 0x600F50`). It picks the target (uEmIntel_vf52), decides the next action (vf65 -> vf62/vf63) and starts it (vtable +0x58).
 * On a client every enemy is a replica of the host's (its own creators are refused), the host's records say what it does, so its own brain must not decide anything: the update returns at once.
 * Switch: `net w ai 0|1` (1 = replicas keep their own AI, the stage 6 behaviour); REV2_REPLICA_AI=1 in the environment. Classes with their own update (Em0100, 0200, 0300, 1300, 1500, 3000, 3500, 5xxx) are not covered yet. */
typedef void(__attribute__((thiscall)) *intel_upd_t)(void *self);
static intel_upd_t o_intel_upd;
static unsigned g_w_ai_skipped;
static __attribute__((thiscall)) void h_intel_update(void *self)
{
    if (!g_w_replica_ai && g_w_on && g_w_guard && g_cs && cs_active(g_cs) && !cs_is_host(g_cs)) {
        g_w_ai_skipped++;
        return;
    }
    o_intel_upd(self);
}

/* ---- story flag bit table (story block + 0xEF0, ids are bit numbers): FUN_0097F170(story, id, value) is the only writer (FUN_00407290 is its locked wrapper) ---- */
typedef void(__attribute__((thiscall)) *flagset_t)(void *story, unsigned id, int val);
static flagset_t o_flagset;
static unsigned g_w_flag_tx, g_w_flag_rx, g_w_flag_n;
static int g_w_flag_sync = 1, g_w_flag_log = 300;
static int w_flag_get(unsigned story, unsigned id)
{
    return id == 0xFFFFFFFFu ? 0 : (int)((rd32(story + 0xEF0 + (id >> 5) * 4) >> (id & 31)) & 1);
}

static __attribute__((thiscall)) void h_flagset(void *self, unsigned id, int val)
{
    unsigned story = (unsigned)self;
    int before = w_flag_get(story, id);
    const void *ret = __builtin_return_address(0);
    o_flagset(self, id, val);
    int after = w_flag_get(story, id);
    if (before == after || story != rd32(0x157D120)) return;
    g_w_flag_n++;
    if ((int)g_w_flag_n <= g_w_flag_log) r2_log(W_LOG "flag %u (0x%x) %d -> %d, caller exe+%x%s", id, id, before, after, (unsigned)ret - 0x400000, g_w_replay ? " [replay]" : "");
    if (g_w_on && g_w_flag_sync && !g_w_replay && g_cs && cs_active(g_cs)) {
        unsigned char b[8];
        memcpy(b, &id, 4);
        b[4] = (unsigned char)after;
        w_ev_send(EV_W_FLAG, b, 5);
        g_w_flag_tx++;
    }
}

/* ---- stats guard: whose profile a kill belongs to (stage 6d) ----
 * FUN_00986230 (thiscall on the story block, `ret 0xC`: enemy, DamageInfo, max hp) is the game's kill report; for every kill it adds to THREE tables of the profile that the achievements read:
 * the global total by kind (FUN_00979580, story+0x20+0x1BC, u32 x 0x51; the record check `goal <= total` of KNIFE_WEAPON_KILL and the others compares against it), the per-slot table (FUN_00972E90,
 * story+0x31320+0x10F0D8, u16 x 0x66 x 8) and, only for a HUMAN killer slot, the credited ledger (FUN_00972ED0, story+0x31320+0x118D38, u16 x 0x51). The global total is bumped for ANY killer slot,
 * so a kill of the OTHER player (the puppet that replays his hit on the host, or a replica that dies by the host's blow on the client) would move this PC's achievements towards the partner's work.
 * In a running session the report is therefore run on a snapshot of the three tables and rolled back unless the killer is the local human. Switch: `net w statguard 0|1`. */
typedef void(__attribute__((thiscall)) *killrep_t)(void *story, unsigned enemy, unsigned di, int maxhp);
static killrep_t o_killrep;
static int g_w_stat_guard = 1;
static unsigned g_w_stat_rolled, g_w_stat_kept;
#define STAT_GLOBAL(st) ((st) + 0x20 + 0x1BC)
#define STAT_SLOT(st) ((st) + 0x31320 + 0x10F0D8)
#define STAT_HUMAN(st) ((st) + 0x31320 + 0x118D38)
enum { STAT_GLOBAL_LEN = 0x51 * 4, STAT_SLOT_LEN = 8 * 0x66 * 2, STAT_HUMAN_LEN = 0x51 * 2 };

static __attribute__((thiscall)) void h_killrep(void *story_p, unsigned enemy, unsigned di, int maxhp)
{
    unsigned st = (unsigned)story_p;
    unsigned att = di ? rd32(di + 0xDC) : 0;
    int active = g_w_on && g_w_stat_guard && g_cs && cs_active(g_cs) && st == rd32(0x157D120);
    int mine = att && att == player_obj(g_my_slot);
    int theirs = att && att == player_obj(g_rem_slot);
    /* host: only the remote puppet's kills are rolled back (the host's own and the environment's stay); client: every report that is not the local human's (replicas die by the host's events) */
    int roll = active && !mine && (cs_is_host(g_cs) ? theirs : 1);
    static unsigned char snap[STAT_GLOBAL_LEN + STAT_SLOT_LEN + STAT_HUMAN_LEN];
    if (roll) {
        memcpy(snap, (void *)STAT_GLOBAL(st), STAT_GLOBAL_LEN);
        memcpy(snap + STAT_GLOBAL_LEN, (void *)STAT_SLOT(st), STAT_SLOT_LEN);
        memcpy(snap + STAT_GLOBAL_LEN + STAT_SLOT_LEN, (void *)STAT_HUMAN(st), STAT_HUMAN_LEN);
    }
    o_killrep(story_p, enemy, di, maxhp);
    if (roll) {
        int changed = memcmp(snap, (void *)STAT_GLOBAL(st), STAT_GLOBAL_LEN) || memcmp(snap + STAT_GLOBAL_LEN, (void *)STAT_SLOT(st), STAT_SLOT_LEN) ||
                      memcmp(snap + STAT_GLOBAL_LEN + STAT_SLOT_LEN, (void *)STAT_HUMAN(st), STAT_HUMAN_LEN);
        memcpy((void *)STAT_GLOBAL(st), snap, STAT_GLOBAL_LEN);
        memcpy((void *)STAT_SLOT(st), snap + STAT_GLOBAL_LEN, STAT_SLOT_LEN);
        memcpy((void *)STAT_HUMAN(st), snap + STAT_GLOBAL_LEN + STAT_SLOT_LEN, STAT_HUMAN_LEN);
        if (changed && ++g_w_stat_rolled <= 40) r2_log(W_LOG "kill report of enemy %08x by %s (attacker %08x): stat tables rolled back, the kill is not this profile's", enemy, theirs ? "the partner's puppet" : "someone else", att);
    } else if (active && mine) {
        if (++g_w_stat_kept <= 40) r2_log(W_LOG "kill report of enemy %08x by the local human (attacker %08x): credited", enemy, att);
    }
}

#include "story.inc"
#include "door.inc"
#include "scen.inc"

/* Stage 6i: until now the story sync of stage 6 ran only in lab sessions that set REV2_WORLD=1 and typed `net w sf sync 1` / `net w doors coop 2`: a player who starts a co-op game from the menu got puppets and nothing else (no
 * enemy replicas, no scenario flag sync, no co-op door replay, no kill-credit guard). When a session starts (or this PC joins a running one) the three switches are the defaults. A lab run that wants its own set-up exports
 * REV2_WORLD_DEFAULTS=0 (or `net w defaults 0` before the start). Explicit values typed earlier are kept (sync > 0, coop > 0). */
static void session_defaults(void)
{
    if (g_defaults_off) return;
    if (!g_w_on) {
        if (w_install()) {
            g_w_on = 1;
            r2_log(W_LOG "session start: world sync switched on by default (hooks installed)");
        } else
            r2_log(W_LOG "session start: the world hooks could not be installed (game not ready or bytes differ): no enemy / story sync");
    }
    if (!g_sf_sync) g_sf_sync = 1, g_sf_init = 0, r2_log(W_LOG "session start: scenario flag sync 1 by default");
    if (!g_w_sm_coop) g_w_sm_coop = 2, r2_log(W_LOG "session start: co-op door replay (mode 2) by default");
}

static void w_announce_spawn(unsigned obj, const layout_t *l)
{
    char cn[100];
    w_class(obj, cn, sizeof cn);
    unsigned char b[32];
    memcpy(b, &l->a, 4), memcpy(b + 4, &l->b, 2), memcpy(b + 6, &l->c, 2);
    float pos[3] = {rdf(obj + 0x40), rdf(obj + 0x44), rdf(obj + 0x48)};
    memcpy(b + 8, pos, 12);
    unsigned hp = rd32(obj + 0x1A08);
    memcpy(b + 20, &hp, 4);
    w_ev_send(EV_W_SPAWN, b, 24);
    r2_log(W_LOG "SPAWN sent: %s layout (%d,%u,%u) at (%.0f,%.0f,%.0f) hp %u", cn, l->a, l->b, l->c, pos[0], pos[1], pos[2], hp);
}

static __attribute__((thiscall)) void *h_create(void *self, unsigned vtbl, int a, unsigned bc, unsigned p5, unsigned p6)
{
    const void *ret = __builtin_return_address(0);
    g_w_n_create++;
    int client = g_w_on && g_cs && cs_active(g_cs) && !cs_is_host(g_cs);
    if (client && g_w_guard && !g_w_from_host) {   /* the host decides when an enemy appears ... */
        if (!g_w_demand_on || !glue_demand_check(g_w_demand, a, bc, GetTickCount())) {
            g_w_refused++;
            if (g_w_refused <= (unsigned)g_w_log_calls) r2_log(W_LOG "CreateEnemy(layout (%d,%u,%u)) refused on the client (the host's SPAWN creates it), caller exe+%x", a, bc & 0xFFFF, bc >> 16, (unsigned)ret - 0x400000);
            return NULL;
        }
        /* ... unless the story keeps asking for it (6f, glue_logic.h): it waits for the enemy (the opening scene of Claire's half) and the host never made one. Created here; a host record for the same layout id finds it. */
        g_w_demand_allowed++;
        r2_log(W_LOG "CreateEnemy(layout (%d,%u,%u)): the client's story asks again and again (%u requests refused so far): created on this PC, caller exe+%x", a, bc & 0xFFFF, bc >> 16, g_w_refused, (unsigned)ret - 0x400000);
    }
    void *r = o_create(self, vtbl, a, bc, p5, p6);
    if (g_w_n_create <= (unsigned)g_w_log_calls) {
        char cn[100] = "-";
        if (r) w_class((unsigned)r, cn, sizeof cn);
        r2_log(W_LOG "CreateEnemy(layout (%d,%u,%u), flags %u,%u) -> %p %s, caller exe+%x%s", a, bc & 0xFFFF, bc >> 16, p5, p6, r, cn, (unsigned)ret - 0x400000, g_w_from_host ? " [from host]" : "");
    }
    if (r && g_w_on && !g_w_from_host && g_cs && cs_active(g_cs) && cs_is_host(g_cs)) {
        layout_t l = {a, bc & 0xFFFF, bc >> 16};
        w_announce_spawn((unsigned)r, &l);
    }
    return r;
}

static __attribute__((thiscall)) void h_damage(void *self, void *dmg, int amount)
{
    unsigned obj = (unsigned)self, di = (unsigned)dmg;
    unsigned attacker = di ? rd32(di + 0xDC) : 0;
    int aslot = w_slot_of_player(attacker);
    g_w_n_damage++;
    layout_t l = {0, 0, 0};
    layout_of(obj, &l);
    int client = g_w_on && g_cs && cs_active(g_cs) && !cs_is_host(g_cs);
    if (g_w_n_damage <= (unsigned)g_w_log_calls) {
        char cn[100];
        r2_log(W_LOG "ApplyDamage(%s %08x layout (%d,%u,%u) hp %d, damage %d, attacker %08x slot %d, di flags %08x %08x)%s", w_class(obj, cn, sizeof cn), obj, l.a, l.b, l.c, (int)rd32(obj + 0x1A08), amount, attacker, aslot,
               di ? rd32(di + 0x28) : 0, di ? rd32(di + 0x2C) : 0, client ? " [client: forwarded]" : "");
    }
    if (client && !g_w_replay && aslot == g_my_slot) {   /* my hit on a replica: the host's enemy takes it */
        unsigned char b[24];
        memcpy(b, &l.a, 4), memcpy(b + 4, &l.b, 2), memcpy(b + 6, &l.c, 2);
        memcpy(b + 8, &amount, 4);
        b[12] = (unsigned char)aslot;
        unsigned fl = di ? rd32(di + 0x28) : 0, fl2 = di ? rd32(di + 0x2C) : 0;
        memcpy(b + 13, &fl, 4), memcpy(b + 17, &fl2, 4);
        w_ev_send(EV_W_HIT, b, 21);
        return;   /* the replica's own HP comes from the host */
    }
    int hp0 = (int)rd32(obj + 0x1A08);
    damage_t od = w_orig_for(obj);
    if (!od) return;
    story_note_hit(obj, aslot, di ? rd32(di + 0x28) : 0, di ? rd32(di + 0x2C) : 0);
    od(self, dmg, amount);
    int hp1 = (int)rd32(obj + 0x1A08);
    if (g_w_on && g_cs && cs_active(g_cs) && cs_is_host(g_cs) && hp0 > 0 && hp1 <= 0) {   /* the host's enemy died (also when the killing blow was a client's HIT replayed here) */
        wdeath_t *dd = wdeath_slot(obj);
        if (dd) dd->sent = 1;   /* the per-pass scan must not send it a second time */
        unsigned char b[24];
        memcpy(b, &l.a, 4), memcpy(b + 4, &l.b, 2), memcpy(b + 6, &l.c, 2);
        b[8] = (unsigned char)(aslot < 0 ? 255 : aslot);
        unsigned act = rd32(obj + 0x1A54), f28 = di ? rd32(di + 0x28) : 0, f2c = di ? rd32(di + 0x2C) : 0;
        memcpy(b + 9, &act, 4), memcpy(b + 13, &f28, 4), memcpy(b + 17, &f2c, 4);
        w_ev_send(EV_W_DEATH, b, 21);
        r2_log(W_LOG "DEATH sent: layout (%d,%u,%u), killer slot %d, action %u", l.a, l.b, l.c, aslot, act);
    }
}

static int w_install(void)
{
    if (g_w_installed) return 1;
    if (!r2_text_decrypted()) return 0;
    static const unsigned char b_add[] = {0x83, 0xEC, 0x24, 0x53, 0x56};
    static const unsigned char b_create[] = {0x83, 0xEC, 0x0C, 0x53, 0x55};
    void *t;
    int ok = 0;
    if ((t = w_tramp(0x6EA6A0, b_add, 5))) o_additem = (additem_t)t, ok += w_jmp(0x6EA6A0, 5, (void *)h_additem);
    if ((t = w_tramp(0x5FD7C0, b_create, 5))) o_create = (create_t)t, ok += w_jmp(0x5FD7C0, 5, (void *)h_create);
    static const unsigned char b_psub[] = {0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x74, 0x6F, 0x00, 0x00};
    if ((t = w_tramp(0x715020, b_psub, 9))) o_psub = (psub_t)t, ok += w_jmp(0x715020, 9, (void *)h_psub);
    static const unsigned char b_flag[] = {0x8B, 0x54, 0x24, 0x04, 0x57};
    if ((t = w_tramp(0x97F170, b_flag, 5))) o_flagset = (flagset_t)t, ok += w_jmp(0x97F170, 5, (void *)h_flagset);
    static const unsigned char b_intel[] = {0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x4E, 0x30};   /* push esi; mov esi, ecx; push edi; mov ecx, [esi+0x30] */
    if ((t = w_tramp(0x600F50, b_intel, 7))) o_intel_upd = (intel_upd_t)t, ok += w_jmp(0x600F50, 7, (void *)h_intel_update);
    static const unsigned char b_killrep[] = {0x83, 0xEC, 0x10, 0x53, 0x56};   /* sub esp, 0x10; push ebx; push esi */
    if ((t = w_tramp(0x986230, b_killrep, 5))) o_killrep = (killrep_t)t, ok += w_jmp(0x986230, 5, (void *)h_killrep);
    story_install(&ok);
    scen_install(&ok);
    for (int i = 0; i < W_NVT; i++) {   /* slot 234 of every enemy class vtable (checked against the value read from the image) */
        unsigned slot = w_vts[i].vt + 4 * 234, cur = 0;
        if (!r2_safe_read((void *)slot, &cur, 4) || cur != w_vts[i].orig) {
            r2_log(W_LOG "vtable %08x slot 234 is %08x, expected %08x: not patched", w_vts[i].vt, cur, w_vts[i].orig);
            continue;
        }
        DWORD old;
        if (VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old)) {
            *(volatile unsigned *)slot = (unsigned)h_damage;
            VirtualProtect((void *)slot, 4, old, &old);
            g_w_vt_patched++;
        }
    }
    ok += g_w_vt_patched > 0;
    g_w_installed = 1;
    r2_log(W_LOG "hooks installed: AddItem %s, CreateEnemy %s, player SubHP %s, kill report %s, ApplyDamage in %d of %d enemy vtables", o_additem ? "yes" : "NO", o_create ? "yes" : "NO", o_psub ? "yes" : "NO", o_killrep ? "yes" : "NO",
           g_w_vt_patched, W_NVT);
    return ok > 0;
}

/* ---------------------------------------------------------------- diagnostics */
static void w_obj_line(unsigned o, char *out, int size, int *pos)
{
    char cn[100];
    layout_t l;
    layout_of(o, &l);
    *pos += snprintf(out + *pos, size - *pos, "  %08x %-12s layout (%d,%u,%u) pos (%.0f,%.0f,%.0f) hp %d/%d act %d/%d pstate %08x type7920 %u intel %08x flags6f4c %08x\n", o, w_class(o, cn, sizeof cn), l.a, l.b, l.c, rdf(o + 0x40),
                     rdf(o + 0x44), rdf(o + 0x48), (int)rd32(o + 0x1A08), (int)rd32(o + 0x1A0C), (int)rd32(o + 0x1A54), (int)rd32(o + 0x1A58), rd32(o + 0x37B8), rd32(o + 0x7920), rd32(o + 0x6F10), rd32(o + 0x6F4C));
}

/* the two object lists of a manager (sEnemy / sItem): count at +0x30 with the pointer array at +0x3C, count at +0x44 with the array at +0x50 */
static void w_list_manager(const char *label, unsigned cell, char *out, int size, int *pos)
{
    unsigned m = rd32(cell);
    *pos += snprintf(out + *pos, size - *pos, "%s %08x:", label, m);
    if (!m) {
        *pos += snprintf(out + *pos, size - *pos, " none\n");
        return;
    }
    unsigned c1 = rd32(m + 0x30), a1 = rd32(m + 0x3C), c2 = rd32(m + 0x44), a2 = rd32(m + 0x50);
    *pos += snprintf(out + *pos, size - *pos, " list1 %u (array %08x), list2 %u (array %08x)\n", c1, a1, c2, a2);
    for (int k = 0; k < 2; k++) {
        unsigned c = k ? c2 : c1, a = k ? a2 : a1;
        for (unsigned i = 0; i < c && i < 40 && a; i++) {
            unsigned o = rd32(a + 4 * i);
            if (o && *pos < size - 400) w_obj_line(o, out, size, pos);
        }
    }
}

static void w_status_ext(char *out, int size);
static void w_status(char *out, int size)
{
    int o = snprintf(out, size, "world: hooks %s, sync %s, guard %d; calls: AddItem %u (remote replays %u), CreateEnemy %u (refused %u), ApplyDamage %u\n", g_w_installed ? "installed" : "not installed", g_w_on ? "ON" : "off", g_w_guard,
                     g_w_n_additem, g_w_n_remote_additem, g_w_n_create, g_w_refused, g_w_n_damage);
    w_list_manager("sEnemy", SENEMY_CELL, out, size, &o);
    w_list_manager("sItem", SITEM_CELL, out, size, &o);
    if (o < size - 300) w_status_ext(out + o, size - o);
}

static unsigned w_find_active(unsigned cell, int a, unsigned b, unsigned c);
static unsigned g_w_kill_reports;
static int g_w_autoheal;
static unsigned g_w_healed;
/* console: net w on|off|observe|guard 0|1|log N|status|obj <hex>|spawn <a> <b> <c>|hit <a> <b> <c> [damage]|give <hex item> [count] */
static void w_command(const char *rest, char *out, int size)
{
    char sub[16] = "";
    sscanf(rest, "%15s", sub);
    const char *arg = rest + strlen(sub);
    while (*arg == ' ') arg++;
    if (!strcmp(sub, "on")) {
        if (!w_install()) {
            snprintf(out, size, "world: hooks could not be installed (game not ready or bytes differ), see the log");
            return;
        }
        g_w_on = 1;
        snprintf(out, size, "world sync ON (hooks %s)", g_w_installed ? "installed" : "NOT installed");
    } else if (!strcmp(sub, "defaults")) {   /* net w defaults [0|1]: the session-start defaults (6i) */
        if (arg[0]) g_defaults_off = atoi(arg) == 0;
        snprintf(out, size, "session-start defaults %s (world sync on, scenario flag sync 1, co-op door replay 2); now: world %s, sf sync %d, doors coop %d", g_defaults_off ? "OFF" : "on", g_w_on ? "ON" : "off", g_sf_sync, g_w_sm_coop);
    } else if (!strcmp(sub, "off")) {
        g_w_on = 0;
        snprintf(out, size, "world sync off (hooks stay, log only)");
    } else if (!strcmp(sub, "observe")) {   /* hooks installed, nothing sent or refused */
        snprintf(out, size, w_install() ? "world: hooks installed, observing (sync %s)" : "world: hooks could not be installed", g_w_on ? "ON" : "off");
    } else if (!strcmp(sub, "errreset")) {
        g_w_err_sum = g_w_err_max = 0, g_w_err_n = 0;
        snprintf(out, size, "replica error statistics cleared");
    } else if (!strcmp(sub, "pdmg")) {
        if (arg[0]) g_w_pdmg_mode = atoi(arg) != 0;
        snprintf(out, size, "damage to the puppet: %s", g_w_pdmg_mode ? "forwarded to its owner (PDMG)" : "not forwarded (the owner's replica enemy hurts the real body)");
    } else if (!strcmp(sub, "guard")) {
        g_w_guard = atoi(arg) != 0;
        snprintf(out, size, "client creator guard %d", g_w_guard);
    } else if (!strcmp(sub, "grab")) {   /* net w grab [0|1] | free [0|1]: a replica that starts a grab action attaches it to the victim the record names (6f); free = a replica inside the grab chain is not re-forced (6g) */
        if (!strncmp(arg, "free", 4)) {
            const char *v = arg + 4;
            while (*v == ' ') v++;
            if (*v) g_w_replica_free = atoi(v) != 0;
            snprintf(out, size, "replica grab chain: %s (%u forced actions skipped because the replica was already inside the chain)", g_w_replica_free ? "not re-forced" : "re-forced as in 6f", g_w_chain_skips);
        } else {
            if (arg[0]) g_w_grab_attach = atoi(arg) != 0;
            snprintf(out, size, "replica grab attach %s (%u attaches, last catch id %08x), chain %s (%u skipped)", g_w_grab_attach ? "on" : "off", g_w_grab_n, g_w_grab_last_tag, g_w_replica_free ? "not re-forced" : "re-forced", g_w_chain_skips);
        }
    } else if (!strcmp(sub, "demand")) {   /* net w demand [0|1]: a client lets a creation through that its story asks for again and again (glue_logic.h, 6f) */
        if (arg[0]) g_w_demand_on = atoi(arg) != 0;
        snprintf(out, size, "story demand escalation %s (%u creations allowed, %u refused so far)", g_w_demand_on ? "on" : "off", g_w_demand_allowed, g_w_refused);
    } else if (!strcmp(sub, "log")) {
        g_w_log_calls = atoi(arg);
        snprintf(out, size, "log the first %d calls of each hook", g_w_log_calls);
    } else if (!strcmp(sub, "obj")) {
        unsigned o = (unsigned)strtoul(arg, NULL, 16), pos = 0;
        if (!o) snprintf(out, size, "usage: net w obj <hex addr>");
        else {
            int p = 0;
            w_obj_line(o, out, size, &p);
            (void)pos;
        }
    } else if (!strcmp(sub, "probe")) {   /* net w probe <hex va> <n> [maxlog] [stack]: log registers / arguments / call sites of a game function (probe.inc); game thread */
        unsigned va = 0, n = 0;
        int ml = 40, dl = 0;
        char st[8] = "", dc[8] = "";
        if (sscanf(arg, "%x %u %d %7s %7s %d", &va, &n, &ml, st, dc, &dl) < 2) {
            snprintf(out, size, "usage: net w probe <hex va> <stolen bytes> [maxlog] [stack|-] [dump arg: c|0..5|-] [dump bytes]");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w probe %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        probe_install(va, n, ml, !strcmp(st, "stack"), dc[0] == 'c' ? 10 : (dc[0] >= '0' && dc[0] <= '5') ? dc[0] - '0' : -1, dl, out, size);
    } else if (!strcmp(sub, "unprobe")) {
        unsigned va = (unsigned)strtoul(arg, NULL, 16);
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w unprobe %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        probe_remove(va, out, size);
    } else if (!strcmp(sub, "flags")) {   /* net w flags [sync 0|1]: the set bits of the story flag table (dword index: hex value) */
        unsigned story = rd32(0x157D120);
        if (!strncmp(arg, "sync", 4)) g_w_flag_sync = atoi(arg + 4) != 0;
        int o = snprintf(out, size, "flag sync %d, tx %u rx %u, writes seen %u; set bits (dword: value):", g_w_flag_sync, g_w_flag_tx, g_w_flag_rx, g_w_flag_n);
        for (unsigned i = 0; story && i < 128 && o < size - 40; i++) {
            unsigned v = rd32(story + 0xEF0 + 4 * i);
            if (v) o += snprintf(out + o, size - o, " %u:%08x", i, v);
        }
    } else if (!strcmp(sub, "story")) {   /* net w story [equip 0|1] [barrier 0|1 [ms]] [grant 0|1]: counters and switches of the 6b hooks */
        int v1 = 0, v2 = 0;
        if (sscanf(arg, "equip %d", &v1) == 1) g_w_equip = v1 != 0;
        else if (sscanf(arg, "barrier %d %d", &v1, &v2) >= 1) {
            g_w_barrier = v1 != 0;
            if (v2 > 0) g_w_barrier_ms = v2;
        } else if (sscanf(arg, "grant %d", &v1) == 1) g_w_grant_sync = v1 != 0;
        else if (sscanf(arg, "scene %d %d", &v1, &v2) >= 1) {
            g_w_scene = v1 != 0;
            if (v2 > 0) g_w_scene_len = (unsigned)v2;
        }
        story_status(out, size);
        int o2 = (int)strlen(out);
        snprintf(out + o2, size - o2, "switches: equip %d barrier %d (%d ms) grant sync %d flag sync %d", g_w_equip, g_w_barrier, g_w_barrier_ms, g_w_grant_sync, g_w_flag_sync);
    } else if (!strcmp(sub, "inv")) {   /* net w inv [slot]: the personal inventory (item id : count) and the equipped weapon slots */
        int sl = arg[0] ? atoi(arg) : g_my_slot, o2 = 0;
        unsigned pi = inv_personal(sl), pl = player_obj(sl);
        o2 += snprintf(out, size, "inventory of slot %d (personal %08x, player %08x):", sl, pi, pl);
        for (unsigned i = 0; pi && i < 0x22 && o2 < size - 40; i++) {
            unsigned e = rd32(pi + 0xC + 4 * i);
            if (e) o2 += snprintf(out + o2, size - o2, " [%u]%04x x%d", i, rd32(e + 4), (int)rd32(e + 0xC));
        }
        if (pl && o2 < size - 80) o2 += snprintf(out + o2, size - o2, "; weapon slots 1..3: %04x %04x %04x, hand slot %u", equipped_item(pl, 1), equipped_item(pl, 2), equipped_item(pl, 3), do_call(3, 0x6FB060, (unsigned[]){pl}, 1) & 0xFF);
    } else if (!strcmp(sub, "cell")) {   /* net w cell <hex vtable>: the global cells (0x1500000..0x1610000) that hold a pointer to an object with this vtable (finds manager singletons) */
        unsigned vt = (unsigned)strtoul(arg, NULL, 16), o2 = 0, n = 0;
        for (unsigned c = 0x1500000; c < 0x1610000 && o2 < (unsigned)size - 80; c += 4) {
            unsigned p = rd32(c), v = 0;
            if (p < 0x10000 || p == vt || !r2_safe_read((void *)p, &v, 4)) continue;
            if (v == vt) o2 += (unsigned)snprintf(out + o2, size - o2, "cell %08x -> %08x\n", c, p), n++;
        }
        if (!n) snprintf(out, size, "no cell holds an object of vtable %08x", vt);
    } else if (!strcmp(sub, "mgr")) {   /* net w mgr <hex cell>: list the objects of a manager singleton (sEnemy 1561E70, sItem 1562E10, sSetModel: see `net w cell 13df950`) */
        unsigned cell = (unsigned)strtoul(arg, NULL, 16);
        int o2 = 0;
        w_list_manager("manager", cell, out, size, &o2);
    } else if (!strcmp(sub, "target")) {   /* net w target <a> <b> <c> [slot]: set the enemy's current target (FUN_0061A520, thiscall on the enemy: +0x7168) to a player, to wake a test enemy */
        int a = 0, sl = g_my_slot;
        unsigned b = 0, c = 0;
        if (sscanf(arg, "%d %u %u %d", &a, &b, &c, &sl) < 3) {
            snprintf(out, size, "usage: net w target <a> <b> <c> [slot]");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w target %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        unsigned en = w_find_active(SENEMY_CELL, a, b, c), pl = player_obj(sl);
        if (!en || !pl) {
            snprintf(out, size, "no enemy (%u) or player (%u)", en, pl);
            return;
        }
        unsigned args[2] = {en, pl};
        do_call(2, 0x61A520, args, 2);
        snprintf(out, size, "enemy %08x target now %08x (slot %d)", en, rd32(en + 0x7168), sl);
    } else if (!strcmp(sub, "act")) {   /* net w act <a> <b> <c> <action id>: ChangeAction (vtable +0xB8) on an enemy, to find its walk / attack ids */
        int a = 0, id = 0;
        unsigned b = 0, c = 0, cellv = SENEMY_CELL;
        if (sscanf(arg, "%d %u %u %d %x", &a, &b, &c, &id, &cellv) < 4) {
            snprintf(out, size, "usage: net w act <a> <b> <c> <action id> [manager cell hex, default sEnemy; sSetModel 1581920]");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w act %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        unsigned en = w_find_active(cellv, a, b, c);
        if (!en) {
            snprintf(out, size, "no object (%d,%u,%u) in manager %x", a, b, c, cellv);
            return;
        }
        if (cellv == SSETMODEL_CELL && !w_sm_class_ok(en) && rd32(en + 0x1A38 + 0xC) == 0) {   /* co-op set model without an actor: its first action update dereferences the actor (hardware run s6c3: host AV 0x74E035) */
            snprintf(out, size, "refused: co-op set model %08x has no actor (a bare ChangeAction crashed the host); use `net w coopact <a> <b> <c> <id> <actor slot>`", en);
            return;
        }
        unsigned fn = rd32(rd32(en) + 0xB8), args[6] = {en, (unsigned)id, 1, 0, 0xFFFFFFFFu, 0};
        do_call(2, fn, args, 6);
        snprintf(out, size, "enemy %08x action now %d", en, (int)rd32(en + 0x1A54));
    } else if (!strcmp(sub, "grabforce")) {   /* net w grabforce <a> <b> <c> <victim slot> [force]: the forced grab of 6f (the enemy's catch id = the victim's, then ChangeAction 77) in ONE call; refused while the enemy is still in the grab chain (stacked forced actions wedged an enemy in 77 for 3 minutes), `force` stacks anyway (wedge test) */
        int a = 0, sl = 0;
        unsigned b = 0, c = 0;
        char fw[12] = "";
        if (sscanf(arg, "%d %u %u %d %11s", &a, &b, &c, &sl, fw) < 4 || sl < 0 || sl > 1) {
            snprintf(out, size, "usage: net w grabforce <a> <b> <c> <victim slot 0|1> [force]");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w grabforce %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        unsigned en = w_find_active(SENEMY_CELL, a, b, c), vic = player_obj(sl);
        if (!en || !vic) {
            snprintf(out, size, "no enemy (%08x) or victim (%08x)", en, vic);
            return;
        }
        unsigned cur = rd32(en + 0x1A54);
        if (glue_grab_family(cur) && strcmp(fw, "force")) {
            snprintf(out, size, "refused: the enemy is in the grab chain (action %u); wait for it to leave (or add 'force' to stack, which can wedge it)", cur);
            return;
        }
        unsigned tag = rd32(en + 0x73E8);
        if (!tag || tag == 0xFFFFFFFFu) {   /* ffffffff is "no catch id": the host's grab start does not take a victim with it (6g hardware: the forced 77 ended in action 16 without a hold) */
            tag = 0x600D0000u | (++g_w_grab_n & 0xFFFF);
            wr(en + 0x73E8, &tag, 4);
        }
        wr(vic + 0x3788, &tag, 4);
        unsigned fn = rd32(rd32(en) + 0xB8), args[6] = {en, 77, 1, 0, 0xFFFFFFFFu, 0};
        do_call(2, fn, args, 6);
        snprintf(out, size, "enemy %08x forced into the grab of slot %d (catch id %08x), action now %d, victim action %d", en, sl, tag, (int)rd32(en + 0x1A54), (int)rd32(vic + 0x1A54));
    } else if (!strcmp(sub, "coopact")) {   /* net w coopact <a> <b> <c> <id> <actor slot>: ChangeAction on a co-op set model with that slot's player as the action's actor (+0x1A38+0xC): the console version of the DOOR coop replay */
        int a = 0, id = 0, sl = 0;
        unsigned b = 0, c = 0;
        if (sscanf(arg, "%d %u %u %d %d", &a, &b, &c, &id, &sl) < 5 || sl < 0 || sl > 1) {
            snprintf(out, size, "usage: net w coopact <a> <b> <c> <action id> <actor slot 0|1>");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w coopact %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        unsigned so = w_find_active(SSETMODEL_CELL, a, b, c), actor = player_obj(sl), action = so + 0x1A38;
        if (!so || !actor || rd32(action + 0x20) != so) {
            snprintf(out, size, "not done: set model %08x, actor %08x, action owner %08x", so, actor, so ? rd32(action + 0x20) : 0);
            return;
        }
        wr(action + 0xC, &actor, 4);
        unsigned fn = rd32(rd32(so) + 0xB8), args[6] = {so, (unsigned)id, 1, 0, 0xFFFFFFFFu, 0};
        do_call(2, fn, args, 6);
        snprintf(out, size, "co-op set model %08x action now %d (actor slot %d)", so, (int)rd32(so + 0x1A54), sl);
    } else if (!strcmp(sub, "place")) {   /* net w place <area> [class substring] [from b]: the placements (cSetInfo* records of the level layout) of one area id: layout (area,b,c), class, position; brute force over b 0..255, c 0..7 through FUN_00A946A0 on the sEnemy manager */
        int area = 0, from = 0;
        char flt[40] = "Em";
        sscanf(arg, "%d %39s %d", &area, flt, &from);
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w place %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        unsigned se = rd32(SENEMY_CELL);
        if (!se) {
            snprintf(out, size, "no sEnemy");
            return;
        }
        int o = 0, n = 0;
        for (unsigned b = (unsigned)from; b < 256 && o < size - 200; b++)
            for (unsigned c = 0; c < 8 && o < size - 200; c++) {
                unsigned args[4] = {se, LAYOUT_VTBL, (unsigned)area, b | (c << 16)};
                unsigned rec = do_call(2, 0xA946A0, args, 4);
                if (!rec || rec < 0x10000) continue;
                unsigned si = rd32(rec + 0x10);
                if (!si) continue;
                char cn[300] = "";
                classes_name(si, cn, sizeof cn - 1);
                char *nm = strstr(cn, ": ");
                nm = nm ? nm + 2 : cn;
                char *sp = strchr(nm, ' ');
                if (sp) *sp = 0;
                if (flt[0] != '-' && !strstr(nm, flt)) continue;
                o += snprintf(out + o, size - o, "(%d,%u,%u) %s si %08x pos (%.0f,%.0f,%.0f)\n", area, b, c, nm, si, rdf(si + 0x50), rdf(si + 0x54), rdf(si + 0x58));
                n++;
            }
        snprintf(out + o, size - o, "%d placements of area %d matching '%s' (from b=%d)", n, area, flt, from);
    } else if (!strcmp(sub, "kills") || !strcmp(sub, "stats")) {   /* net w kills|stats [ach]: the kill ledgers the game credits (Result_OnEnemyKilled_A/B). The counter functions are thiscalls on `story + 0x31320` (the caller does `lea ecx, [esi+0x31320]`,
                                           * hardware run s6d1), so the ledgers are at story+0x31320+0x118D38 (kills of a HUMAN slot by kind, FUN_00972ED0) and story+0x31320+0x10F0D8 (per slot, FUN_00972E90, 0x66 words per slot);
                                           * the global stat (FUN_00979580, thiscall on story+0x20) is the u32 table at story+0x20+0x1BC */
        unsigned story = rd32(0x157D120);
        int o = 0;
        if (!story) {
            snprintf(out, size, "no story block");
            return;
        }
        o += snprintf(out + o, size - o, "kills credited to human slots, by kind:");
        for (unsigned k = 0; k < 0x51 && o < size - 40; k++) {
            unsigned short v = 0;
            r2_safe_read((void *)(STAT_HUMAN(story) + 2 * k), &v, 2);
            if (v) o += snprintf(out + o, size - o, " %02x:%u", k, v);
        }
        for (unsigned sl = 0; sl < 2 && o < size - 80; sl++) {
            o += snprintf(out + o, size - o, "\nper slot %u:", sl);
            for (unsigned k = 0; k < 0x51 && o < size - 40; k++) {
                unsigned short v = 0;
                r2_safe_read((void *)(STAT_SLOT(story) + 2 * (sl * 0x66 + k)), &v, 2);
                if (v) o += snprintf(out + o, size - o, " %02x:%u", k, v);
            }
        }
        o += snprintf(out + o, size - o, "\nglobal total by kind (what the records compare against):");
        for (unsigned k = 0; k < 0x51 && o < size - 40; k++) {
            unsigned v = rd32(STAT_GLOBAL(story) + 4 * k);
            if (v) o += snprintf(out + o, size - o, " %02x:%u", k, v);
        }
        o += snprintf(out + o, size - o, "\nstat guard %s, rolled back %u, local kills kept %u, kill reports of the replica path %u", g_w_stat_guard ? "on" : "off", g_w_stat_rolled, g_w_stat_kept, g_w_kill_reports);
        if (!strncmp(arg, "ach", 3) || !strncmp(sub, "stats", 5)) {   /* the kill records: state bits (story+0x304+id*8: bit 0..2), goal (sBioAchievement record counter), total of the kind it counts */
            static const struct { unsigned rec, kind; const char *name; } kr[] = {{0x4F, 0x0F, "PROGRESSOR"}, {0x54, 0x14, "HOMUNCULUS"}, {0x57, 0x18, "GHOST"}, {0x64, 0x26, "KNIFE"}, {0x65, 0x28, "BLOW"},
                                                                                {0x66, 0x2A, "GRAPPLE"}, {0x67, 0x2B, "BACK"}, {0x6A, 0x30, "LIGHT"}, {0x6C, 0x33, "BAR"}, {0x6E, 0x35, "STONE"}, {0x78, 0x38, "HOMU_WEAPON"},
                                                                                {0x79, 0x39, "JUMPATK"}, {0x7D, 0x37, "ASSIST"}};
            unsigned ach = rd32(0x1552728);
            o += snprintf(out + o, size - o, "\nkill records (record: state bits, goal, total of kind):");
            for (unsigned i = 0; i < sizeof kr / sizeof kr[0] && o < size - 80; i++) {
                unsigned bits = rd32(story + 0x304 + kr[i].rec * 8) & 7, goal = 0;
                if (ach) {
                    unsigned ga[2] = {ach, kr[i].rec};
                    goal = do_call(2, 0x409DF0, ga, 2);
                }
                o += snprintf(out + o, size - o, "\n  %02x %-11s bits %u goal %u total %u", kr[i].rec, kr[i].name, bits, goal, rd32(STAT_GLOBAL(story) + 4 * kr[i].kind));
            }
        }
    } else if (!strcmp(sub, "autoheal")) {   /* net w autoheal <hp>|0: test aid, tops the local human up to its maximum whenever its health falls below <hp> */
        if (arg[0]) g_w_autoheal = atoi(arg);
        snprintf(out, size, "autoheal %d (below this health the local human is restored to its maximum; %u restores so far)", g_w_autoheal, g_w_healed);
    } else if (!strcmp(sub, "statguard")) {
        if (arg[0]) g_w_stat_guard = atoi(arg) != 0;
        snprintf(out, size, "stat guard %d (1: a kill report of someone else's kill is rolled back on this PC)", g_w_stat_guard);
    } else if (!strcmp(sub, "killrep")) {   /* net w killrep <a> <b> <c> <slot>: run the game's kill report for a live enemy with the killer = the player object of <slot> (a test of the stat guard and the credit path) */
        int a = 0, sl = 0;
        unsigned b = 0, c = 0;
        if (sscanf(arg, "%d %u %u %d", &a, &b, &c, &sl) < 4 || sl < 0 || sl > 1) {
            snprintf(out, size, "usage: net w killrep <a> <b> <c> <killer slot 0|1>");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w killrep %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        unsigned en = w_find_active(SENEMY_CELL, a, b, c), story = rd32(0x157D120), att = player_obj(sl);
        if (!en || !story || !att) {
            snprintf(out, size, "no such enemy / story / player (enemy %08x story %08x player %08x)", en, story, att);
            return;
        }
        static unsigned char di[0x280];
        memset(di, 0, sizeof di);
        memcpy(di + 0xDC, &att, 4);
        unsigned g0 = 0, g1 = 0, ka[4] = {story, en, (unsigned)(unsigned long)di, rd32(en + 0x1A0C)};
        for (unsigned k = 0; k < 0x51; k++) g0 += rd32(STAT_GLOBAL(story) + 4 * k);
        for (unsigned k = 0; k < 0x51; k++) g1 += *(volatile unsigned short *)(STAT_HUMAN(story) + 2 * k);
        unsigned flag = rd32(en + 0x39D8) & 0xFF;
        g_w_replay++;
        do_call(2, 0x986230, ka, 4);
        g_w_replay--;
        unsigned g2 = 0, g3 = 0;
        for (unsigned k = 0; k < 0x51; k++) g2 += rd32(STAT_GLOBAL(story) + 4 * k);
        for (unsigned k = 0; k < 0x51; k++) g3 += *(volatile unsigned short *)(STAT_HUMAN(story) + 2 * k);
        snprintf(out, size, "kill report run for enemy %08x with killer slot %d (+0x39D8 was %u, +0x37B8 %d): global total sum %u -> %u, human ledger sum %u -> %u, guard %s", en, sl, flag, (int)rd32(en + 0x37B8), g0, g2, g1, g3,
                 g_w_stat_guard ? "on" : "off");
    } else if (!strcmp(sub, "placesum")) {   /* net w placesum <from area> <to area>: per area, the number of placements of every enemy class (summary of `net w place`) */
        int a0 = 0, a1 = 0;
        if (sscanf(arg, "%d %d", &a0, &a1) < 2 || a1 < a0 || a1 - a0 > 400) {
            snprintf(out, size, "usage: net w placesum <from area> <to area> (at most 400 areas)");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w placesum %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        unsigned se = rd32(SENEMY_CELL);
        if (!se) {
            snprintf(out, size, "no sEnemy");
            return;
        }
        int o = 0;
        for (int area = a0; area <= a1 && o < size - 300; area++) {
            char names[8][24];
            int cnt[8], nn = 0;
            for (unsigned b = 0; b < 256; b++)
                for (unsigned c = 0; c < 8; c++) {
                    unsigned args[4] = {se, LAYOUT_VTBL, (unsigned)area, b | (c << 16)};
                    unsigned rec = do_call(2, 0xA946A0, args, 4);
                    if (!rec || rec < 0x10000) continue;
                    unsigned si = rd32(rec + 0x10);
                    if (!si) continue;
                    char cn[300] = "";
                    classes_name(si, cn, sizeof cn - 1);
                    char *nm = strstr(cn, ": ");
                    nm = nm ? nm + 2 : cn;
                    char *sp = strchr(nm, '(');
                    if (sp) *sp = 0;
                    if (!strstr(nm, "cSetInfoEm")) continue;
                    int k = 0;
                    for (; k < nn; k++)
                        if (!strncmp(names[k], nm, 23)) break;
                    if (k == nn && nn < 8) snprintf(names[nn], 24, "%s", nm), cnt[nn++] = 0;
                    if (k < 8) cnt[k]++;
                }
            if (nn) {
                o += snprintf(out + o, size - o, "area %d:", area);
                for (int k = 0; k < nn; k++) o += snprintf(out + o, size - o, " %s x%d", names[k] + 8, cnt[k]);
                o += snprintf(out + o, size - o, "\n");
            }
        }
        snprintf(out + o, size - o, "scan %d..%d done", a0, a1);
    } else if (!strcmp(sub, "trace")) {   /* net w trace <file>|off */
        if (g_w_trace) fclose(g_w_trace), g_w_trace = NULL;
        if (arg[0] && strcmp(arg, "off")) {
            g_w_trace = fopen(arg, "w");
            memset(g_w_trace_seen, 0, sizeof g_w_trace_seen);
            if (g_w_trace) fprintf(g_w_trace, "# E,ms,layout,class,x,y,z,hp,act,prev,target slot,pstate | H,ms,slot,type,x,y,z,hp,act,pstate,ctrl,flags7900 | G,ms,object (left the list)\n");
        }
        snprintf(out, size, g_w_trace ? "trace on: %s" : "trace off", arg);
    } else if (!strcmp(sub, "ai")) {   /* net w ai [0|1]: 0 = replicas on a client do not think (default), 1 = their own AI runs as well */
        if (arg[0]) g_w_replica_ai = atoi(arg) != 0;
        snprintf(out, size, "replica AI %s (Intel update hook %s, updates skipped %u)", g_w_replica_ai ? "RUNNING (as before)" : "off on clients", o_intel_upd ? "installed" : "NOT installed", g_w_ai_skipped);
    } else if (!strcmp(sub, "equip")) {   /* net w equip <hex item> [weapon slot type 1..3 = 2] [slot = my slot]: put an item of the personal inventory into the hand (FUN_00710360), e.g. Barry's knife 14b */
        unsigned item = 0, type = 2;
        int sl = g_my_slot;
        if (sscanf(arg, "%x %u %d", &item, &type, &sl) < 1) {
            snprintf(out, size, "usage: net w equip <hex item id> [type 1..3] [slot]");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w equip %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        unsigned p = player_obj(sl);
        int idx = inv_find(sl, item);
        if (!p || idx < 0) {
            snprintf(out, size, "slot %d does not hold item %04x (player %08x)", sl, item, p);
            return;
        }
        story_equip(p, idx, (int)type, 1);
        unsigned a1[1] = {p};
        snprintf(out, size, "equipped item %04x (inventory index %d) as weapon slot %u; weapon slots now %04x %04x %04x, hand slot %u", item, idx, type, equipped_item(p, 1), equipped_item(p, 2), equipped_item(p, 3),
                 do_call(3, 0x6FB060, a1, 1) & 0xFF);
    } else if (!strcmp(sub, "doors")) {   /* net w doors [on|off|list|log N]: set model action mirror (door.inc) */
        w_sm_command(arg, out, size);
    } else if (!strcmp(sub, "holdact")) {   /* net w holdact [id]: the action the puppet plays while its owner is held by an enemy (0 = none), see coop_game.inc */
        if (arg[0]) g_hold_act = atoi(arg);
        snprintf(out, size, "hold pose action %d (applied %u times)", g_hold_act, g_hold_applied);
    } else if (!strcmp(sub, "sf")) {   /* net w sf [get n | set n v | watch 0|1 | watch log N]: scenario flags of the story FSM (scen.inc) */
        scen_command(arg, out, size);
    } else if (!strcmp(sub, "radio")) {   /* net w radio [0|1]: the end-of-half trigger (scen.inc, 6m) */
        char t[48];
        snprintf(t, sizeof t, "radio %s", arg);
        scen_command(t, out, size);
    } else if (!strcmp(sub, "stages")) {   /* net w stages [all]: the story's stage transition table of the current episode (scen.inc, 6h) */
        scen_stages_command(arg, out, size);
    } else if (!strcmp(sub, "bgl")) {   /* net w bgl [req <stage> <block> [ep]]: the background loader that streams the next stage (scen.inc, 6j) */
        scen_bgl_command(arg, out, size);
    } else if (!strcmp(sub, "zonetest")) {   /* net w zonetest [slot]: which enabled trigger zones the game's own shape test puts the humans of this PC in (scen.inc, 6j) */
        scen_zonetest_command(arg, out, size);
    } else if (!strcmp(sub, "smtrace")) {   /* net w smtrace <a> <b> <c> [lines] | off: per-frame change log of one set model and both humans' actions (door.inc, 6f) */
        smtrace_command(arg, out, size);
    } else if (!strcmp(sub, "door")) {   /* net w door <a> <b> <c> [op <acttype>] [act <id>]: one set model's state / the FSM's door operation (scen.inc) */
        scen_door_command(arg, out, size);
    } else if (!strcmp(sub, "probes")) {
        probe_list(out, size);
    } else if (!strcmp(sub, "spawn")) {   /* test: create an enemy by layout id on this PC (game thread) */
        int a = 0;
        unsigned b = 0, c = 0;
        if (sscanf(arg, "%d %u %u", &a, &b, &c) < 3) {
            snprintf(out, size, "usage: net w spawn <a> <b> <c>");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w spawn %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        unsigned se = rd32(SENEMY_CELL);
        if (!se) {
            snprintf(out, size, "no sEnemy");
            return;
        }
        g_w_from_host++;
        unsigned args[6] = {se, LAYOUT_VTBL, (unsigned)a, b | (c << 16), 0, 0};
        unsigned r = do_call(2, 0x5FD780, args, 6);
        g_w_from_host--;
        snprintf(out, size, "CreateEnemy(%d,%u,%u) -> %08x", a, b, c, r);
    } else if (!strcmp(sub, "hit")) {   /* test: a hit of this PC's human on the enemy with that layout id, through the enemy's own virtual (the hook forwards it on a client) */
        int a = 0, amount = 100;
        unsigned b = 0, c = 0;
        if (sscanf(arg, "%d %u %u %d", &a, &b, &c, &amount) < 3) {
            snprintf(out, size, "usage: net w hit <a> <b> <c> [damage]");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w hit %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        unsigned en = w_find_active(SENEMY_CELL, a, b, c);
        if (!en) {
            snprintf(out, size, "no active enemy with layout (%d,%u,%u)", a, b, c);
            return;
        }
        static unsigned char di[0x280];
        memset(di, 0, sizeof di);
        unsigned att = player_obj(g_my_slot);
        memcpy(di + 0xDC, &att, 4);
        unsigned vt = rd32(en), fn = rd32(vt + VTBL_SLOT_APPLYDAMAGE_OFF);
        int hp0 = (int)rd32(en + 0x1A08);
        ((damage_t)fn)((void *)en, di, amount);
        snprintf(out, size, "hit sent through vtable slot 234 (%08x): hp %d -> %d", fn, hp0, (int)rd32(en + 0x1A08));
    } else if (!strcmp(sub, "give")) {   /* test: AddItem for this PC's own slot as a pickup would (object 0): net w give <hex item> [count] */
        unsigned item = 0;
        int count = 1;
        if (sscanf(arg, "%x %d", &item, &count) < 1) {
            snprintf(out, size, "usage: net w give <hex item id> [count]");
            return;
        }
        if (!lt_on_game_thread()) {
            char cmd[120];
            snprintf(cmd, sizeof cmd, "net w give %s", arg);
            lt_post(cmd, out, size);
            return;
        }
        if ((item >> 8) == 1) {   /* weapons need the world's item object: AddItem with a null object crashed the game at 0x40ACE3 (6 and 6e); weapons come from the story / `net w equip` of what the slot holds */
            snprintf(out, size, "refused: %04x is a weapon (AddItem without an item object crashes the game)", item);
            return;
        }
        unsigned p = player_obj(g_my_slot);
        int r = p && g_w_installed ? o_additem(p, item, count, 0, 0) : -1;
        snprintf(out, size, "AddItem(slot %d, %04x, %d) -> %d", g_my_slot, item, count, r);
    } else {
        w_status(out, size);
    }
}

/* ---------------------------------------------------------------- receivers (events of the other PC) */
static unsigned g_w_applied_spawn, g_w_applied_death, g_w_applied_pickup, g_w_applied_hit, g_w_unmatched, g_w_kill_reports;   /* kill reports: the game's kill report run for the local human's kills on replicas */

static unsigned w_find_active(unsigned cell, int a, unsigned b, unsigned c)
{
    unsigned m = rd32(cell);
    if (!m) return 0;
    unsigned args[4] = {m, LAYOUT_VTBL, (unsigned)a, b | (c << 16)};
    return do_call(2, 0xA94550, args, 4);   /* FUN_00A94550: the active object with this LayoutID in the manager's two lists, 0 when none */
}

static void w_on_event(const cp_event *e)
{
    if (!g_w_on) return;
    unsigned char b[CP_EVENT_MAX];
    memcpy(b, e->body, e->len);
    int a = 0;
    unsigned lb = 0, lc = 0;
    if (e->len >= 8) memcpy(&a, b, 4), lb = b[4] | (b[5] << 8), lc = b[6] | (b[7] << 8);
    switch (e->kind) {
    case EV_W_SPAWN:   /* host -> client: create the same enemy */
        if (cs_is_host(g_cs) || e->len < 24) break;
        {
            unsigned se = rd32(SENEMY_CELL);
            if (!se) break;
            if (w_find_active(SENEMY_CELL, a, lb, lc)) break;   /* already there (it came up by itself) */
            g_w_from_host++;
            unsigned args[6] = {se, LAYOUT_VTBL, (unsigned)a, lb | (lc << 16), 0, 0};
            unsigned r = do_call(2, 0x5FD780, args, 6);
            g_w_from_host--;
            g_w_applied_spawn++;
            r2_log(W_LOG "SPAWN applied: layout (%d,%u,%u) -> %08x", a, lb, lc, r);
        }
        break;
    case EV_W_HIT:   /* client -> host: apply the damage to the real enemy */
        if (!cs_is_host(g_cs) || e->len < 21) break;
        {
            unsigned en = w_find_active(SENEMY_CELL, a, lb, lc);
            int dmg;
            memcpy(&dmg, b + 8, 4);
            if (!en || !w_orig_for(en)) {
                g_w_unmatched++;
                break;
            }
            static unsigned char di[0x280];
            memset(di, 0, sizeof di);
            unsigned att = player_obj(b[12] < 2 ? b[12] : g_rem_slot);
            memcpy(di + 0xDC, &att, 4);
            memcpy(di + 0x28, b + 13, 4), memcpy(di + 0x2C, b + 17, 4);
            story_note_hit(en, b[12] < 2 ? b[12] : g_rem_slot, *(unsigned *)(di + 0x28), *(unsigned *)(di + 0x2C));
            g_w_replay++;
            w_orig_for(en)((void *)en, di, dmg);
            g_w_replay--;
            g_w_applied_hit++;
            r2_log(W_LOG "HIT applied on the host: layout (%d,%u,%u) damage %d by slot %d -> hp %d", a, lb, lc, dmg, b[12], (int)rd32(en + 0x1A08));
        }
        break;
    case EV_W_DEATH:   /* host -> client: kill the replica the way a hit would */
        if (cs_is_host(g_cs) || e->len < 13) break;
        {
            unsigned en = w_find_active(SENEMY_CELL, a, lb, lc);
            if (!en || !w_orig_for(en)) {
                g_w_unmatched++;
                break;
            }
            static unsigned char di[0x280];
            memset(di, 0, sizeof di);
            int hp = (int)rd32(en + 0x1A08);
            if (b[8] == g_my_slot) {   /* my kill on the host's enemy: the replica dies by my hand, so the game's own kill evaluators (achievements) credit my human (type 0) on this PC */
                unsigned att = player_obj(g_my_slot);
                memcpy(di + 0xDC, &att, 4);
                if (e->len >= 21) memcpy(di + 0x28, b + 13, 8);   /* the blow type flags of the killing hit (knife, melee, gun...): some achievements count by blow type */
            }
            if (b[8] == g_my_slot) {   /* credit: FUN_00986230(story, enemy, DamageInfo, max hp) is the game's kill report (Result_OnEnemyKilled_A/B, counters, kill message); it ignores an enemy whose +0x37B8 is already negative
                                        * or whose +0x39D8 flag is set, and the real death path sets both, so it must run BEFORE the lethal ApplyDamage (a second call from the real path is then refused by the flag) */
                unsigned story = rd32(0x157D120);
                if (story && !(rd32(en + 0x39D8) & 0xFF) && (int)rd32(en + 0x37B8) >= 0) {
                    unsigned ka[4] = {story, en, (unsigned)(unsigned long)di, rd32(en + 0x1A0C)};
                    g_w_replay++;
                    do_call(2, 0x986230, ka, 4);
                    g_w_replay--;
                    g_w_kill_reports++;
                }
            }
            g_w_replay++;
            w_orig_for(en)((void *)en, di, hp > 0 ? hp : 1);
            g_w_replay--;
            g_w_applied_death++;
            r2_log(W_LOG "DEATH applied: layout (%d,%u,%u) hp %d -> %d", a, lb, lc, hp, (int)rd32(en + 0x1A08));
        }
        break;
    case EV_W_PICKUP:   /* the other human took an item from the world: this PC's copy of the object is consumed through the same call, the item goes into that slot's inventory here */
        if (e->len >= 17) {
            unsigned item, count;
            memcpy(&item, b, 4), memcpy(&count, b + 4, 4);
            int la;
            memcpy(&la, b + 8, 4);
            unsigned lbb = b[12] | (b[13] << 8), lcc = b[14] | (b[15] << 8);
            int slot = b[16];
            if (slot > 1 || slot == g_my_slot) break;
            unsigned obj = w_find_active(SITEM_CELL, la, lbb, lcc);
            g_w_put_rx++;
            if (!obj) {   /* no such object here: it is gone already (the story or this PC's own FSM took it): nothing to do */
                g_w_pick_skip++;
                r2_log(W_LOG "PICKUP skipped: object (%d,%u,%u) is not here (item %04x, slot %d)", la, lbb, lcc, item, slot);
                break;
            }
            int fl = 0;
            if (e->len >= 21) memcpy(&fl, b + 17, 4);
            unsigned pp = player_obj(slot);
            if (!pp || !o_additem) break;
            g_w_replay++;
            int rr = o_additem(pp, item, (int)count, obj, fl);
            g_w_replay--;
            g_w_applied_pickup++;
            r2_log(W_LOG "PICKUP applied: slot %d item %04x count %u, object %08x (layout (%d,%u,%u)) flag %d -> %d, inventory now %d items", slot, item, count, obj, la, lbb, lcc, fl, rr, inv_count(slot));
        }
        break;
    case EV_W_FLAG:   /* the other PC's story flag table changed: same bit here (replay guard: no echo) */
        if (e->len >= 5 && o_flagset && g_w_flag_sync) {
            unsigned id;
            memcpy(&id, b, 4);
            unsigned story = rd32(0x157D120);
            if (story && id < 0x4000 && w_flag_get(story, id) != b[4]) {
                g_w_replay++;
                o_flagset((void *)story, id, b[4]);
                g_w_replay--;
                g_w_flag_rx++;
                if ((int)g_w_flag_rx <= g_w_flag_log) r2_log(W_LOG "FLAG applied: %u (0x%x) = %d", id, id, b[4]);
            }
        }
        break;
    case EV_W_PDMG:   /* the other PC's enemy hit my body (its puppet): apply it to the real one */
        if (e->len >= 5) {
            int amount;
            memcpy(&amount, b + 1, 4);
            unsigned p = player_obj(g_my_slot);
            if (p && b[0] == g_my_slot && o_psub && amount > 0) {
                g_w_replay++;
                o_psub((void *)p, amount);
                g_w_replay--;
                g_w_pdmg_applied++;
                r2_log(W_LOG "PDMG applied: damage %d to my character (slot %d), hp now %d", amount, g_my_slot, (int)rd32(p + 0x1A08));
            }
        }
        break;
    case EV_W_DOOR:   /* a set model's action changed on the other PC (a human opened a door / locker): same action here */
        w_sm_event(e);
        break;
    case EV_W_SF:   /* a scenario flag was set on the other PC (scen.inc, `net w sf sync 1`) */
        scen_event(e);
        break;
    case EV_W_BGL:   /* the other PC's block loader list (scen.inc, 6j) */
        bgl_event(e);
        break;
    default:
        story_on_event(e);
        break;
    }
}

/* ---------------------------------------------------------------- enemy records (WORLD batches, host -> client, ~15 Hz)
 * One record per live enemy of the host (30 bytes): layout a (i16), b, c (u16), position xyz (f32), yaw (i16 rad*10000), hp (i16), action id (u16), previous action (u16), +0x37B8 (u32).
 * The client finds its replica by layout id and drives it as the puppet is driven: target fields +0x39A0.. then the engine's move-over-N-frames helpers, HP from the host,
 * ChangeAction (vtable +0xB8) when the host's action differs. The Raid replication objects (cNetEmNNNN) carry the same fields plus class tails; see stage6-design.md 1.3. */
#define WREC 32
static unsigned g_w_last_send, g_w_tx_batches, g_w_tx_records, g_w_rx_batches, g_w_rx_records, g_w_rx_nomatch, g_w_act_changes, g_w_enemy_snaps;
static int g_w_apply_frames = 4;
static int g_w_apply_act = 1;       /* ChangeAction on the replica */
static int g_w_apply_hp = 1;
static int g_w_apply_target = 1;    /* the replica's current target follows the host's enemy (which human it chases) */
static unsigned g_w_tgt_set;
static int g_w_autospawn = 1;      /* the client creates a replica when records name an enemy it does not have */

static unsigned w_live_enemies(unsigned *out, unsigned max)
{
    unsigned m = rd32(SENEMY_CELL), n = 0;
    if (!m) return 0;
    for (int k = 0; k < 2; k++) {
        unsigned c = rd32(m + (k ? 0x44 : 0x30)), a = rd32(m + (k ? 0x50 : 0x3C));
        for (unsigned i = 0; i < c && i < 64 && a && n < max; i++) {
            unsigned o = rd32(a + 4 * i), dup = 0;
            if (!o) continue;
            for (unsigned j = 0; j < n; j++)
                if (out[j] == o) dup = 1;
            if (!dup) out[n++] = o;
        }
    }
    return n;
}

static void w_send_enemies(void)
{
    unsigned now = GetTickCount();
    if (now - g_w_last_send < 66) return;
    g_w_last_send = now;
    unsigned list[24];
    unsigned n = w_live_enemies(list, 24);
    story_scan_deaths(list, n);
    if (!n) return;
    unsigned char buf[CP_WORLD_MAX];
    unsigned len = 0, recs = 0;
    for (unsigned i = 0; i < n && len + WREC <= CP_WORLD_MAX; i++) {
        unsigned o = list[i];
        layout_t l;
        layout_of(o, &l);
        short a16 = (short)l.a;
        float pos[3] = {rdf(o + 0x40), rdf(o + 0x44), rdf(o + 0x48)};
        short yaw = (short)(player_yaw(o) * 10000.0f), hp = (short)(int)rd32(o + 0x1A08);
        unsigned short act = (unsigned short)rd32(o + 0x1A54), pact = (unsigned short)rd32(o + 0x1A58);
        unsigned ps = rd32(o + 0x37B8);
        unsigned char *p = buf + len;
        memcpy(p, &a16, 2), memcpy(p + 2, &l.b, 2), memcpy(p + 4, &l.c, 2), memcpy(p + 6, pos, 12), memcpy(p + 18, &yaw, 2), memcpy(p + 20, &hp, 2), memcpy(p + 22, &act, 2), memcpy(p + 24, &pact, 2), memcpy(p + 26, &ps, 4);
        int ts = w_slot_of_player(rd32(o + 0x7168));
        p[30] = (unsigned char)(ts < 0 ? 255 : ts), p[31] = 0;   /* which human the host's enemy chases */
        len += WREC, recs++;
    }
    if (recs && cs_send_world(g_cs, WK_ENEMIES, buf, len)) g_w_tx_batches++, g_w_tx_records += recs;
}

/* Stage 6f, grab: the enemy's grab-start action (cEm1100ActGrap, action ids 0x4D..0x58, 0x5A1D80) looks for its victim among the 8 player slots: the player whose `+0x3788` equals the enemy's `+0x73E8`
 * (a catch id both sides got when the enemy's catch collision hit the player), checks hp > 0 and starts cEm1100ActGrapPL on that player. A replica only plays the host's action ids: its catch collision need not
 * touch the victim (a puppet is never "caught": the grabbed puppet stayed in idle in 6d, 6e), so nothing attaches. Before the replica starts a grab action the victim named by the record gets the same id.
 * `net w grab 0|1`, counters in `net w grab`. Research, untested on hardware. */
static void w_grab_attach(unsigned en, int slot)
{
    unsigned victim = player_obj(slot);
    if (!victim || victim == en) return;
    unsigned tag = rd32(en + 0x73E8);
    if (!tag) {
        tag = 0x600D0000u | (++g_w_grab_n & 0xFFFF);
        wr(en + 0x73E8, &tag, 4);
    }
    if (rd32(victim + 0x3788) != tag) {
        wr(victim + 0x3788, &tag, 4);
        g_w_grab_n++;
        g_w_grab_last_tag = tag;
        if (g_w_grab_n < 20) r2_log(W_LOG "grab: the replica %08x starts a grab: victim slot %d (%08x, hp %d) gets the catch id %08x", en, slot, victim, (int)rd32(victim + 0x1A08), tag);
    }
}

/* Stage 6g: the enemy that holds the human `victim_tag` names (catch id: victim+0x3788 == enemy+0x73E8, glue_logic.h). The chain test is known for the Afflicted (Em1100) only; other classes count as "in the chain". */
static int w_holder_info(unsigned tag, glue_hold_in_t *in, unsigned *act_out, char *cls, int cls_size)
{
    in->holder_found = 0, in->holder_hp = 0, in->holder_in_chain = 1, in->max_ms = GLUE_HOLD_MAX_MS;
    if (act_out) *act_out = 0;
    if (cls_size > 0) cls[0] = 0;
    if (!tag) return 0;
    unsigned list[48];
    unsigned n = w_live_enemies(list, 48);
    for (unsigned i = 0; i < n; i++) {
        unsigned e = list[i];
        if (rd32(e + 0x73E8) != tag) continue;
        char cn[100];
        w_class(e, cn, sizeof cn);
        unsigned act = rd32(e + 0x1A54);
        in->holder_found = 1, in->holder_hp = (int)rd32(e + 0x1A08);
        int afflicted = strstr(cn, "Em1100") != NULL;
        in->holder_in_chain = afflicted ? glue_grab_family(act) : 1;
        in->max_ms = afflicted ? GLUE_HOLD_MAX_MS : GLUE_HOLD_MAX_OTHER_MS;
        if (act_out) *act_out = act;
        if (cls_size > 0) snprintf(cls, (size_t)cls_size, "%.60s", cn);
        return 1;
    }
    return 0;
}

static void w_apply_enemy_record(const unsigned char *p)
{
    short a16, yaw, hp;
    unsigned short b, c, act, pact;
    float pos[3];
    memcpy(&a16, p, 2), memcpy(&b, p + 2, 2), memcpy(&c, p + 4, 2), memcpy(pos, p + 6, 12), memcpy(&yaw, p + 18, 2), memcpy(&hp, p + 20, 2), memcpy(&act, p + 22, 2), memcpy(&pact, p + 24, 2);
    unsigned en = w_find_active(SENEMY_CELL, (int)a16, b, c);
    if (!en) {   /* no replica yet (the SPAWN was missed or came before this PC was running): a record is as good as a SPAWN, at most once per second per id */
        g_w_rx_nomatch++;
        static unsigned try_t[16], try_k[16];
        unsigned key = ((unsigned)(unsigned short)a16 << 20) ^ (b << 8) ^ c, now = GetTickCount(), slot = key & 15;
        if (g_w_guard && g_w_autospawn && (try_k[slot] != key || now - try_t[slot] > 1000)) {
            try_k[slot] = key, try_t[slot] = now;
            unsigned se = rd32(SENEMY_CELL);
            if (se) {
                g_w_from_host++;
                unsigned args[6] = {se, LAYOUT_VTBL, (unsigned)(int)a16, b | ((unsigned)c << 16), 0, 0};
                unsigned r = do_call(2, 0x5FD780, args, 6);
                g_w_from_host--;
                g_w_applied_spawn++;
                r2_log(W_LOG "replica created from a record: layout (%d,%u,%u) -> %08x", (int)a16, b, c, r);
            }
        }
        return;
    }
    float cx = rdf(en + 0x40), cz = rdf(en + 0x48), dx = pos[0] - cx, dz = pos[2] - cz, dist = sqrtf(dx * dx + dz * dz);
    if (dist < 2000.0f) g_w_err_sum += dist, g_w_err_n++, g_w_err_max = dist > g_w_err_max ? dist : g_w_err_max;
    float yawf = (float)yaw / 10000.0f;
    wr(en + 0x39A0, &pos[0], 4), wr(en + 0x39A4, &pos[1], 4), wr(en + 0x39A8, &pos[2], 4), wr(en + 0x39C4, &yawf, 4);
    if (dist > 400.0f || fabsf(pos[1] - rdf(en + 0x44)) > 200.0f) {   /* far: snap by writing the position (FUN_00710FD0, the player's remote teleport, crashed on a freshly created replica: it reads player-only fields) */
        wr(en + 0x40, &pos[0], 4), wr(en + 0x44, &pos[1], 4), wr(en + 0x48, &pos[2], 4);
        g_w_enemy_snaps++;
    } else {
        unsigned a[2] = {en, (unsigned)g_w_apply_frames};
        do_call(2, 0xADCA10, a, 2);
        do_call(2, 0xADC8C0, a, 2);
    }
    if (g_w_apply_target && p[30] < 2) {   /* same target as the host's enemy: the replica's own AI then chases / attacks the same human (EnemyIntel target at +0x50, the enemy's at +0x7168) */
        unsigned tp = player_obj(p[30]);
        if (tp && rd32(en + 0x7168) != tp) {
            unsigned intel = rd32(en + 0x6F10);
            if (intel > 0x10000) wr(intel + 0x50, &tp, 4);
            unsigned ta[2] = {en, tp};
            do_call(2, 0x61A520, ta, 2);
            g_w_tgt_set++;
        }
    }
    int dying = hp <= 0 && (int)rd32(en + 0x1A08) > 0;   /* the host says dead: the DEATH event kills the replica through ApplyDamage (the killer's PC credits the kill); the record only does it when that event is late */
    if (dying) {
        static unsigned dy_obj[8], dy_t[8];
        unsigned k = (en >> 4) & 7, now2 = GetTickCount();
        if (dy_obj[k] != en) dy_obj[k] = en, dy_t[k] = now2;
        if (now2 - dy_t[k] < 1500) return;   /* wait for the DEATH event */
    }
    if (g_w_apply_hp && hp >= 0 && (int)rd32(en + 0x1A08) != hp) {   /* the host's HP is the truth (a replica starts with its own roll of the enemy's HP) */
        unsigned h = (unsigned)hp;
        wr(en + 0x1A08, &h, 4);
    }
    unsigned cur_act = rd32(en + 0x1A54) & 0xFFFF;
    int force_act = g_w_apply_act && act != cur_act && act < 0x400;
    if (force_act && g_w_replica_free && !glue_replica_force_ok(act, cur_act)) force_act = 0, g_w_chain_skips++;   /* 6g: the replica is inside the grab chain the host is in: its own actions walk it (re-forcing 77 every time it moved to 79 restarted the grab over and over) */
    if (force_act) {
        unsigned vt = rd32(en), fn = rd32(vt + 0xB8);
        if (fn > 0x400000 && fn < 0x1400000) {
            if (g_w_grab_attach && act >= 0x4D && act <= 0x58 && p[30] < 2) w_grab_attach(en, p[30]);   /* the host's enemy starts a grab: let the replica's own grab start find its victim */
            unsigned args[6] = {en, act, 1, 0, 0xFFFFFFFFu, 0};
            g_w_replay++;
            do_call(2, fn, args, 6);
            g_w_replay--;
            g_w_act_changes++;
        }
    }
}

static void w_recv_world(void)
{
    cp_world wd;
    int guard = 0;
    while (guard++ < 8 && cs_recv_world(g_cs, &wd)) {
        if (wd.kind == WK_FLAGS) {
            story_apply_digest(wd.body, wd.len);
            continue;
        }
        if (wd.kind == WK_ITEMS) {
            story_apply_items(wd.body, wd.len);
            continue;
        }
        if (wd.kind != WK_ENEMIES || cs_is_host(g_cs)) continue;
        g_w_rx_batches++;
        for (unsigned off = 0; off + WREC <= wd.len; off += WREC) {
            g_w_rx_records++;
            w_apply_enemy_record(wd.body + off);
        }
    }
}

/* net w trace <file>|off: a CSV of every live enemy (E rows: ms, layout, class, pos, hp, action, previous action, target slot) and both humans (H rows: ms, slot, type, pos, hp, action) at 10 Hz, plus G rows when an
 * enemy leaves the list (death, despawn): offline analysis of chase / attack / replica error without console round trips. Works on both PCs, any time. */
static void w_trace_tick(void)
{
    if (!g_w_trace) return;
    unsigned now = GetTickCount();
    if (now - g_w_trace_t < 100) return;
    g_w_trace_t = now;
    unsigned list[40];
    unsigned n = w_live_enemies(list, 40);
    for (unsigned i = 0; i < n; i++) {
        unsigned o = list[i];
        layout_t l;
        layout_of(o, &l);
        char cn[100];
        fprintf(g_w_trace, "E,%u,%d.%u.%u,%s,%.0f,%.0f,%.0f,%d,%d,%d,%d,%08x\n", now, l.a, l.b, l.c, w_class(o, cn, sizeof cn), rdf(o + 0x40), rdf(o + 0x44), rdf(o + 0x48), (int)rd32(o + 0x1A08), (int)rd32(o + 0x1A54),
                (int)rd32(o + 0x1A58), w_slot_of_player(rd32(o + 0x7168)), rd32(o + 0x37B8));
    }
    for (unsigned i = 0; i < 64; i++) {   /* enemies that were listed before and are not now */
        if (!g_w_trace_seen[i]) continue;
        int found = 0;
        for (unsigned j = 0; j < n; j++)
            if (list[j] == g_w_trace_seen[i]) found = 1;
        if (!found) fprintf(g_w_trace, "G,%u,%08x\n", now, g_w_trace_seen[i]), g_w_trace_seen[i] = 0;
    }
    for (unsigned j = 0; j < n; j++) {
        int have = 0, free_i = -1;
        for (unsigned i = 0; i < 64; i++) {
            if (g_w_trace_seen[i] == list[j]) have = 1;
            if (!g_w_trace_seen[i] && free_i < 0) free_i = (int)i;
        }
        if (!have && free_i >= 0) g_w_trace_seen[free_i] = list[j];
    }
    for (int sl = 0; sl < 2; sl++) {
        unsigned p = player_obj(sl);
        if (p)   /* + pstate, ctrl word, flags7900 (6e: what a hold / a QTE looks like in the state words) */
            fprintf(g_w_trace, "H,%u,%d,%u,%.0f,%.0f,%.0f,%d,%d,%08x,%08x,%08x\n", now, sl, rd32(p + 0x7920), rdf(p + 0x40), rdf(p + 0x44), rdf(p + 0x48), (int)rd32(p + 0x1A08), (int)rd32(p + 0x1A54), rd32(p + 0x37B8), rd32(p + 0x78E8),
                    rd32(p + 0x7900));
    }
    fflush(g_w_trace);
}

/* once per game pass while PLAYING (coop_game_frame) */
static void w_tick(void)
{
    w_trace_tick();
    if (g_w_autoheal > 0) {   /* test aid (`net w autoheal <hp>`): the local human is topped up to its maximum whenever its health falls below the threshold, so a long fight can be measured; damage events are unchanged */
        unsigned p = player_obj(g_my_slot);
        int hp = p ? (int)rd32(p + 0x1A08) : 0, mx = p ? (int)rd32(p + 0x1A0C) : 0;
        if (p && hp > 0 && mx > 0 && hp < g_w_autoheal && hp < mx) {
            unsigned v = (unsigned)mx;
            wr(p + 0x1A08, &v, 4);
            g_w_healed++;
        }
    }
    if (!g_w_on || !g_cs || !cs_active(g_cs)) return;
    if (cs_is_host(g_cs)) w_send_enemies(), story_send_digest();
    story_send_items();
    w_sm_tick();
    w_recv_world();
}

static void w_status_ext(char *out, int size)
{
    int o = 0;
    o += snprintf(out + o, size - o, "world records: sent %u batches / %u records, received %u / %u (no replica for %u), action changes %u, snaps %u, target sets %u, kill reports %u; events applied: spawn %u, hit %u, death %u, pickup %u (unmatched %u), puppet damage sent %u applied %u; replica error at apply: mean %.2f max %.1f (%u records)\n",
                  g_w_tx_batches, g_w_tx_records, g_w_rx_batches, g_w_rx_records, g_w_rx_nomatch, g_w_act_changes, g_w_enemy_snaps, g_w_tgt_set, g_w_kill_reports, g_w_applied_spawn, g_w_applied_hit, g_w_applied_death, g_w_applied_pickup, g_w_unmatched, g_w_pdmg_sent,
                  g_w_pdmg_applied, g_w_err_n ? g_w_err_sum / g_w_err_n : 0.0, g_w_err_max, g_w_err_n);
    if (o < size - 500) story_status(out + o, size - o);
}
