/* ach.inc: stage 8b, Steam achievements credited to each player in co-op. Included at the end of coop3.c after world.inc (same translation unit: rd32 / wr / do_call / player_obj,
 * g_cs / g_state / g_my_slot / g_rem_slot, w_tramp / w_jmp / w_ev_send come from there). Pure data and decisions: ach_logic.h (tested offline, tests\test_ach.c).
 * Design, the table of all 43 achievements and the evidence: docs\achievements.md (public), notes\stage8b.md (private).
 *
 * The mod never calls SetAchievement. Every unlock still goes through the game's own record path on the PC of the human who earned it:
 *   event -> stat bump (FUN_00979580 + FUN_00972E90) -> record wrapper -> Ach_SetRecordState (0x408F00) -> Ach_RecordToSteam -> queue -> SetAchievement.
 * What this file adds (only while a co-op session runs; outside a session every hook is a plain pass-through):
 *   1. THE GATE on the stat bump pair. The game bumps the profile's global total of a kind and, right after, the per-slot table of the acting slot; the record wrapper then compares the
 *      total with a goal. The global bump happens for ANY slot, so the partner's puppet (type 2) would move this profile towards an achievement. The pair is intercepted: when the slot
 *      is not a local human, the global total is put back and the per-slot bump is skipped, so the check that follows sees an unchanged profile (world.inc's h_killrep rolled the three
 *      tables back only AFTER the record wrappers had run, which left a hole at goal-1).
 *   2. FORWARDING of the host's enemy-side causes. The host's real enemies react to both humans (back attack, light attack, assisting a held partner, homunculus weapon, progressor
 *      jump attack: stat kinds 0x2B 0x30 0x37 0x38 0x39); the client's replicas have no AI, so the client's PC never sees those causes for its own human. When such a bump is made for
 *      the partner's puppet on the host, it is sent to the client (EV_ACH_BUMP) and the client replays the game's own bump + record check for its local human, unless its own code
 *      bumped the same kind a moment ago (dedupe window). Off by default until the causes are verified live (`net ach fwd 1`, REV2_ACHFWD=1).
 *   3. Context guards for the two enemy functions that set GRAP_COMBINATION (0x7B) directly: while the enemy's target is the partner's puppet, record sets are suppressed.
 *   4. A log of every record the game achieves (`ach: RECORD ...`) and the console `net ach` (status, totals, test aids that only work while the Steam writers are blocked).
 */
#include "ach_logic.h"

#define ACH_LOG "ach: "
enum { EV_ACH_BUMP = CP_EV_USER + 0x50 };   /* 0x150; 0x150..0x15F belong to this file (kinds below 0x120 are not queued, 0x120..0x12F are the world events) */
#define ACH_EV_FIRST EV_ACH_BUMP
#define ACH_EV_LAST (CP_EV_USER + 0x5F)
#define ACH_STORY_CELL 0x157D120u
#define ACH_SBIO_CELL 0x1552728u

typedef void(__attribute__((thiscall)) *a_statadd_t)(void *tbl, unsigned kind, int n);                    /* FUN_00979580, ecx = story+0x20, ret 8 */
typedef void(__attribute__((thiscall)) *a_statslot_t)(void *tbl, unsigned slot, unsigned kind, int n);    /* FUN_00972E90, ecx = story+0x31320, ret 0xC */
typedef void(__attribute__((thiscall)) *a_setrec_t)(void *self, unsigned id, unsigned state);             /* Ach_SetRecordState, ecx = sBioAchievement, ret 8 */
typedef void(__attribute__((fastcall)) *a_grap_t)(void *enemy);                                           /* FUN_006067B0, ecx = enemy */
typedef int(__attribute__((thiscall)) *a_em1100_t)(void *intel);                                          /* uEm1100Intel_vf62: plain `ret` (the decompiler's extra parameters are uninitialised registers), returns the next action id */
static a_statadd_t o_a_statadd;
static a_statslot_t o_a_statslot;
static a_setrec_t o_a_setrec;
static a_grap_t o_a_grap;
static a_em1100_t o_a_em1100;

static int g_ach_installed, g_ach_gate = 1, g_ach_fwd, g_ach_ctx = 1;
static unsigned g_ach_dedupe_ms = 1500;
static int g_ach_suppress;                        /* >0: Ach_SetRecordState is swallowed (context guard, console test) */
static unsigned g_ach_pend_tbl, g_ach_pend_kind, g_ach_pend_before;
static int g_ach_pend, g_ach_pend_n;
static unsigned g_ach_last_mine[0x51];            /* GetTickCount of the last stat bump of each kind for the local human (0 = none) */
static unsigned g_ach_seq, g_ach_n_mine, g_ach_n_undone, g_ach_n_unpaired, g_ach_n_fwd_tx, g_ach_n_fwd_rx, g_ach_n_fwd_applied, g_ach_n_fwd_dup, g_ach_n_fwd_off, g_ach_n_ctx_supp, g_ach_n_rec_supp;
static unsigned g_ach_recs[64], g_ach_recs_t[64], g_ach_nrecs;
static int g_ach_loglim = 80;

static int ach_session(void)
{
    /* a partner must be connected: after he left, the slot is the game's own business again (an AI partner's class kills count towards the totals as in single player) */
    return g_cs && cs_active(g_cs) && g_state >= G_SETUP && (g_my_slot == 0 || g_my_slot == 1) && (g_rem_slot == 0 || g_rem_slot == 1) && cs_remote_pid(g_cs) >= 0;
}

static ach_cfg_t ach_cfg(void)
{
    ach_cfg_t c;
    c.session = ach_session();
    c.gate_on = g_ach_gate;
    c.is_host = g_cs ? cs_is_host(g_cs) : 0;
    c.fwd_on = g_ach_fwd;
    return c;
}

static int ach_rec_bit(unsigned story, unsigned id, unsigned bit)
{
    unsigned a[3] = {story + 0x20, id, bit};
    return (int)(do_call(2, 0x979B20, a, 3) & 0xFF);   /* Story_GetRecordBit, thiscall on story+0x20 */
}

/* ---------------------------------------------------------------- the gate */
static __attribute__((thiscall)) void h_a_statadd(void *tbl, unsigned kind, int n)
{
    g_ach_pend = 0;
    if (kind < 0x51 && n > 0) {
        g_ach_pend_tbl = (unsigned)tbl, g_ach_pend_kind = kind, g_ach_pend_n = n;
        g_ach_pend_before = rd32((unsigned)tbl + 0x1BC + 4 * kind);
        g_ach_pend = 1;
    }
    o_a_statadd(tbl, kind, n);
}

static void ach_send_bump(unsigned kind, unsigned n)
{
    unsigned char b[ACH_EV_BUMP_LEN];
    ach_pack_bump(b, kind, n, ++g_ach_seq, GetTickCount());
    w_ev_send(EV_ACH_BUMP, b, ACH_EV_BUMP_LEN);
    g_ach_n_fwd_tx++;
}

static __attribute__((thiscall)) void h_a_statslot(void *tbl2, unsigned slot, unsigned kind, int n)
{
    int pend = g_ach_pend && g_ach_pend_kind == kind && g_ach_pend_n == n;
    g_ach_pend = 0;
    if (slot < 8 && ach_session()) {
        ach_cfg_t c = ach_cfg();
        unsigned p = player_obj((int)slot);
        int local = p && rd32(p + 0x7920) == 0;      /* the game's own definition of "a human of this PC" (it credits the human ledger on it) */
        int remote = (int)slot == g_rem_slot;
        int v = ach_gate(&c, local, remote, kind);
        if (local && kind < 0x51) {
            g_ach_last_mine[kind] = GetTickCount() | 1;
            g_ach_n_mine++;
            if (g_ach_loglim > 0 && ach_fwd_find(kind)) r2_log(ACH_LOG "bump kind 0x%02X x%d for the local human (slot %u): credited", kind, n, slot);
        } else if (!local && v != ACH_ALLOW) {
            if (pend) {
                wr(g_ach_pend_tbl + 0x1BC + 4 * kind, &g_ach_pend_before, 4);   /* the global total goes back: the record check that follows sees an unchanged profile */
                g_ach_n_undone++;
                if (g_ach_loglim > 0) {
                    g_ach_loglim--;
                    r2_log(ACH_LOG "bump kind 0x%02X x%d for slot %u (%s) is not this profile's: global total restored to %u%s", kind, n, slot, remote ? "the partner's puppet" : "no local human", g_ach_pend_before,
                           v == ACH_UNDO_FORWARD ? ", forwarded to the partner" : "");
                }
                if (v == ACH_UNDO_FORWARD) ach_send_bump(kind, (unsigned)n);
                return;   /* the per-slot bump of the partner's slot is skipped as well (6d: a partner's kill never moves this profile's tables) */
            }
            g_ach_n_unpaired++;
        }
    }
    o_a_statslot(tbl2, slot, kind, n);
}

/* ---------------------------------------------------------------- records */
static __attribute__((thiscall)) void h_a_setrec(void *self, unsigned id, unsigned state)
{
    const void *ret = __builtin_return_address(0);
    unsigned story = rd32(ACH_STORY_CELL);
    if (g_ach_suppress > 0 && state == 2 && ach_ctx_record(id)) {
        g_ach_n_rec_supp++;
        if (g_ach_loglim > 0) {
            g_ach_loglim--;
            const ach_entry_t *e = ach_by_record(id);
            r2_log(ACH_LOG "record 0x%02X%s%s set to 2 by the game: SUPPRESSED (the cause belongs to the partner), caller exe+%x", id, e ? " " : "", e ? e->name : "", (unsigned)ret - 0x400000);
        }
        return;
    }
    static int n_epi;   /* 6h: the episode-end records are logged whenever the game asks, also when the bit was set already (an account that cleared the episode before shows no RECORD line) */
    if (state == 2 && (id <= 0x0F || id == 0x70) && n_epi < 60) {
        n_epi++;
        r2_log(ACH_LOG "SetRecordState(0x%02X, 2) asked by the game: bit 2 before %d (caller exe+%x)", id, story ? ach_rec_bit(story, id, 2) : -1, (unsigned)ret - 0x400000);
    }
    int before = story && state == 2 ? ach_rec_bit(story, id, 2) : 1;
    o_a_setrec(self, id, state);
    if (story && state == 2 && !before && ach_rec_bit(story, id, 2)) {
        const ach_entry_t *e = ach_by_record(id);
        if (g_ach_nrecs < 64) g_ach_recs[g_ach_nrecs] = id, g_ach_recs_t[g_ach_nrecs++] = GetTickCount();
        r2_log(ACH_LOG "RECORD 0x%02X achieved%s%s%s%s (caller exe+%x, session %s%s)", id, e ? " " : "", e ? e->name : " (no Steam twin)", e ? " rule " : "",
               e ? (e->rule == ACHR_PLAYER ? "PLAYER" : e->rule == ACHR_SHARED ? "SHARED" : e->rule == ACHR_RISKY ? "RISKY" : "N/A") : "", (unsigned)ret - 0x400000,
               ach_session() ? "yes" : "no", steam_ach_blocking() ? ", Steam writers BLOCKED" : "");
    }
}

/* ---- context guards: GRAP_COMBINATION (0x7B) is set directly by the host's enemy code for the human it involves ---- */
static int ach_target_is_partner(unsigned enemy)
{
    if (!g_ach_ctx || !enemy || !ach_session()) return 0;
    unsigned tgt = rd32(enemy + 0x38D0);   /* the enemy's current target (piVar1[0xE34]) */
    return tgt && tgt == player_obj(g_rem_slot);
}

static unsigned g_ach_n_grap_calls;
static __attribute__((fastcall)) void h_a_grap(void *enemy)
{
    int sup = ach_target_is_partner((unsigned)enemy);
    g_ach_n_grap_calls++;
    if (sup) g_ach_suppress++, g_ach_n_ctx_supp++;
    o_a_grap(enemy);
    if (sup) g_ach_suppress--;
}

static unsigned g_ach_n_em1100_calls;   /* with g_ach_n_grap_calls: proof that the trampolines run (net ach status) */
static __attribute__((thiscall)) int h_a_em1100(void *intel)
{
    unsigned enemy = intel ? rd32((unsigned)intel + 0x30) : 0;
    int sup = ach_target_is_partner(enemy);
    g_ach_n_em1100_calls++;
    if (sup) g_ach_suppress++, g_ach_n_ctx_supp++;
    int r = o_a_em1100(intel);
    if (sup) g_ach_suppress--;
    return r;
}

/* ---------------------------------------------------------------- forwarded event (client side) */
static void ach_replay_bump(unsigned kind, unsigned n)
{
    const ach_kindw_t *f = ach_fwd_find(kind);
    unsigned story = rd32(ACH_STORY_CELL), sbio = rd32(ACH_SBIO_CELL);
    if (!f || !story || !sbio) return;
    unsigned a[4] = {story, (unsigned)g_my_slot, kind, n};
    do_call(2, 0x9815E0, a, 4);                                   /* the game's stat bump for my slot (the same call FUN_00603EA0 makes), through the gate: my human, credited */
    unsigned s[1] = {sbio};
    do_call(2, 0x4093D0, s, 1);                                   /* the checks the enemy code runs after the bump */
    do_call(2, 0x409200, s, 1);
    do_call(2, f->wrapper, s, 1);                                 /* the kind's own record check */
}

static void ach_on_event(const cp_event *e)
{
    unsigned kind = 0, n = 0, seq = 0;
    if (e->kind != EV_ACH_BUMP || !ach_unpack_bump(e->body, e->len, &kind, &n, &seq)) return;
    if (!ach_session() || (g_cs && cs_is_host(g_cs))) return;   /* only the client receives the host's forwarded causes */
    g_ach_n_fwd_rx++;
    if (!ach_fwd_find(kind)) return;
    if (!g_ach_fwd) {
        g_ach_n_fwd_off++;
        r2_log(ACH_LOG "forwarded bump kind 0x%02X #%u ignored: forwarding is off on this PC (net ach fwd 1)", kind, seq);
        return;
    }
    if (!ach_dedupe_ok(GetTickCount(), g_ach_last_mine[kind], g_ach_dedupe_ms)) {
        g_ach_n_fwd_dup++;
        r2_log(ACH_LOG "forwarded bump kind 0x%02X #%u dropped: this PC bumped the same kind for its human %u ms ago", kind, seq, GetTickCount() - g_ach_last_mine[kind]);
        return;
    }
    g_ach_n_fwd_applied++;
    r2_log(ACH_LOG "forwarded bump kind 0x%02X x%u #%u from the host: replaying the game's bump and record check for my human (slot %d)", kind, n, seq, g_my_slot);
    ach_replay_bump(kind, n);
}

/* ---------------------------------------------------------------- install */
static int ach_install(void);
static void ach_frame(void)
{
    if (!g_ach_installed && g_cs && cs_active(g_cs)) ach_install();   /* the hooks go in with the first session, before any slot has a partner (nothing is patched for solo play) */
}

static int ach_install(void)
{
    if (g_ach_installed) return 1;
    if (!r2_text_decrypted()) return 0;
    char v[8];
    if (r2_env("REV2_ACHFWD", v, sizeof v) && v[0] == '1') g_ach_fwd = 1;
    static const unsigned char b_add[] = {0x8B, 0x54, 0x24, 0x04, 0x56};                 /* mov edx,[esp+4]; push esi */
    static const unsigned char b_slot[] = {0x8B, 0x54, 0x24, 0x04, 0x83, 0xFA, 0x08};     /* mov edx,[esp+4]; cmp edx,8 */
    static const unsigned char b_rec[] = {0x55, 0x56, 0x8B, 0xE9, 0x57};                 /* push ebp; push esi; mov ebp,ecx; push edi */
    static const unsigned char b_grap[] = {0x83, 0xEC, 0x08, 0x57, 0x8B, 0xF9};           /* sub esp,8; push edi; mov edi,ecx */
    static const unsigned char b_em[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};             /* push ebp; mov ebp,esp; and esp,-16 */
    void *t;
    int ok = 0;
    if ((t = w_tramp(0x979580, b_add, 5))) o_a_statadd = (a_statadd_t)t, ok += w_jmp(0x979580, 5, (void *)h_a_statadd);
    if ((t = w_tramp(0x972E90, b_slot, 7))) o_a_statslot = (a_statslot_t)t, ok += w_jmp(0x972E90, 7, (void *)h_a_statslot);
    if ((t = w_tramp(0x408F00, b_rec, 5))) o_a_setrec = (a_setrec_t)t, ok += w_jmp(0x408F00, 5, (void *)h_a_setrec);
    if ((t = w_tramp(0x6067B0, b_grap, 6))) o_a_grap = (a_grap_t)t, ok += w_jmp(0x6067B0, 6, (void *)h_a_grap);
    /* The Em1100 context hook is OFF unless REV2_ACHEM1100=1: in the s8b1 pair run (60 s of an awake Afflicted that chased the partner's puppet) the function was never entered, so its trampoline
     * has not run live; the grapple function hook (0x6067B0) has (crows, host). Without it the Afflicted path of GRAP_COMBINATION (FUN_00603F20 from uEm1100Intel_vf62) is unguarded. */
    if (r2_env("REV2_ACHEM1100", v, sizeof v) && v[0] == '1' && (t = w_tramp(0x5EC800, b_em, 6))) o_a_em1100 = (a_em1100_t)t, ok += w_jmp(0x5EC800, 6, (void *)h_a_em1100);
    g_ach_installed = 1;
    r2_log(ACH_LOG "hooks installed: stat bump %s, per-slot bump %s, record state %s, grapple context %s, Em1100 context %s (off by default: REV2_ACHEM1100=1); forwarding %s; Steam writers %s", o_a_statadd ? "yes" : "NO",
           o_a_statslot ? "yes" : "NO", o_a_setrec ? "yes" : "NO", o_a_grap ? "yes" : "NO", o_a_em1100 ? "yes" : "NO", g_ach_fwd ? "ON" : "off", steam_ach_blocking() ? "BLOCKED" : "pass through");
    return ok > 0;
}

/* ---------------------------------------------------------------- console: net ach ... (game thread) */
static void ach_totals(char *out, int size)
{
    unsigned story = rd32(ACH_STORY_CELL), sbio = rd32(ACH_SBIO_CELL);
    int o = 0;
    if (!story || !sbio) {
        snprintf(out, size, "no story block / achievement system yet");
        return;
    }
    o += snprintf(out + o, size - o, "kind  total(global)  my-ledger  goal  record  state  achievement\n");
    for (int i = 0; i < ACH_N && o < size - 120; i++) {
        const ach_entry_t *e = &ach_table[i];
        if (e->kind == ACH_NOKIND) continue;
        unsigned total = rd32(story + 0x20 + 0x1BC + 4 * e->kind), ga[2] = {sbio, e->rec};
        unsigned short led = 0;
        r2_safe_read((void *)(story + 0x31320 + 0x118D38 + 2 * e->kind), &led, 2);
        unsigned goal = do_call(2, 0x409DF0, ga, 2);   /* Ach_GetRecordCounter */
        o += snprintf(out + o, size - o, "0x%02X  %-14u %-10u %-5u 0x%02X    %s     %s  (record bits 0/1/2: %d%d%d)\n", e->kind, total, led, goal, e->rec, ach_rec_bit(story, e->rec, 2) ? "DONE" : "-", e->name,
                      ach_rec_bit(story, e->rec, 0), ach_rec_bit(story, e->rec, 1), ach_rec_bit(story, e->rec, 2));
    }
}

static void ach_command(const char *rest, char *out, int size)
{
    char sub[24] = "", arg[200] = "";
    sscanf(rest, "%23s %199[^\n]", sub, arg);
    if (!sub[0] || !strcmp(sub, "status")) {
        ach_cfg_t c = ach_cfg();
        int o = snprintf(out, size, "achievements (stage 8b): hooks %s, session %s (%s), gate %s, forwarding %s (dedupe %u ms), context guards %s\n", g_ach_installed ? "installed" : "NOT installed", c.session ? "running" : "no",
                         c.is_host ? "host" : "client", g_ach_gate ? "on" : "off", g_ach_fwd ? "on" : "off", g_ach_dedupe_ms, g_ach_ctx ? "on" : "off");
        o += snprintf(out + o, size - o, "bumps: %u credited to my human, %u of the partner undone, %u unpaired; forwarded: %u sent, %u received (%u applied, %u dropped as doubles, %u ignored with forwarding off); context guard hits %u, records suppressed %u\n",
                      g_ach_n_mine, g_ach_n_undone, g_ach_n_unpaired, g_ach_n_fwd_tx, g_ach_n_fwd_rx, g_ach_n_fwd_applied, g_ach_n_fwd_dup, g_ach_n_fwd_off, g_ach_n_ctx_supp, g_ach_n_rec_supp);
        o += snprintf(out + o, size - o, "context hooks ran: enemy grapple function %u, Em1100 intel %u times\n", g_ach_n_grap_calls, g_ach_n_em1100_calls);
        o += snprintf(out + o, size - o, "records achieved by the game on this PC: %u", g_ach_nrecs);
        for (unsigned i = 0; i < g_ach_nrecs && o < size - 400; i++) {
            const ach_entry_t *e = ach_by_record(g_ach_recs[i]);
            o += snprintf(out + o, size - o, "\n  0x%02X %s", g_ach_recs[i], e ? e->name : "(no Steam twin)");
        }
        o += snprintf(out + o, size - o, "\n");
        steam_ach_status(out + o, size - o);
    } else if (!strcmp(sub, "gate")) {
        if (arg[0]) g_ach_gate = atoi(arg) != 0;
        snprintf(out, size, "gate %d (1: a stat bump for a slot that is not a local human is undone)", g_ach_gate);
    } else if (!strcmp(sub, "fwd")) {
        if (arg[0]) g_ach_fwd = atoi(arg) != 0;
        snprintf(out, size, "forwarding %d (1: the host sends the partner's enemy-side bumps, the client replays them for its human; set it on BOTH PCs)", g_ach_fwd);
    } else if (!strcmp(sub, "dedupe")) {
        if (arg[0]) g_ach_dedupe_ms = (unsigned)atoi(arg);
        snprintf(out, size, "dedupe window %u ms", g_ach_dedupe_ms);
    } else if (!strcmp(sub, "ctx")) {
        if (arg[0]) g_ach_ctx = atoi(arg) != 0;
        snprintf(out, size, "context guards %d (1: records set by the enemy code while its target is the partner's puppet are suppressed)", g_ach_ctx);
    } else if (!strcmp(sub, "log")) {
        if (arg[0]) g_ach_loglim = atoi(arg);
        snprintf(out, size, "log budget %d lines", g_ach_loglim);
    } else if (!strcmp(sub, "totals")) {
        ach_totals(out, size);
    } else if (!strcmp(sub, "table")) {
        int o = 0;
        for (int i = 0; i < ACH_N && o < size - 160; i++) {
            char k[8] = "-";
            if (ach_table[i].kind != ACH_NOKIND) snprintf(k, sizeof k, "0x%02X", ach_table[i].kind);
            o += snprintf(out + o, size - o, "%2d rec 0x%02X %-30s %-6s kind %s\n", ach_table[i].steam, ach_table[i].rec, ach_table[i].name,
                          ach_table[i].rule == ACHR_PLAYER ? "PLAYER" : ach_table[i].rule == ACHR_SHARED ? "SHARED" : ach_table[i].rule == ACHR_RISKY ? "RISKY" : "N/A", k);
        }
    } else if (!strcmp(sub, "bump") || !strcmp(sub, "set") || !strcmp(sub, "rec")) {
        /* test aids: they can earn a record. Only while the Steam writers are blocked (REV2_ACHBLOCK, the default of every lab run): a real account never receives them */
        if (!steam_ach_blocking()) {
            snprintf(out, size, "refused: the Steam writers are NOT blocked (REV2_ACHBLOCK=1 or a console run); this command could unlock a real achievement");
            return;
        }
        unsigned story = rd32(ACH_STORY_CELL), sbio = rd32(ACH_SBIO_CELL);
        if (!story || !sbio) {
            snprintf(out, size, "no story block / achievement system yet");
            return;
        }
        if (!strcmp(sub, "set")) {   /* net ach set <kind hex> <value>: the profile's global total of a kind (to stand one below a goal) */
            unsigned kind = 0, val = 0;
            if (sscanf(arg, "%x %u", &kind, &val) != 2 || kind >= 0x51) {
                snprintf(out, size, "usage: net ach set <kind hex> <value>");
                return;
            }
            wr(story + 0x20 + 0x1BC + 4 * kind, &val, 4);
            snprintf(out, size, "global total of kind 0x%02X is now %u", kind, rd32(story + 0x20 + 0x1BC + 4 * kind));
        } else if (!strcmp(sub, "bump")) {   /* net ach bump <kind hex> <slot> [n]: what the enemy code does for a human: the game's bump for that slot + the record checks of the kind (through the gate) */
            unsigned kind = 0, slot = 0, n = 1;
            if (sscanf(arg, "%x %u %u", &kind, &slot, &n) < 2 || kind >= 0x51 || slot > 1 || n < 1 || n > 50) {
                snprintf(out, size, "usage: net ach bump <kind hex> <slot 0|1> [n]");
                return;
            }
            unsigned a[4] = {story, slot, kind, n}, s[1] = {sbio};
            unsigned t0 = g_ach_n_undone, f0 = g_ach_n_fwd_tx, m0 = g_ach_n_mine;
            do_call(2, 0x9815E0, a, 4);
            do_call(2, 0x4093D0, s, 1);
            do_call(2, 0x409200, s, 1);
            const ach_kindw_t *f = ach_kind_find(kind);
            if (f) do_call(2, f->wrapper, s, 1);
            snprintf(out, size, "bump kind 0x%02X x%u for slot %u: total now %u; gate: %u credited to me, %u undone, %u forwarded", kind, n, slot, rd32(story + 0x20 + 0x1BC + 4 * kind), g_ach_n_mine - m0, g_ach_n_undone - t0, g_ach_n_fwd_tx - f0);
        } else if (!strcmp(sub, "rec")) {   /* net ach rec <record hex>: Ach_SetRecordState(id, 2) through the game (the full path up to the blocked SetAchievement) */
            unsigned id = 0;
            if (sscanf(arg, "%x", &id) != 1 || id > 0x13F) {
                snprintf(out, size, "usage: net ach rec <record hex>");
                return;
            }
            unsigned a[3] = {sbio, id, 2};
            do_call(2, 0x408F00, a, 3);
            snprintf(out, size, "record 0x%02X: state 2 %s", id, ach_rec_bit(story, id, 2) ? "set" : "not set (record not valid in this profile?)");
        }
    } else {
        snprintf(out, size, "net ach status|totals|table|gate [0|1]|fwd [0|1]|dedupe <ms>|ctx [0|1]|log <n>|bump <kind> <slot> [n]|set <kind> <value>|rec <record> (the last three only while the Steam writers are blocked)");
    }
}
