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
 *      bumped the same kind a moment ago (dedupe window). G-ACH (2026-11-07): on by default on the host (auto sends; REV2_ACHFWD=0 / `net ach fwd 0` stops it), see ach_logic.h.
 *   3. Context guards for the two enemy functions that set GRAP_COMBINATION (0x7B) directly: while the enemy's target is the partner's puppet, record sets are suppressed. With
 *      forwarding on, the host sends a swallowed 0x7B to the client (EV_ACH_REC), whose human is then Claire (the record's own condition), and the client sets it (P6).
 *   4. A log of every record the game achieves (`ach: RECORD ...`) and the console `net ach` (status, totals, test aids that only work while the Steam writers are blocked).
 *   5. (P6) ASSIST_HOLD_HELP: the game credits only the seat-0 slot (slot 0 on every PC of the mod); a 5-byte call patch in uEmIntel_vf22 answers the slot of the enemy's last attacker
 *      when he is one of the two humans, so the support role and a client in slot 1 can earn it. Installed with REV2_ACHFWD=1 or REV2_ACHASSIST=1 (or `net ach assist 1`): not run live.
 * Forwarding switch: off / on / auto (default). The host sends unless off; a client replays unless off. The assist call patch needs an explicit REV2_ACHFWD=1 / REV2_ACHASSIST=1.
 *   6. (G-ACH) F6: on a client, the kill report of its own kill runs when the host's DEATH arrives; evaluator A then sees the human's last melee blow if it ended inside the
 *      window with nothing but neutral actions after it (h_a_evala, ach_logic.h ach_f6_action). REV2_ACHF6=0 / `net ach f6 0` switches it off.
 */
#include "ach_logic.h"

#define ACH_LOG "ach: "
enum { EV_ACH_BUMP = CP_EV_USER + 0x50, EV_ACH_REC = CP_EV_USER + 0x51 };   /* 0x150; 0x150..0x15F belong to this file (kinds below 0x120 are not queued, 0x120..0x12F are the world events) */
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

static int g_ach_installed, g_ach_gate = 1, g_ach_fwd = ACH_FWD_AUTO, g_ach_ctx = 1;
static int g_ach_assist_installed, g_ach_assist = 1;   /* P6: the ASSIST_HOLD_HELP call patch (installed only on request) and its decision switch */
static unsigned g_ach_n_assist_calls, g_ach_n_assist_moved, g_ach_n_rec_tx, g_ach_n_rec_rx, g_ach_n_rec_applied, g_ach_rec_seq;
static unsigned g_ach_dedupe_ms = 1500;
static int g_ach_suppress;                        /* >0: Ach_SetRecordState is swallowed (context guard, console test) */
static unsigned g_ach_pend_tbl, g_ach_pend_kind, g_ach_pend_before;
static int g_ach_pend, g_ach_pend_n;
static unsigned g_ach_last_mine[0x51];            /* GetTickCount of the last stat bump of each kind for the local human (0 = none) */
static unsigned g_ach_seq, g_ach_n_mine, g_ach_n_undone, g_ach_n_unpaired, g_ach_n_fwd_tx, g_ach_n_fwd_rx, g_ach_n_fwd_applied, g_ach_n_fwd_dup, g_ach_n_fwd_off, g_ach_n_ctx_supp, g_ach_n_rec_supp;
static unsigned g_ach_recs[64], g_ach_recs_t[64], g_ach_nrecs;
static int g_ach_loglim = 80;
/* G-ACH (2026-11-07): the local human's last melee-class action (ach_is_melee_act) with its start / end tick (end 0 while it runs), the lab action trace, and
 * a budget for the lines that name every record-feeding bump of the local human with the action / weapon class at that moment (the values evaluator A reads) */
static unsigned g_ach_m_act, g_ach_m_t0, g_ach_m_t1, g_ach_m_last_act = 0xFFFFFFFFu;
static ach_blowhist_t g_ach_blow;   /* F6: the local human's last counted blow (ach_logic.h) */
static int g_ach_f6 = 1;
static unsigned g_ach_f6_window = 1000, g_ach_n_f6_seen, g_ach_n_f6_sub;
static int g_ach_tr_slot = -1, g_ach_tr_left, g_ach_mine_loglim = 200;
static unsigned g_ach_tr_obj, g_ach_tr_act = 0xFFFFFFFFu, g_ach_tr_t, g_ach_tr_lines;

static unsigned ach_weapon_class(unsigned p)   /* FUN_006FB230 (thiscall on the player, plain ret): the class id of the weapon object in the hand (player+0x7380), the call evaluator A makes */
{
    unsigned a[1] = {p};
    return p && rd32(p + 0x7380) ? do_call(2, 0x6FB230, a, 1) & 0xFFFF : 0;
}

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
    c.fwd_on = ach_fwd_send(g_ach_fwd);
    return c;
}

static const char *ach_fwd_name(int m) { return m == ACH_FWD_ON ? "on" : m == ACH_FWD_OFF ? "off" : "auto (host: sends, client: replays)"; }

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
            const ach_kindw_t *kw = ach_kind_find(kind);
            g_ach_last_mine[kind] = GetTickCount() | 1;
            g_ach_n_mine++;
            if (kw && g_ach_mine_loglim > 0) {   /* every record-feeding kind (G-ACH): the action / weapon class now (what evaluator A read a moment ago) and the last melee-class action */
                g_ach_mine_loglim--;
                unsigned now = GetTickCount();
                r2_log(ACH_LOG "bump kind 0x%02X x%d for the local human (slot %u): credited (action %u, weapon %04x; last melee-class action %u %s %u ms ago)", kind, n, slot, rd32(p + 0x1A54),
                       ach_weapon_class(p), g_ach_m_act, g_ach_m_t1 ? "ended" : "started", g_ach_m_act ? now - (g_ach_m_t1 ? g_ach_m_t1 : g_ach_m_t0) : 0);
            }
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
        int fwd = g_cs && cs_is_host(g_cs) && ach_fwd_send(g_ach_fwd) && ach_rec_fwd_ok(id) && ach_session();
        if (g_ach_loglim > 0) {
            g_ach_loglim--;
            const ach_entry_t *e = ach_by_record(id);
            r2_log(ACH_LOG "record 0x%02X%s%s set to 2 by the game: SUPPRESSED (the cause belongs to the partner)%s, caller exe+%x", id, e ? " " : "", e ? e->name : "",
                   fwd ? ", forwarded to the partner" : "", (unsigned)ret - 0x400000);
        }
        if (fwd) {   /* P6: the partner's PC sets it for its human (the record's own condition named the partner's character) */
            unsigned char b[ACH_EV_REC_LEN];
            ach_pack_rec(b, id, ++g_ach_rec_seq);
            w_ev_send(EV_ACH_REC, b, ACH_EV_REC_LEN);
            g_ach_n_rec_tx++;
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

/* ---- F6 (G-ACH): evaluator A of the kill report (Result_OnEnemyKilled_A 0x9808E0, thiscall on the story block, one stack argument = the DamageInfo, `ret 4`, prologue
 * `53 8B 5C 24 08`). On a CLIENT, during world.inc's DEATH replay of its own kill (g_w_replay > 0, the DamageInfo's attacker = the local human), the report may read the human's
 * action one round trip after the killing blow; ach_f6_action (ach_logic.h) gives the last counted blow back to it for the length of the call. Everything else passes through. */
typedef void(__attribute__((thiscall)) *a_evala_t)(void *story, unsigned di);
static a_evala_t o_a_evala;
static __attribute__((thiscall)) void h_a_evala(void *story, unsigned di)
{
    unsigned p = 0, cur = 0, use = 0;
    if (g_ach_f6 && g_w_replay > 0 && di && g_cs && !cs_is_host(g_cs) && ach_session() && (p = player_obj(g_my_slot)) && rd32(di + 0xDC) == p) {
        unsigned now = GetTickCount();
        cur = rd32(p + 0x1A54);
        use = ach_f6_action(&g_ach_blow, cur, now, g_ach_f6_window);
        g_ach_n_f6_seen++;
        if (g_ach_loglim > 0) {
            g_ach_loglim--;
            r2_log(ACH_LOG "F6: the kill report of my kill runs at the host's DEATH: my action %u, last counted blow %u %s %u ms ago%s", cur, g_ach_blow.blow, g_ach_blow.end_ms ? "ended" : "-",
                   g_ach_blow.end_ms ? now - g_ach_blow.end_ms : 0, use != cur ? ": the report sees the blow" : "");
        }
        if (use != cur) wr(p + 0x1A54, &use, 4), g_ach_n_f6_sub++;
        else use = 0;
    }
    o_a_evala(story, di);
    if (use) wr(p + 0x1A54, &cur, 4);
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

static void ach_on_rec_event(const cp_event *e)
{
    unsigned rec = 0, seq = 0;
    if (!ach_unpack_rec(e->body, e->len, &rec, &seq)) return;
    if (!ach_session() || (g_cs && cs_is_host(g_cs))) return;
    g_ach_n_rec_rx++;
    int ch = g_cs ? cs_local_character(g_cs) : -1;
    if (!ach_fwd_accept(g_ach_fwd) || !ach_rec_fwd_accept(rec, ch)) {
        r2_log(ACH_LOG "forwarded record 0x%02X #%u ignored (forwarding %s, my character %d)", rec, seq, ach_fwd_name(g_ach_fwd), ch);
        return;
    }
    unsigned story = rd32(ACH_STORY_CELL), sbio = rd32(ACH_SBIO_CELL);
    if (!story || !sbio) return;
    if (ach_rec_bit(story, rec, 2)) {   /* already achieved in this save: nothing to do (the game would not send it again either) */
        r2_log(ACH_LOG "forwarded record 0x%02X #%u: already achieved in this save", rec, seq);
        return;
    }
    g_ach_n_rec_applied++;
    r2_log(ACH_LOG "forwarded record 0x%02X #%u from the host: the game's Ach_SetRecordState for my human", rec, seq);
    unsigned a[3] = {sbio, rec, 2};
    do_call(2, 0x408F00, a, 3);
}

static void ach_on_event(const cp_event *e)
{
    unsigned kind = 0, n = 0, seq = 0;
    if (e->kind == EV_ACH_REC) {
        ach_on_rec_event(e);
        return;
    }
    if (e->kind != EV_ACH_BUMP || !ach_unpack_bump(e->body, e->len, &kind, &n, &seq)) return;
    if (!ach_session() || (g_cs && cs_is_host(g_cs))) return;   /* only the client receives the host's forwarded causes */
    g_ach_n_fwd_rx++;
    if (!ach_fwd_find(kind)) return;
    if (!ach_fwd_accept(g_ach_fwd)) {
        g_ach_n_fwd_off++;
        r2_log(ACH_LOG "forwarded bump kind 0x%02X #%u ignored: forwarding is off on this PC (net ach fwd auto|1)", kind, seq);
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

/* ---------------------------------------------------------------- ASSIST_HOLD_HELP call patch (P6, static analysis only: <lab>\re2q\ach_p6b.out)
 * uEmIntel_vf22 at 0x6018D5: MOV ECX,[0x1567EAC]; 0x6018DB: CALL 0x6E6E50 (seat-0 slot) -> EAX; MOV ECX,[0x1567EAC]; PUSH EAX; CALL sPlayer_GetPlayer; CMP [ESI+0x3940],EAX; ...
 * FUN_00603EA0(0x37, player). ESI is the enemy. The call is redirected to ach_assist_stub: it calls the original with the game's ECX, then ach_assist_slot_c(enemy, seat-0 slot)
 * returns the slot to compare with (ach_logic.h ach_assist_slot). EAX / ECX / EDX may change as in the original; the game reloads ECX right after. */
#define ACH_ASSIST_SITE 0x6018D5u
#define ACH_ASSIST_CALL 0x6018DBu
#define ACH_SEAT0_FN 0x6E6E50u
extern int ach_assist_stub(void);
int __cdecl ach_assist_slot_c(unsigned enemy, int seat0)
{
    g_ach_n_assist_calls++;
    if (!g_ach_assist || !enemy || !ach_session()) return seat0;
    unsigned hit = rd32(enemy + 0x3940), pm = player_obj(g_my_slot), pr = player_obj(g_rem_slot);
    int hs = !hit ? -1 : hit == pm ? g_my_slot : hit == pr ? g_rem_slot : -1;
    int s = ach_assist_slot(1, seat0, hs);
    if (s != seat0) {
        g_ach_n_assist_moved++;
        if (g_ach_loglim > 0) {
            g_ach_loglim--;
            r2_log(ACH_LOG "assist (kind 0x37): the enemy %08x holding a player was hit by slot %d (%s); the game asked the seat-0 slot %d: answered %d", enemy, s, s == g_my_slot ? "my human" : "the partner's puppet", seat0, s);
        }
    }
    return s;
}
__asm__(".text\n"
        ".globl _ach_assist_stub\n"
        "_ach_assist_stub:\n"
        "  movl $0x6E6E50, %eax\n"
        "  call *%eax\n"               /* the original: ECX = sPlayer as the game set it; EAX = the seat-0 slot */
        "  pushl %eax\n"
        "  pushl %esi\n"               /* the enemy */
        "  call _ach_assist_slot_c\n"
        "  addl $8, %esp\n"
        "  ret\n");

static int ach_assist_install(void)
{
    if (g_ach_assist_installed) return 1;
    static const unsigned char want[6] = {0x8B, 0x0D, 0xAC, 0x7E, 0x56, 0x01};   /* MOV ECX,[0x1567EAC] before and after the call */
    unsigned char c[17];
    if (!r2_safe_read((void *)ACH_ASSIST_SITE, c, sizeof c) || memcmp(c, want, 6) != 0 || c[6] != 0xE8 || (unsigned)(ACH_ASSIST_CALL + 5 + *(int *)(c + 7)) != ACH_SEAT0_FN ||
        memcmp(c + 11, want, 6) != 0) {
        r2_log(ACH_LOG "assist call patch: the bytes at %08x are not the expected ones, NOT installed", ACH_ASSIST_SITE);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect((void *)ACH_ASSIST_CALL, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int *)(ACH_ASSIST_CALL + 1) = (int)((unsigned)ach_assist_stub - (ACH_ASSIST_CALL + 5));
    VirtualProtect((void *)ACH_ASSIST_CALL, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)ACH_ASSIST_CALL, 5);
    g_ach_assist_installed = 1;
    r2_log(ACH_LOG "assist call patch installed at %08x (ASSIST_HOLD_HELP credited to the enemy's last attacker among the two humans)", ACH_ASSIST_CALL);
    return 1;
}

/* ---------------------------------------------------------------- install */
static int ach_install(void);
/* G-ACH: one read of the local human's action id per frame (the melee-class history), and the lab trace of one slot's action changes */
static void ach_track(void)
{
    unsigned now = GetTickCount() | 1;
    unsigned p = player_obj(g_my_slot >= 0 ? g_my_slot : 0);
    if (p) {
        unsigned a = rd32(p + 0x1A54);
        if (a != g_ach_m_last_act) {
            if (ach_is_melee_act(a)) g_ach_m_act = a, g_ach_m_t0 = now, g_ach_m_t1 = 0;
            else if (ach_is_melee_act(g_ach_m_last_act) && g_ach_m_act == g_ach_m_last_act && !g_ach_m_t1) g_ach_m_t1 = now;
            ach_blowhist_on_change(&g_ach_blow, g_ach_m_last_act, a, now);
            g_ach_m_last_act = a;
        }
    }
    if (g_ach_tr_slot < 0 || g_ach_tr_left <= 0) return;
    unsigned q = player_obj(g_ach_tr_slot);
    if (!q) return;
    unsigned a = rd32(q + 0x1A54);
    if (q != g_ach_tr_obj) g_ach_tr_obj = q, g_ach_tr_act = 0xFFFFFFFFu;
    if (a == g_ach_tr_act) return;
    g_ach_tr_left--, g_ach_tr_lines++;
    r2_log(ACH_LOG "trace slot %d: action %d -> %d after %u ms (weapon %04x, hp %d, type %u)", g_ach_tr_slot, (int)g_ach_tr_act, (int)a, g_ach_tr_t ? now - g_ach_tr_t : 0, ach_weapon_class(q),
           (int)rd32(q + 0x1A08), rd32(q + 0x7920));
    g_ach_tr_act = a, g_ach_tr_t = now;
}

/* ---- lab: net ach fight <a> <b> <c> <ms> [range] [lt] [approach]: a real blow by the LOCAL human, made in-process (a console round trip is too slow for combat, traps 6c).
 * For <ms>: while the local human idles (action 2 / 3) and the enemy with that layout id is alive, the human is turned toward it with the game's own teleport (same
 * position, new yaw: what `net tp` does) and, inside <range> units (default 150), RT is pressed 250 ms (with LT held when lt = 1: aimed fire) through the pad path of the
 * bot (a session: the human's real-pad user; solo: the synthetic pad of slot 0). approach = 1 puts the human 90 units from the enemy first when it is farther. The blow
 * itself is the game's: knife lunge, physical attack, follow-up or a shot, whatever RT does in that state. Test aid only; refused while the Steam writers pass through. */
static struct { int on, a, lt, approach, phase, presses, n_log, sweep; unsigned b, c, until, t, enemy; float range; } g_ach_fight;
static void aim_cam_retarget(int a, unsigned b, unsigned c);   /* aim.inc */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } ach_gamepad_t;   /* the XINPUT_GAMEPAD layout */
static void ach_fight_pad(int rt, int lt)
{
    ach_gamepad_t g;
    memset(&g, 0, sizeof g);
    g.bRightTrigger = rt ? 255 : 0, g.bLeftTrigger = lt ? 255 : 0;
    if (g_cs && cs_active(g_cs)) testmode_pad_pretend(0, 1, &g);
    else testmode_pad_set(0, 1, &g);
}
static void ach_fight_stop(const char *why)
{
    if (!g_ach_fight.on) return;
    g_ach_fight.on = 0;
    if (g_cs && cs_active(g_cs)) testmode_pad_pretend(0, 0, NULL);
    else testmode_pad_set(0, 0, NULL);
    r2_log(ACH_LOG "fight (%d,%u,%u): stopped (%s) after %d presses", g_ach_fight.a, g_ach_fight.b, g_ach_fight.c, why, g_ach_fight.presses);
}
static void ach_fight_tick(void)
{
    if (!g_ach_fight.on) return;
    unsigned now = GetTickCount();
    if ((int)(now - g_ach_fight.until) >= 0) { ach_fight_stop("time"); return; }
    int me = g_my_slot >= 0 ? g_my_slot : 0;
    unsigned p = player_obj(me), en = w_find_active(SENEMY_CELL, g_ach_fight.a, g_ach_fight.b, g_ach_fight.c);
    if (!p) return;
    if (g_ach_fight.sweep) {   /* `net ach fight sweep`: every tick the NEAREST living enemy of the sEnemy manager (no console round trip per target; lab) */
        unsigned m = rd32(SENEMY_CELL), best = 0;
        float bd = 1e30f, qx = rdf(p + 0x40), qz = rdf(p + 0x48);
        for (int k = 0; m && k < 2; k++) {
            unsigned cnt = rd32(m + (k ? 0x44 : 0x30)), arr = rd32(m + (k ? 0x50 : 0x3C));
            for (unsigned i = 0; i < cnt && i < 64 && arr; i++) {
                unsigned o = rd32(arr + 4 * i);
                layout_t l;
                if (!o || (int)rd32(o + 0x1A08) <= 0 || !layout_of(o, &l) || l.a < 0) continue;
                float ox = rdf(o + 0x40), oz = rdf(o + 0x48);
                if (ox == 0.0f && oz == 0.0f) continue;   /* the NPC set models parked at the origin */
                float d2 = (ox - qx) * (ox - qx) + (oz - qz) * (oz - qz);
                if (d2 < bd) bd = d2, best = o;
            }
        }
        if (!best) return;
        layout_t l;
        layout_of(best, &l);
        if (l.a != g_ach_fight.a || l.b != g_ach_fight.b || l.c != g_ach_fight.c) {
            g_ach_fight.a = l.a, g_ach_fight.b = l.b, g_ach_fight.c = l.c;
            aim_cam_retarget(l.a, l.b, l.c);
            if (g_ach_fight.n_log++ < 60) r2_log(ACH_LOG "sweep: next target (%d,%u,%u) at %.0f units", l.a, l.b, l.c, sqrtf(bd));
        }
        en = best;
    }
    if (!en || (int)rd32(en + 0x1A08) <= 0) { ach_fight_stop(en ? "the enemy is dead" : "no such enemy"); return; }
    if ((int)rd32(p + 0x1A08) < 400) { int full = (int)rd32(p + 0x1A0C); if (full > 0) wr(p + 0x1A08, &full, 4); }   /* lab: keep the human standing */
    float px = rdf(p + 0x40), py = rdf(p + 0x44), pz = rdf(p + 0x48), ex = rdf(en + 0x40), ey = rdf(en + 0x44), ez = rdf(en + 0x48);
    float dx = ex - px, dz = ez - pz, d = sqrtf(dx * dx + dz * dz);
    unsigned act = rd32(p + 0x1A54);
    if (g_ach_fight.phase == 1) {   /* RT down */
        if ((int)(now - g_ach_fight.t) >= 250) ach_fight_pad(0, g_ach_fight.lt), g_ach_fight.phase = 2, g_ach_fight.t = now;
        return;
    }
    if (g_ach_fight.phase == 2 && (int)(now - g_ach_fight.t) < 350) return;   /* RT up, the blow plays */
    g_ach_fight.phase = 0;
    if (act != 2 && act != 3 && (int)(now - g_ach_fight.t) < 1500) return;   /* wait for the blow / reaction to end (at most 1.5 s) */
    if (d > g_ach_fight.range) {
        if (!g_ach_fight.approach || d < 1.0f || fabsf(ey - py) > 200.0f) return;   /* approach only on the same floor (P8: a stair target put Claire under the map) */
        float k = (d - 90.0f) / d, nx = px + dx * k, nz = pz + dz * k, yaw = atan2f(ex - nx, ez - nz), ny = ey;
        wr(p + 0x39A0, &nx, 4), wr(p + 0x39A4, &ny, 4), wr(p + 0x39A8, &nz, 4), wr(p + 0x39C4, &yaw, 4);
        unsigned a1[1] = {p};
        do_call(3, 0x710FD0, a1, 1);
        g_ach_fight.t = now;
        return;
    }
    float yaw = atan2f(dx, dz);   /* face the enemy: the game's own teleport to the same point with the new yaw */
    wr(p + 0x39A0, &px, 4), wr(p + 0x39A4, &py, 4), wr(p + 0x39A8, &pz, 4), wr(p + 0x39C4, &yaw, 4);
    unsigned a1[1] = {p};
    do_call(3, 0x710FD0, a1, 1);
    ach_fight_pad(1, g_ach_fight.lt);
    g_ach_fight.phase = 1, g_ach_fight.t = now, g_ach_fight.presses++;
    if (g_ach_fight.n_log++ < 60)
        r2_log(ACH_LOG "fight (%d,%u,%u): press %d at %.0f units, my action %u weapon %04x, enemy hp %d act %u", g_ach_fight.a, g_ach_fight.b, g_ach_fight.c, g_ach_fight.presses, d, act, ach_weapon_class(p),
               (int)rd32(en + 0x1A08), rd32(en + 0x1A54));
}

/* ---- lab: `net ach walk <ms> <stop> <x1> <z1> [<x2> <z2> ...] | stop`: the local human WALKS (real pad input, no teleport: the level loaders follow a human's own movement, a teleport to an
 * unloaded height lands in the void) along up to 8 ground waypoints. Every tick the camera is turned to the waypoint (same convention as `net aim cam`: yaw = atan2(dx, dz)) and the left
 * stick is pushed forward; a human that made less than 40 units in 2.5 s presses the action button for 150 ms (doors, ladders, vaults) every 2.5 s. Stops at the last waypoint within <stop>
 * units, after <ms>, or when the human fell (y < -500). Test aid for the Episode 2 climb after the siege (allep P13). */
static struct { int on, n, cur, presses, n_log; float x[8], z[8], stop, lx, lz; unsigned until, t_prog, t_press, t_log; int btn; } g_ach_walk;
static void ach_walk_pad(int fwd, int btn)
{
    ach_gamepad_t g;
    memset(&g, 0, sizeof g);
    g.sThumbLY = fwd ? 32000 : 0;
    g.wButtons = btn ? 0x1000 : 0;   /* XINPUT_GAMEPAD_A */
    if (g_cs && cs_active(g_cs)) testmode_pad_pretend(0, 1, &g);
    else testmode_pad_set(0, 1, &g);
}
static void ach_walk_stop(const char *why)
{
    if (!g_ach_walk.on) return;
    g_ach_walk.on = 0;
    if (g_cs && cs_active(g_cs)) testmode_pad_pretend(0, 0, NULL);
    else testmode_pad_set(0, 0, NULL);
    r2_log(ACH_LOG "walk: stopped (%s) at waypoint %d of %d, %d presses", why, g_ach_walk.cur + 1, g_ach_walk.n, g_ach_walk.presses);
}
static void ach_walk_tick(void)
{
    if (!g_ach_walk.on) return;
    unsigned now = GetTickCount();
    if ((int)(now - g_ach_walk.until) >= 0) { ach_walk_stop("time"); return; }
    int me = g_my_slot >= 0 ? g_my_slot : 0;
    unsigned p = player_obj(me), ctrl = p ? rd32(p + 0x4780) : 0;
    if (!p || !ctrl) return;
    float px = rdf(p + 0x40), py = rdf(p + 0x44), pz = rdf(p + 0x48);
    if (py < -500.0f) { ach_walk_stop("the human fell"); return; }
    float dx = g_ach_walk.x[g_ach_walk.cur] - px, dz = g_ach_walk.z[g_ach_walk.cur] - pz, d = sqrtf(dx * dx + dz * dz);
    if (d < g_ach_walk.stop) {
        if (g_ach_walk.cur + 1 >= g_ach_walk.n) { ach_walk_stop("arrived"); return; }
        g_ach_walk.cur++;
        r2_log(ACH_LOG "walk: waypoint %d reached at (%.0f,%.0f,%.0f)", g_ach_walk.cur, px, py, pz);
        return;
    }
    float yaw = atan2f(dx, dz), pitch = 0.0f;
    wr(ctrl + 0x23AC, &yaw, 4), wr(ctrl + 0x23B4, &yaw, 4), wr(ctrl + 0x23B8, &yaw, 4);
    wr(ctrl + 0x23A8, &pitch, 4), wr(ctrl + 0x23B0, &pitch, 4), wr(ctrl + 0x23BC, &pitch, 4);
    int btn = 0;
    if (!g_ach_walk.t_prog) g_ach_walk.t_prog = now, g_ach_walk.lx = px, g_ach_walk.lz = pz;
    if ((int)(now - g_ach_walk.t_prog) >= 2500) {
        float mx = px - g_ach_walk.lx, mz = pz - g_ach_walk.lz;
        if (mx * mx + mz * mz < 40.0f * 40.0f) g_ach_walk.t_press = now, g_ach_walk.presses++, r2_log(ACH_LOG "walk: stuck at (%.0f,%.0f,%.0f) heading (%.0f,%.0f): action button", px, py, pz, g_ach_walk.x[g_ach_walk.cur], g_ach_walk.z[g_ach_walk.cur]);
        g_ach_walk.t_prog = now, g_ach_walk.lx = px, g_ach_walk.lz = pz;
    }
    if (g_ach_walk.t_press && (int)(now - g_ach_walk.t_press) < 150) btn = 1;
    ach_walk_pad(1, btn);
    if ((int)(now - g_ach_walk.t_log) >= 3000 && g_ach_walk.n_log++ < 200) {
        g_ach_walk.t_log = now;
        r2_log(ACH_LOG "walk: at (%.0f,%.0f,%.0f) -> waypoint %d (%.0f,%.0f), %.0f units, my action %u", px, py, pz, g_ach_walk.cur + 1, g_ach_walk.x[g_ach_walk.cur], g_ach_walk.z[g_ach_walk.cur], d, rd32(p + 0x1A54));
    }
}

/* ---- lab: `net ach follow <ms> [near 180] [tp 1500] [fight 0|1] | stop`: the local human FOLLOWS the partner (owner-check sessions: the N100's human keeps up with the owner).
 * Every tick: health kept at >= 400 (god mode); farther than <near> units: the camera turns to the partner and the stick goes forward (real walking, the level loaders follow);
 * farther than <tp> or 300 units above / below for 4 s, or fallen 1000 below the partner: the game's teleport puts this human 100 units from the partner on the side it came from;
 * when the partner stands still within 250 units for 1.5 s the action button is pulsed every 2 s (co-op doors, shutters and pair actions need both humans); fight 1: a living enemy
 * within 400 units on the same floor is fought with the sweep (aimed fire, approach 0) and the walk waits. Test aid only. */
static struct { int on, fight, presses, tps, n_log, own_fight; float nr, tp, plx, plz; unsigned until, t_far, t_still, t_press, t_log; } g_ach_fol;
static void ach_follow_stop(const char *why)
{
    if (!g_ach_fol.on) return;
    g_ach_fol.on = 0;
    if (g_ach_fol.own_fight) ach_fight_stop("follow stopped"), g_ach_fol.own_fight = 0;
    if (g_cs && cs_active(g_cs)) testmode_pad_pretend(0, 0, NULL);
    else testmode_pad_set(0, 0, NULL);
    r2_log(ACH_LOG "follow: stopped (%s): %d teleports, %d presses", why, g_ach_fol.tps, g_ach_fol.presses);
}
static void ach_follow_tick(void)
{
    if (!g_ach_fol.on) return;
    unsigned now = GetTickCount();
    if ((int)(now - g_ach_fol.until) >= 0) { ach_follow_stop("time"); return; }
    if (g_my_slot < 0 || g_rem_slot < 0) return;
    unsigned p = player_obj(g_my_slot), q = player_obj(g_rem_slot), ctrl = p ? rd32(p + 0x4780) : 0;
    if (!p || !q || !ctrl) return;
    if ((int)rd32(p + 0x1A08) < 400) { int full = (int)rd32(p + 0x1A0C); if (full > 0) wr(p + 0x1A08, &full, 4); }   /* god mode */
    float px = rdf(p + 0x40), py = rdf(p + 0x44), pz = rdf(p + 0x48), qx = rdf(q + 0x40), qy = rdf(q + 0x44), qz = rdf(q + 0x48);
    float dx = qx - px, dz = qz - pz, d = sqrtf(dx * dx + dz * dz), dy = qy - py;
    if (g_ach_fol.fight) {   /* a living enemy near on the same floor: the sweep fights it, the walk waits */
        unsigned m = rd32(SENEMY_CELL), near_en = 0;
        for (int k = 0; m && k < 2 && !near_en; k++) {
            unsigned cnt = rd32(m + (k ? 0x44 : 0x30)), arr = rd32(m + (k ? 0x50 : 0x3C));
            for (unsigned i = 0; i < cnt && i < 64 && arr; i++) {
                unsigned o = rd32(arr + 4 * i);
                if (!o || (int)rd32(o + 0x1A08) <= 0) continue;
                float ox = rdf(o + 0x40), oy = rdf(o + 0x44), oz = rdf(o + 0x48);
                if (ox == 0.0f && oz == 0.0f) continue;
                if ((ox - px) * (ox - px) + (oz - pz) * (oz - pz) < 400.0f * 400.0f && fabsf(oy - py) < 200.0f) { near_en = o; break; }
            }
        }
        if (near_en && d < g_ach_fol.tp) {
            if (!g_ach_fight.on) {
                memset(&g_ach_fight, 0, sizeof g_ach_fight);
                g_ach_fight.on = 1, g_ach_fight.sweep = 1, g_ach_fight.a = -1, g_ach_fight.lt = 1, g_ach_fight.approach = 0, g_ach_fight.range = 400.0f, g_ach_fight.until = now + 600000u;
                g_ach_fol.own_fight = 1;
                r2_log(ACH_LOG "follow: an enemy within 400 units: fighting");
            }
            return;
        }
        if (g_ach_fol.own_fight && g_ach_fight.on) ach_fight_stop("follow: no enemy near"), g_ach_fol.own_fight = 0;
    }
    int far_now = d > g_ach_fol.tp || fabsf(dy) > 300.0f;
    if (py < qy - 1000.0f || (far_now && g_ach_fol.t_far && (int)(now - g_ach_fol.t_far) >= 4000)) {
        float k = d > 1.0f ? 100.0f / d : 0.0f, nx = qx - dx * k, nz = qz - dz * k, ny = qy, yaw = atan2f(dx, dz);
        if (d <= 1.0f) nx = qx - 100.0f, nz = qz;
        wr(p + 0x39A0, &nx, 4), wr(p + 0x39A4, &ny, 4), wr(p + 0x39A8, &nz, 4), wr(p + 0x39C4, &yaw, 4);
        unsigned a1[1] = {p};
        do_call(3, 0x710FD0, a1, 1);
        g_ach_fol.tps++, g_ach_fol.t_far = 0;
        if (g_ach_fol.n_log++ < 100) r2_log(ACH_LOG "follow: teleport %d to the partner (%.0f units away, dy %.0f): (%.0f,%.0f,%.0f)", g_ach_fol.tps, d, dy, nx, ny, nz);
        return;
    }
    if (far_now) { if (!g_ach_fol.t_far) g_ach_fol.t_far = now | 1u; } else g_ach_fol.t_far = 0;
    float mv = (qx - g_ach_fol.plx) * (qx - g_ach_fol.plx) + (qz - g_ach_fol.plz) * (qz - g_ach_fol.plz);
    if (mv > 20.0f * 20.0f) g_ach_fol.plx = qx, g_ach_fol.plz = qz, g_ach_fol.t_still = now | 1u;
    int btn = 0;
    if (d < 250.0f && g_ach_fol.t_still && (int)(now - g_ach_fol.t_still) >= 1500 && (int)(now - g_ach_fol.t_press) >= 2000)
        g_ach_fol.t_press = now | 1u, g_ach_fol.presses++;
    if (g_ach_fol.t_press && (int)(now - g_ach_fol.t_press) < 150) btn = 1;
    int fwd = d > g_ach_fol.nr;
    if (fwd) {
        float yaw = atan2f(dx, dz), pitch = 0.0f;
        wr(ctrl + 0x23AC, &yaw, 4), wr(ctrl + 0x23B4, &yaw, 4), wr(ctrl + 0x23B8, &yaw, 4);
        wr(ctrl + 0x23A8, &pitch, 4), wr(ctrl + 0x23B0, &pitch, 4), wr(ctrl + 0x23BC, &pitch, 4);
    }
    ach_walk_pad(fwd, btn);
    if ((int)(now - g_ach_fol.t_log) >= 5000 && g_ach_fol.n_log++ < 200)
        g_ach_fol.t_log = now, r2_log(ACH_LOG "follow: %.0f units from the partner (dy %.0f), %d teleports, %d presses", d, dy, g_ach_fol.tps, g_ach_fol.presses);
}

/* ---- lab: `net ach god <ms> | stop`: the local human's health is kept at >= 400 (owner-check sessions: a death of the owner's character would stop the check at the
 * death screen, which both PCs must confirm). Test aid only; nothing else changes. */
static struct { int on, fills; unsigned until; } g_ach_god;
static void ach_god_tick(void)
{
    if (!g_ach_god.on) return;
    if ((int)(GetTickCount() - g_ach_god.until) >= 0) { g_ach_god.on = 0; r2_log(ACH_LOG "god: off (time), %d refills", g_ach_god.fills); return; }
    unsigned p = g_my_slot >= 0 ? player_obj(g_my_slot) : 0;
    if (!p) return;
    int hp = (int)rd32(p + 0x1A08), full = (int)rd32(p + 0x1A0C);
    if (hp < 400 && full > 0) { wr(p + 0x1A08, &full, 4); g_ach_god.fills++; }
}

static void ach_frame(void)
{
    if (!g_ach_installed && g_cs && cs_active(g_cs)) ach_install();   /* the hooks go in with the first session, before any slot has a partner (nothing is patched for solo play) */
    ach_track();
    ach_fight_tick();
    ach_walk_tick();
    ach_follow_tick();
    ach_god_tick();
}

static int ach_install(void)
{
    if (g_ach_installed) return 1;
    if (!r2_text_decrypted()) return 0;
    char v[8];
    if (r2_env("REV2_ACHFWD", v, sizeof v) && (v[0] == '1' || v[0] == '0')) g_ach_fwd = v[0] == '1' ? ACH_FWD_ON : ACH_FWD_OFF;
    int want_assist = ach_assist_wanted(g_ach_fwd, r2_env("REV2_ACHASSIST", v, sizeof v) && v[0] == '1');
    if (r2_env("REV2_ACHF6", v, sizeof v) && (v[0] == '0' || v[0] == '1')) g_ach_f6 = v[0] == '1';
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
    static const unsigned char b_evala[] = {0x53, 0x8B, 0x5C, 0x24, 0x08};               /* push ebx; mov ebx,[esp+8] (F6) */
    if ((t = w_tramp(0x9808E0, b_evala, 5))) o_a_evala = (a_evala_t)t, ok += w_jmp(0x9808E0, 5, (void *)h_a_evala);
    /* The Em1100 context hook is OFF unless REV2_ACHEM1100=1: in the s8b1 pair run (60 s of an awake Afflicted that chased the partner's puppet) the function was never entered, so its trampoline
     * has not run live; the grapple function hook (0x6067B0) has (crows, host). Without it the Afflicted path of GRAP_COMBINATION (FUN_00603F20 from uEm1100Intel_vf62) is unguarded. */
    if (r2_env("REV2_ACHEM1100", v, sizeof v) && v[0] == '1' && (t = w_tramp(0x5EC800, b_em, 6))) o_a_em1100 = (a_em1100_t)t, ok += w_jmp(0x5EC800, 6, (void *)h_a_em1100);
    if (want_assist) ok += ach_assist_install();
    g_ach_installed = 1;
    r2_log(ACH_LOG "hooks installed: stat bump %s, per-slot bump %s, record state %s, grapple context %s, Em1100 context %s (off by default: REV2_ACHEM1100=1), assist call patch %s (REV2_ACHFWD=1 / REV2_ACHASSIST=1), kill report evaluator A %s (F6 %s); forwarding %s; Steam writers %s",
           o_a_statadd ? "yes" : "NO", o_a_statslot ? "yes" : "NO", o_a_setrec ? "yes" : "NO", o_a_grap ? "yes" : "NO", o_a_em1100 ? "yes" : "NO", g_ach_assist_installed ? "yes" : "no", o_a_evala ? "yes" : "NO",
           g_ach_f6 ? "on" : "off", ach_fwd_name(g_ach_fwd),
           steam_ach_blocking() ? "BLOCKED" : "pass through");
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
                         c.is_host ? "host" : "client", g_ach_gate ? "on" : "off", ach_fwd_name(g_ach_fwd), g_ach_dedupe_ms, g_ach_ctx ? "on" : "off");
        o += snprintf(out + o, size - o, "assist call patch %s (decision %s): %u calls, %u answered with the last attacker; records forwarded: %u sent, %u received, %u applied\n", g_ach_assist_installed ? "installed" : "not installed",
                      g_ach_assist ? "on" : "off", g_ach_n_assist_calls, g_ach_n_assist_moved, g_ach_n_rec_tx, g_ach_n_rec_rx, g_ach_n_rec_applied);
        o += snprintf(out + o, size - o, "bumps: %u credited to my human, %u of the partner undone, %u unpaired; forwarded: %u sent, %u received (%u applied, %u dropped as doubles, %u ignored with forwarding off); context guard hits %u, records suppressed %u\n",
                      g_ach_n_mine, g_ach_n_undone, g_ach_n_unpaired, g_ach_n_fwd_tx, g_ach_n_fwd_rx, g_ach_n_fwd_applied, g_ach_n_fwd_dup, g_ach_n_fwd_off, g_ach_n_ctx_supp, g_ach_n_rec_supp);
        o += snprintf(out + o, size - o, "context hooks ran: enemy grapple function %u, Em1100 intel %u times; F6 %s (window %u ms): %u reports of my kills at the host's DEATH, %u saw the last blow\n",
                      g_ach_n_grap_calls, g_ach_n_em1100_calls, g_ach_f6 ? "on" : "off", g_ach_f6_window, g_ach_n_f6_seen, g_ach_n_f6_sub);
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
        if (arg[0]) g_ach_fwd = !strcmp(arg, "auto") ? ACH_FWD_AUTO : atoi(arg) != 0 ? ACH_FWD_ON : ACH_FWD_OFF;
        if (g_ach_fwd == ACH_FWD_ON && g_ach_installed) ach_assist_install();
        snprintf(out, size, "forwarding %s (on: the host sends the partner's enemy-side bumps and swallowed records; a client replays them unless off; the host's switch decides)", ach_fwd_name(g_ach_fwd));
    } else if (!strcmp(sub, "assist")) {   /* net ach assist [0|1]: the ASSIST_HOLD_HELP call patch (1 installs it if needed) */
        if (arg[0]) g_ach_assist = atoi(arg) != 0;
        if (arg[0] && g_ach_assist && g_ach_installed) ach_assist_install();
        snprintf(out, size, "assist call patch %s, decision %d (1: the enemy's last attacker among the two humans is credited; 0: the game's seat-0 slot)", g_ach_assist_installed ? "installed" : "not installed", g_ach_assist);
    } else if (!strcmp(sub, "f6")) {   /* net ach f6 [0|1] [window ms]: the client's kill report sees its last melee blow (F6); REV2_ACHF6=0 at the start */
        int on = g_ach_f6, w = (int)g_ach_f6_window;
        if (arg[0]) sscanf(arg, "%d %d", &on, &w);
        g_ach_f6 = on != 0;
        if (w >= 0 && w <= 5000) g_ach_f6_window = (unsigned)w;
        snprintf(out, size, "F6 %s, window %u ms, evaluator A hook %s; last counted blow of my human %u (%s), %u reports seen, %u saw the blow", g_ach_f6 ? "on" : "off", g_ach_f6_window,
                 o_a_evala ? "installed" : "not installed", g_ach_blow.blow, g_ach_blow.end_ms ? (g_ach_blow.clean ? "ended, clean" : "ended, other actions since") : g_ach_blow.blow ? "running" : "none",
                 g_ach_n_f6_seen, g_ach_n_f6_sub);
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
    } else if (!strcmp(sub, "trace")) {   /* net ach trace <slot>|off [n]: log every action change of that slot's player (time, weapon class, hp), at most n lines (lab, read-only) */
        int sl = -1, n = 300;
        if (!strncmp(arg, "off", 3)) g_ach_tr_slot = -1;
        else if (sscanf(arg, "%d %d", &sl, &n) >= 1 && sl >= 0 && sl < 8) g_ach_tr_slot = sl, g_ach_tr_left = n > 0 ? n : 300, g_ach_tr_obj = 0;
        snprintf(out, size, "action trace %s%s (%d lines left, %u written); last melee-class action of my human %u (%s)", g_ach_tr_slot < 0 ? "off" : "on slot ", g_ach_tr_slot < 0 ? "" : (g_ach_tr_slot ? "1" : "0"),
                 g_ach_tr_left, g_ach_tr_lines, g_ach_m_act, g_ach_m_t1 ? "ended" : g_ach_m_act ? "running" : "none");
    } else if (!strcmp(sub, "fight")) {   /* net ach fight <a> <b> <c> <ms> [range] [lt 0|1] [approach 0|1] | stop: the local human's own blows on one enemy (lab, see ach_fight_tick) */
        int a = 0, ms = 0, lt = 0, ap = 0;
        unsigned b = 0, c = 0;
        float r = 150.0f;
        if (!strncmp(arg, "stop", 4)) {
            ach_fight_stop("console");
            snprintf(out, size, "fight stopped");
            return;
        }
        if (!steam_ach_blocking()) {
            snprintf(out, size, "refused: the Steam writers are NOT blocked (a test aid that makes real kills)");
            return;
        }
        int sweep = 0;
        if (!strncmp(arg, "sweep ", 6)) {   /* net ach fight sweep <ms> [range] [lt] [approach] */
            sweep = 1, a = -1;
            if (sscanf(arg + 6, "%d %f %d %d", &ms, &r, &lt, &ap) < 1 || ms <= 0 || ms > 1800000) {
                snprintf(out, size, "usage: net ach fight sweep <ms> [range 150] [lt 0|1] [approach 0|1]");
                return;
            }
        } else if (sscanf(arg, "%d %u %u %d %f %d %d", &a, &b, &c, &ms, &r, &lt, &ap) < 4 || ms <= 0 || ms > 600000) {
            snprintf(out, size, "usage: net ach fight <a> <b> <c> <ms> [range 150] [lt 0|1] [approach 0|1] | stop");
            return;
        }
        unsigned en = sweep ? 0 : w_find_active(SENEMY_CELL, a, b, c);
        g_ach_fight.sweep = sweep;
        g_ach_fight.on = 1, g_ach_fight.a = a, g_ach_fight.b = b, g_ach_fight.c = c, g_ach_fight.until = GetTickCount() + (unsigned)ms, g_ach_fight.range = r, g_ach_fight.lt = lt, g_ach_fight.approach = ap;
        g_ach_fight.phase = 0, g_ach_fight.t = 0, g_ach_fight.presses = 0, g_ach_fight.n_log = 0;
        ach_fight_pad(0, lt);   /* connected and neutral first (a solo synthetic pad is seen at the game's next scan) */
        snprintf(out, size, "fight (%d,%u,%u) for %d ms, range %.0f, lt %d, approach %d: enemy %08x hp %d", a, b, c, ms, r, lt, ap, en, en ? (int)rd32(en + 0x1A08) : 0);
    } else if (!strcmp(sub, "follow")) {   /* net ach follow <ms> [near 180] [tp 1500] [fight 0|1] | stop (lab, see ach_follow_tick) */
        if (!strncmp(arg, "stop", 4)) {
            ach_follow_stop("console");
            snprintf(out, size, "follow stopped");
            return;
        }
        int ms = 0, fight = 0;
        float nearv = 180.0f, tp = 1500.0f;
        if (sscanf(arg, "%d %f %f %d", &ms, &nearv, &tp, &fight) < 1 || ms <= 0 || ms > 14400000) {
            snprintf(out, size, "usage: net ach follow <ms> [near 180] [tp 1500] [fight 0|1] | stop");
            return;
        }
        ach_walk_stop("follow starts");
        memset(&g_ach_fol, 0, sizeof g_ach_fol);
        g_ach_fol.on = 1, g_ach_fol.fight = fight, g_ach_fol.nr = nearv, g_ach_fol.tp = tp, g_ach_fol.until = GetTickCount() + (unsigned)ms;
        ach_walk_pad(0, 0);
        snprintf(out, size, "follow: the partner (slot %d) for %d ms, near %.0f, teleport beyond %.0f, fight %d, god mode on", g_rem_slot, ms, nearv, tp, fight);
    } else if (!strcmp(sub, "god")) {   /* net ach god <ms> | stop (lab, see ach_god_tick) */
        int ms = 0;
        if (!strncmp(arg, "stop", 4)) {
            g_ach_god.on = 0;
            r2_log(ACH_LOG "god: off (console), %d refills", g_ach_god.fills);
            snprintf(out, size, "god mode off (%d refills)", g_ach_god.fills);
            return;
        }
        if (sscanf(arg, "%d", &ms) < 1 || ms <= 0 || ms > 14400000) {
            snprintf(out, size, "usage: net ach god <ms> | stop");
            return;
        }
        g_ach_god.on = 1, g_ach_god.fills = 0, g_ach_god.until = GetTickCount() + (unsigned)ms;
        r2_log(ACH_LOG "god: on for %d ms (my slot %d)", ms, g_my_slot);
        snprintf(out, size, "god mode on for %d ms: my human (slot %d) keeps hp >= 400", ms, g_my_slot);
    } else if (!strcmp(sub, "walk")) {   /* net ach walk <ms> <stop> <x1> <z1> [<x2> <z2> ...] | stop (lab, see ach_walk_tick) */
        if (!strncmp(arg, "stop", 4)) {
            ach_walk_stop("console");
            snprintf(out, size, "walk stopped");
            return;
        }
        int ms = 0, n = 0, used = 0, k = 0;
        float stop = 120.0f, v[16];
        if (sscanf(arg, "%d %f%n", &ms, &stop, &used) < 2 || ms <= 0 || ms > 1800000) {
            snprintf(out, size, "usage: net ach walk <ms> <stop units> <x1> <z1> [<x2> <z2> ... up to 8] | stop");
            return;
        }
        const char *q = arg + used;
        while (n < 16 && sscanf(q, " %f%n", &v[n], &k) == 1) n++, q += k;
        if (n < 2) {
            snprintf(out, size, "usage: net ach walk <ms> <stop units> <x1> <z1> [<x2> <z2> ... up to 8] | stop");
            return;
        }
        memset(&g_ach_walk, 0, sizeof g_ach_walk);
        g_ach_walk.n = n / 2, g_ach_walk.stop = stop, g_ach_walk.until = GetTickCount() + (unsigned)ms;
        for (int i = 0; i < g_ach_walk.n; i++) g_ach_walk.x[i] = v[2 * i], g_ach_walk.z[i] = v[2 * i + 1];
        g_ach_walk.on = 1;
        ach_walk_pad(0, 0);
        snprintf(out, size, "walk: %d waypoint(s), first (%.0f,%.0f), stop %.0f units, %d ms", g_ach_walk.n, g_ach_walk.x[0], g_ach_walk.z[0], stop, ms);
    } else if (!strcmp(sub, "bits")) {   /* net ach bits [rec hex ...]: record bits 0/1/2 (read-only) + the "flame bust used" bit FLAME_NOT_USED reads (story+0x20+0xCC4 bit 3, 0x404C00) */
        unsigned story = rd32(ACH_STORY_CELL);
        if (!story) {
            snprintf(out, size, "no story block");
            return;
        }
        unsigned ids[16] = {0x02, 0x49, 0x70, 0x96, 0x97, 0x72, 0x7B, 0x7D, 0x7E}, nid = 9, v;
        const char *s = arg;
        int k = 0;
        if (*s) nid = 0;
        while (nid < 16 && sscanf(s, "%x%n", &v, &k) == 1) ids[nid++] = v, s += k;
        int o = snprintf(out, size, "record bits 0/1/2:");
        for (unsigned i = 0; i < nid && o < size - 60; i++) {
            const ach_entry_t *e = ach_by_record(ids[i]);
            o += snprintf(out + o, size - o, " %02X=%d%d%d%s%s", ids[i], ach_rec_bit(story, ids[i], 0), ach_rec_bit(story, ids[i], 1), ach_rec_bit(story, ids[i], 2), e ? ":" : "", e ? e->name + 4 : "");
        }
        unsigned fw = rd32(story + 0x20 + 0xCC4);
        snprintf(out + o, size - o, "; save word +0xCC4 %08x: flame bust used (bit 3) %u; story stage %d", fw, (fw >> 3) & 1, (int)rd32(story + 0x140380));
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
        snprintf(out, size, "net ach status|totals|table|bits [rec ...]|trace <slot>|off [n]|gate [0|1]|fwd [0|1|auto]|assist [0|1]|f6 [0|1] [ms]|dedupe <ms>|ctx [0|1]|log <n>|bump <kind> <slot> [n]|set <kind> <value>|rec <record> (the last three only while the Steam writers are blocked)");
    }
}
