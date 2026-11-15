/* pmot.inc: 0.3.2 anim032, the game side of pmot_logic.h (notes\motion-mirror.md). Included from coop3.c after world.inc (it uses door.inc's SSETMODEL_CELL, world.inc's layout_of /
 * w_find_active). Everything here runs on the game thread: pm_tx_fill from sample_local, pm_rx_frame from apply_remote, pm_sm_* from door.inc's scan / event handler, pm_level_reset
 * from glue_set_state / release_puppet (coop_game_frame and the world events are all game-thread code); the network thread only stores the samples / events.
 * Console `net motion [0|1]`, `net motion sm 0|1`, `net motion hold 1|2`, `net motion layers <hex>`, `net motion` = counters; REV2_MOTION=0|1. */
#include "pmot_logic.h"

#define PM_PL_SLOT27 0x711730u      /* uPlayer vtable +0x6C (PROVEN): -> vf +0x344 0x711EF0 -> 0xAE8530 -> 0xD41FF0 */
#define PM_SM_SLOT27 0x81BB70u      /* uSetModel vtable +0x6C (PROVEN): the tracker +0x4154, -> 0xADD0B0 -> 0xD41FF0 */
#define PM_FN_STOP 0x6F5C80u        /* FUN_006F5C80(layer, keep): the player's layer stop */
#define PM_FN_ATTACH 0xD422C0u      /* FUN_00D422C0(list, bank) */
#define PM_FN_ADDREF 0xC0DCE0u      /* this = the resource */
#define PM_FN_RELEASE 0xC0E080u

static int g_pm_on = 1;             /* the player motion mirror: ON by default since the live proof a032c6 (2026-11-15: the guest's pick-up on the host's puppet, log + screenshot); `net motion 0` / REV2_MOTION=0 */
static int g_pm_sm = 1;             /* the lever / board / rock handle motion (follows the same switch; `net motion sm 0`) */
static int g_pm_hold = 2;           /* 1 = ChangeAction(0) on entry; 2 (default) = also +0x78E8 bit 3 cleared every frame of the mirror: wait's start does not clear it after a walk,
                                     * and the stock climb / fence / jump-over / ladder updates (0x6CE0E0, 0x6CF050, 0x6D0AA0, 0x6D1170) and the aim start (0x6CB9D0) clear it themselves
                                     * so that the per-frame body controller FUN_00724D30 stays off while their own motions play */
static unsigned g_pm_layers = 0xFFFFu;
static pm_tx_t g_pmt;
static pm_rx_t g_pmr;
static int g_pm_init;
static struct { unsigned list; int found; int a; unsigned b, c, k; unsigned t; } g_pm_src;   /* sender: whose motion list is in my bank 15 */
static unsigned g_pm_logs, g_pm_tx_logs, g_pm_vf_refused, g_pm_bank_ops, g_pm_bank_busy, g_pm_src_scans;
static unsigned char g_pm_rx_seen[257], g_pm_tx_seen[257];
static unsigned g_pm_ep_sets;   /* SET lines logged in the current mirror episode */
static unsigned g_pm_sm_tx, g_pm_sm_rx, g_pm_sm_applied, g_pm_sm_refused, g_pm_sm_logs;
static int g_pm_shots_n, g_pm_shots_ms = 250;   /* LAB `net motion shots <n> [ms]`: a screenshot burst when the mirror starts (both sides) or a handle plays; 0 = off (default) */
static unsigned g_pm_shots_t, g_pm_shots_taken;
static void pm_lab_burst(const char *why)
{
    unsigned now = GetTickCount();
    if (g_pm_shots_n <= 0 || (g_pm_shots_t && now - g_pm_shots_t < (unsigned)(g_pm_shots_n * g_pm_shots_ms) + 500u)) return;
    g_pm_shots_t = now ? now : 1, g_pm_shots_taken++;
    d3d_request_burst(g_pm_shots_n, g_pm_shots_ms);
    r2_log(NETLOG_PREFIX "motion: lab screenshot burst (%d x %d ms): %s", g_pm_shots_n, g_pm_shots_ms, why);
}

static unsigned pm_rd16(unsigned a)
{
    unsigned short v = 0;
    r2_safe_read((void *)a, &v, 2);
    return v;
}
static unsigned pm_rec(unsigned unit, unsigned l) { return unit + PM_LAYER_OFF + l * PM_LAYER_SIZE; }
static unsigned pm_ix(uint32_t act) { return act < 256 ? act : 256; }   /* 256 = INT_MIN / other */

/* the frame count of the unit's own motion `id` by the engine's own reads (FUN_00D41A30 / FUN_0047B500), 0 = not found */
static unsigned pm_motion_frames(unsigned unit, unsigned id)
{
    if (id == PM_NO_ID) return 0;
    unsigned list = rd32(unit + PM_BANKS_OFF + ((id >> 8) & 0xFu) * 4u);
    if (list < 0x10000u) return 0;
    unsigned tab = rd32(list + 0x68);
    if (tab < 0x10000u || (id & 0xFFu) >= pm_rd16(tab + 6)) return 0;
    unsigned mot = rd32(tab + 8 + (id & 0xFFu) * 4u);
    if (mot < 0x10000u) return 0;
    unsigned n = rd32(mot + 8);
    return n == 0 ? 1 : n > 0xFFFFu ? 0xFFFFu : n;
}
static int pm_storage_ok(unsigned unit, unsigned l) { return rd32(unit + 0x1934) == 0 || rd32(pm_rec(unit, l) + 0x13C) != 0; }

/* ---------------------------------------------------------------- sender */
/* the set model (layout id + bank) whose motion list is `list`; scanned only when the list in my bank 15 changes */
static void pm_src_find(unsigned list)
{
    if (list == g_pm_src.list) return;
    g_pm_src.list = list, g_pm_src.found = 0, g_pm_src_scans++;
    unsigned m = rd32(SSETMODEL_CELL);
    if (!list || !m) return;
    for (int k = 0; k < 2 && !g_pm_src.found; k++) {
        unsigned n = rd32(m + (k ? 0x44 : 0x30)), arr = rd32(m + (k ? 0x50 : 0x3C));
        for (unsigned i = 0; i < n && i < 160 && arr && !g_pm_src.found; i++) {
            unsigned o = rd32(arr + 4 * i);
            if (!o) continue;
            for (unsigned b = 0; b < 16; b++)
                if (rd32(o + PM_BANKS_OFF + b * 4) == list) {
                    layout_t l;
                    if (layout_of(o, &l) && l.a >= 0 && l.a <= 0x7FFF) g_pm_src.found = 1, g_pm_src.a = l.a, g_pm_src.b = l.b, g_pm_src.c = l.c, g_pm_src.k = b;
                    break;
                }
        }
    }
}

/* sample_local: the tail of my character's state while its action is one the action mirror does not replay */
static void pm_tx_fill(unsigned p, cp_state *st)
{
    st->mo_n = 0;
    if (!g_pm_on || !p) {
        g_pmt.on = 0;
        return;
    }
    uint32_t act = st->action;
    if (!pm_cont_id(act)) {   /* the common case: one compare, no reads */
        pm_tx_want(&g_pmt, g_anim_bits, act, 0, 0, 0);
        return;
    }
    unsigned nl = rd32(p + 0x494);
    if (nl > PM_LAYERS) nl = PM_LAYERS;
    int b15 = 0;
    for (unsigned l = 0; l < nl; l++) {
        unsigned id = pm_rd16(pm_rec(p, l) + 4);
        if (id != PM_NO_ID && ((id >> 8) & 0xFu) == PM_BANK_EVENT) b15 = 1;
    }
    if (b15) pm_src_find(rd32(p + PM_BANKS_OFF + PM_BANK_EVENT * 4));
    int held_enemy = (rd32(p + 0x7900) & 0x2000) != 0;
    int was_on = g_pmt.on;
    if (!pm_tx_want(&g_pmt, g_anim_bits, act, held_enemy, b15, b15 && g_pm_src.found)) return;
    if (!was_on) pm_lab_burst("my character's motions start going with the state");
    unsigned n = 0;
    for (unsigned l = 0; l < nl; l++) {
        unsigned r = pm_rec(p, l), id = pm_rd16(r + 4);
        if (id == PM_NO_ID) continue;
        cp_motion *m = &st->mo[n++];
        float nf = rdf(r + 0x54) + 1.5f, bl = rdf(r + 0x40);
        m->layer = (uint8_t)l, m->id = (uint16_t)id;
        m->nframes = (uint16_t)(nf >= 1.0f && nf < 65535.0f ? (unsigned)nf : 0);
        m->frame = rdf(r + 0x4C), m->speed = rdf(r + 0x5C), m->weight = rdf(r + 0x48);
        m->blend = (uint8_t)(bl > 0 && bl < 255.0f ? (unsigned)bl : bl >= 255.0f ? 255 : 0);
        m->flags = rd32(r + 0xC) & ~0x20000u;
        if (!isfinite(m->frame)) m->frame = 0;
        if (!isfinite(m->speed) || fabsf(m->speed) > 30.0f) m->speed = 1.0f;
        if (!isfinite(m->weight)) m->weight = 1.0f;
    }
    st->mo_n = (uint8_t)n;
    st->mo_kind = b15 && g_pm_src.found ? CP_MOK_SETMODEL : CP_MOK_PLAIN;
    st->mo_src_bank = (uint8_t)(st->mo_kind ? g_pm_src.k : 0);
    st->mo_src_a = (int16_t)(st->mo_kind ? g_pm_src.a : 0), st->mo_src_b = (uint16_t)(st->mo_kind ? g_pm_src.b : 0), st->mo_src_c = (uint16_t)(st->mo_kind ? g_pm_src.c : 0);
    unsigned ix = pm_ix(act);
    if (n && (!was_on || !g_pm_tx_seen[ix]) && g_pm_tx_logs++ < 120) {   /* every start of the tail, and every new action id within it */
        g_pm_tx_seen[ix] = 1;
        char lay[200];
        int o = 0;
        for (unsigned i = 0; i < n && o < (int)sizeof lay - 16; i++) o += snprintf(lay + o, sizeof lay - o, " %u:%04x", st->mo[i].layer, st->mo[i].id);
        if (st->mo_kind) r2_log(NETLOG_PREFIX "motion: my character's motions go with the state for action %d (%s): layers%s; bank 15 = the list of set model (%d,%u,%u) bank %u", (int)act, pa_family(act), lay, g_pm_src.a, g_pm_src.b, g_pm_src.c, g_pm_src.k);
        else r2_log(NETLOG_PREFIX "motion: my character's motions go with the state for action %d (%s): layers%s", (int)act, pa_family(act), lay);
    }
}

/* ---------------------------------------------------------------- receiver */
static void pm_ops_run(unsigned p, const pm_op_t *ops, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned a[3] = {ops[i].res, 0, 0};
        g_pm_bank_ops++;
        if (ops[i].op == PM_OP_ADDREF) do_call(2, PM_FN_ADDREF, a, 1);
        else if (ops[i].op == PM_OP_RELEASE) do_call(2, PM_FN_RELEASE, a, 1);
        else if (ops[i].op == PM_OP_ATTACH) {
            unsigned b[3] = {p, ops[i].res, PM_BANK_EVENT};
            do_call(2, PM_FN_ATTACH, b, 3);
        }
    }
}
/* bank 15 back, keep reference dropped; puppet_ok = the same live puppet */
static void pm_bank_give_back(unsigned p, int puppet_ok)
{
    pm_op_t ops[3];
    unsigned cur = puppet_ok ? rd32(p + PM_BANKS_OFF + PM_BANK_EVENT * 4) : 0;
    int n = pm_bank_end(&g_pmr.bank, puppet_ok, cur, ops);
    if (n > 0) {
        pm_ops_run(p, ops, n);
        if (g_pm_logs++ < 400) r2_log(NETLOG_PREFIX "motion: bank 15 of the puppet %08x given back (%s, %d steps: references balanced)", p, puppet_ok ? "restored" : "puppet gone: keep reference dropped", n);
    }
}

static int pm_set(unsigned p, unsigned l, unsigned id, float blend, float frame, float speed, unsigned flags)
{
    unsigned fn = rd32(rd32(p) + 0x6C);
    if (fn != PM_PL_SLOT27) {
        if (g_pm_vf_refused++ < 10) r2_log(NETLOG_PREFIX "motion: the puppet %08x's slot 27 is %08x, not the verified 0x711730: left alone", p, fn);
        return 0;
    }
    unsigned bb, fb, sb;
    memcpy(&bb, &blend, 4), memcpy(&fb, &frame, 4), memcpy(&sb, &speed, 4);
    unsigned a[7] = {p, l, id, bb, fb, sb, flags & ~0x20000u};
    do_call(2, fn, a, 7);
    return 1;
}
static void pm_weight(unsigned p, unsigned l, float w)
{
    float off = -1.0f, z = 0.0f;
    if (!(w >= 0.0f) || w > 1.0f) w = 1.0f;
    wr(pm_rec(p, l) + 0x48, &w, 4);
    wr(p + 0x701C + l * 8, &off, 4), wr(p + 0x7020 + l * 8, &z, 4);   /* no fade of the puppet's own over the owner's weight (the values vf344 writes) */
}
static void pm_stop(unsigned p, unsigned l)
{
    unsigned a[3] = {p, l, 0};
    do_call(2, PM_FN_STOP, a, 3);
}
static void pm_pup_ids(unsigned p, uint16_t ids[PM_LAYERS])
{
    for (unsigned l = 0; l < PM_LAYERS; l++) ids[l] = (uint16_t)pm_rd16(pm_rec(p, l) + 4);
}
/* a layer caches pointers into its motion (pmot_logic.h pm_ids_use_bank): the layers that play a motion of bank 15 are stopped (the reset path clears those pointers) before the
 * list loses our reference; their mirror state is forgotten, a running mirror sets them again with the new list */
static unsigned pm_stop_bank_layers(unsigned p)
{
    unsigned n = 0;
    for (unsigned l = 0; l < PM_LAYERS; l++) {
        unsigned id = pm_rd16(pm_rec(p, l) + 4);
        if (id != PM_NO_ID && ((id >> 8) & 0xFu) == PM_BANK_EVENT) pm_stop(p, l), g_pmr.set_id[l] = PM_NO_ID, g_pmr.resets[l] = 0, n++;
    }
    return n;
}
/* after the exit: bank 15 back once no layer of the (same, live) puppet plays one of its motions; after PM_BANK_DEFER_MS those layers are stopped first */
static void pm_bank_tick(unsigned p, unsigned now)
{
    if (!g_pmr.bank.active || g_pmr.on || !p || p != g_pmr.pup) return;
    uint16_t ids[PM_LAYERS];
    pm_pup_ids(p, ids);
    int due = pm_bank_due(&g_pmr, ids, now);
    if (!due) return;
    unsigned st = due == 2 ? pm_stop_bank_layers(p) : 0;
    if (st && g_pm_logs++ < 400) r2_log(NETLOG_PREFIX "motion: %u puppet layers still played bank 15 %u ms after the mirror: stopped before the give-back", st, PM_BANK_DEFER_MS);
    pm_bank_give_back(p, 1);
}

static void pm_exit(unsigned p, int quiet, uint32_t own_act)
{
    unsigned stopped = 0;
    if (!quiet)
        for (unsigned l = 4; l < PM_LAYERS; l++)
            if ((g_pmr.touched >> l & 1u) && pm_rd16(pm_rec(p, l) + 4) != PM_NO_ID) pm_stop(p, l), stopped++;
    pm_bank_tick(p, GetTickCount());   /* bank 15 now if no layer plays it, else once the puppet's own action has replaced those motions (pm_bank_due) */
    pa_guard_reset(&g_pa);   /* the action mirror switches the puppet to its owner's (mask) id this same frame */
    if (g_pm_logs++ < 400)
        r2_log(NETLOG_PREFIX "motion: mirror OFF for the puppet (slot %d), its owner is in action %d (%s)%s: set %u, re-set %u, resync %u, stopped %u (+%u layers >= 4 at the exit), gave up %u, refused motion %u / frames %u / storage %u / bank %u, holds %u + %u",
               g_rem_slot, (int)own_act, pa_family(own_act), quiet ? " (the puppet is scripted / dead / down here: not touched)" : "", g_pmr.n_set, g_pmr.n_reset, g_pmr.n_resync, g_pmr.n_stop, stopped, g_pmr.n_gaveup,
               g_pmr.n_ref_motion, g_pmr.n_ref_frames, g_pmr.n_ref_storage, g_pmr.n_ref_bank, g_pmr.n_hold, g_pmr.n_rehold);
}

/* apply_remote: 1 = the mirror runs this frame (the action replay and the HELD pose are skipped) */
static int pm_rx_frame(unsigned p, const cp_state *o)
{
    if (!g_pm_init) pm_rx_reset(&g_pmr), g_pm_init = 1;
    unsigned now = GetTickCount();
    pm_rx_in in;
    memset(&in, 0, sizeof in);
    in.pup = p, in.own_act = o->action, in.tail = o->mo_n > 0, in.enabled = g_pm_on, in.now = now;
    in.scripted = rd32(p + 0x1A54) == 0x80000000u || rd32(p + 0x1A54) == 128u;   /* a stock event / grab / scene has the puppet here, or the story carries it (uPlCarryControl copies its layers) */
    in.dead_down = (o->flags & (CP_SF_DEAD | CP_SF_DOWN)) != 0;
    in.held_enemy = (rd32(p + 0x7900) & 0x2000) != 0;   /* the puppet is held by an enemy on THIS PC */
    unsigned old = g_pmr.pup;
    int d = pm_rx_decide(&g_pmr, g_anim_bits, &in);
    if (d == PM_RX_OFF) {
        pm_bank_tick(p, now);
        return 0;
    }
    if (d == PM_RX_DROP) {   /* the old object: still a player of this game (roles changed) = alive, its bank-15 layers stopped and bank 15 given back; else only our keep reference is dropped */
        int alive = 0;
        for (int sl = 0; sl < 8 && old; sl++)
            if (player_obj(sl) == old) alive = 1;
        if (alive && g_pmr.bank.active) pm_stop_bank_layers(old);
        pm_bank_give_back(old, alive);
        if (g_pm_logs++ < 400) r2_log(NETLOG_PREFIX "motion: the puppet object changed (%08x -> %08x) while the mirror ran: its state dropped (%s)", old, p, alive ? "the old one is still a player: its bank 15 given back" : "no call on the old object");
        return 0;
    }
    if (d == PM_RX_EXIT) {
        pm_exit(p, g_pmr.exit_quiet, o->action);
        return 0;
    }
    unsigned ix = pm_ix(o->action);
    if (d == PM_RX_ENTER) pm_lab_burst("the puppet's mirror starts"), g_pm_ep_sets = 0;
    if (d == PM_RX_ENTER && g_pm_logs++ < 400)
        r2_log(NETLOG_PREFIX "motion: mirror ON for the puppet (slot %d): its owner is in action %d (%s), %u layers, kind %u%s; the puppet was in action %d, ctrl %08x", g_rem_slot, (int)o->action, pa_family(o->action), o->mo_n, o->mo_kind,
               o->mo_kind ? " (a set model's motions)" : "", (int)rd32(p + 0x1A54), rd32(p + 0x78E8));
    /* the hold: action 0 has no body-layer controller (FUN_00724D30 needs +0x78E8 bit 3) */
    if (pm_rx_hold(&g_pmr, rd32(p + 0x1A54), now)) {
        unsigned fn = rd32(rd32(p) + 0xB8);
        if (fn > 0x400000 && fn < 0x1400000) {
            unsigned args[6] = {p, 0, 1, 0, 0xFFFFFFFFu, 0};
            do_call(2, fn, args, 6);
        }
        if (g_pm_logs < 400 && (g_pmr.n_hold + g_pmr.n_rehold) <= 40)
            g_pm_logs++, r2_log(NETLOG_PREFIX "motion: the puppet held in action 0 (%s): now in %d, ctrl %08x", g_pmr.n_rehold ? "again: it had left it" : "entry", (int)rd32(p + 0x1A54), rd32(p + 0x78E8));
    }
    if (g_pm_hold >= 2) {
        unsigned c = rd32(p + 0x78E8);
        if (c & 8u) c &= ~8u, wr(p + 0x78E8, &c, 4);
    }
    int fresh = pm_rx_sample(&g_pmr, o);
    /* bank 15: the same set model's list on this PC */
    int bank_ok = 0;
    unsigned want = 0;
    for (unsigned i = 0; i < o->mo_n; i++)
        if (((o->mo[i].id >> 8) & 0xFu) == PM_BANK_EVENT && o->mo_kind == CP_MOK_SETMODEL) {
            unsigned so = w_find_active(SSETMODEL_CELL, o->mo_src_a, o->mo_src_b, o->mo_src_c);
            want = so ? rd32(so + PM_BANKS_OFF + (o->mo_src_bank & 15u) * 4u) : 0;
            break;
        }
    if (want >= 0x10000u) {
        pm_op_t ops[3];
        unsigned cur = rd32(p + PM_BANKS_OFF + PM_BANK_EVENT * 4);
        if (g_pmr.bank.active && cur == g_pmr.bank.ours && want != cur) pm_stop_bank_layers(p);   /* another set model mid-mirror: no layer may play the list that loses our reference */
        int n = pm_bank_use(&g_pmr.bank, cur, want, ops);
        if (n > 0) {
            pm_ops_run(p, ops, n);
            if (g_pm_logs++ < 400) r2_log(NETLOG_PREFIX "motion: bank 15 of the puppet now holds the motion list of set model (%d,%u,%u) bank %u (%08x; the puppet's own %08x kept)", o->mo_src_a, o->mo_src_b, o->mo_src_c, o->mo_src_bank, want, g_pmr.bank.saved);
        } else if (n < 0 && g_pm_bank_busy++ < 10)
            r2_log(NETLOG_PREFIX "motion: bank 15 of the puppet was changed by the game since our attach (%08x): not touched", cur);
        bank_ok = rd32(p + PM_BANKS_OFF + PM_BANK_EVENT * 4) == want;
    }
    for (unsigned l = 0; l < PM_LAYERS; l++) {
        const cp_motion *m = NULL;
        for (unsigned i = 0; i < o->mo_n; i++)
            if (o->mo[i].layer == l) m = &o->mo[i];
        unsigned r = pm_rec(p, l);
        pm_layer_in li;
        memset(&li, 0, sizeof li);
        li.allowed = (g_pm_layers >> l) & 1u, li.have = m != NULL, li.now = now, li.new_sample = fresh;
        li.pid = (uint16_t)pm_rd16(r + 4), li.pframe = rdf(r + 0x4C);
        if (m) {
            li.oid = m->id, li.onframes = m->nframes, li.oframe = pm_frame_now(&g_pmr, m, o->t_ms);
            li.found_frames = (uint16_t)pm_motion_frames(p, m->id), li.storage_ok = pm_storage_ok(p, l), li.bank_ok = bank_ok;
        }
        uint8_t why;
        unsigned prev_set = g_pmr.set_id[l];
        int plan = pm_layer_plan(&g_pmr, l, &li, &why);
        if (plan == PM_L_SET || plan == PM_L_RESYNC) {   /* the owner's blend for a new motion, none for a re-set / resync (the puppet is already in it) */
            if (pm_set(p, l, m->id, plan == PM_L_SET && prev_set != m->id ? (float)m->blend : 0.0f, li.oframe, m->speed, m->flags)) pm_weight(p, l, m->weight);
            if (plan == PM_L_SET && (g_pm_ep_sets++ < 8 || !g_pm_rx_seen[ix]) && g_pm_logs++ < 400)
                r2_log(NETLOG_PREFIX "motion: puppet layer %u plays its owner's motion %04x from frame %.1f (owner's action %d %s; the layer had %04x)", l, m->id, (double)li.oframe, (int)o->action, pa_family(o->action), li.pid);
        } else if (plan == PM_L_KEEP)
            pm_weight(p, l, m->weight);
        else if (plan == PM_L_STOP)
            pm_stop(p, l);
        else if (plan == PM_L_REFUSE && g_pm_logs < 400 && (g_pmr.n_ref_motion + g_pmr.n_ref_frames + g_pmr.n_ref_storage + g_pmr.n_ref_bank + g_pmr.n_gaveup) <= 40) {
            static const char *const whys[] = {"", "no such motion in the puppet's bank", "another frame count", "no layer storage", "bank 15 not the set model's list", "the puppet kept changing it back"};
            g_pm_logs++;
            r2_log(NETLOG_PREFIX "motion: puppet layer %u: the owner's motion %04x (%u frames) left alone: %s (found %u frames)", l, m ? m->id : 0xFFFFu, m ? m->nframes : 0, whys[why < 6 ? why : 0], li.found_frames);
        }
    }
    g_pm_rx_seen[ix] = 1;
    return 1;
}

/* a load / set-up / the end of the puppet: everything of ours given back while the objects are still there */
static void pm_level_reset(void)
{
    if (!g_pm_init) return;
    unsigned p = g_pmr.pup;
    int ok = p && g_rem_slot >= 0 && player_obj(g_rem_slot) == p && rd32(rd32(p) + 0x6C) == PM_PL_SLOT27;
    if (g_pmr.on || g_pmr.bank.active) {
        if (g_pmr.on && ok) {
            for (unsigned l = 4; l < PM_LAYERS; l++)
                if ((g_pmr.touched >> l & 1u) && pm_rd16(pm_rec(p, l) + 4) != PM_NO_ID) pm_stop(p, l);
        }
        if (ok && g_pmr.bank.active) pm_stop_bank_layers(p);   /* the stage's set models go now: no layer may keep playing their lists when ours is released */
        pm_bank_give_back(p, ok);
        if (g_pm_logs++ < 400) r2_log(NETLOG_PREFIX "motion: level reset: the mirror's state dropped (puppet %s)", ok ? "still there: restored" : "gone");
    }
    pm_bank_t keep = g_pmr.bank;
    pm_rx_reset(&g_pmr);
    g_pmr.bank.n_attach = keep.n_attach, g_pmr.bank.n_restore = keep.n_restore, g_pmr.bank.n_busy = keep.n_busy, g_pmr.bank.n_keep_rel = keep.n_keep_rel;
    g_pm_src.list = 0, g_pm_src.found = 0;
    pm_tx_reset(&g_pmt);
}

/* ---------------------------------------------------------------- the set model's handle (door.inc) */
/* sender: a lever / board / rock step the receiver will not replay: its layer-0 motion after the 31-byte stage-10 tail; returns the new body length (or `len` unchanged) */
static unsigned pm_sm_fill(unsigned o, int kind, int act, unsigned char *body, unsigned len, unsigned cap)
{
    if (!g_pm_sm || !pm_sm_wanted(kind, act) || cap < PM_SM_OFF + PM_SM_LEN) return len;
    unsigned r = pm_rec(o, 0), id = pm_rd16(r + 4);
    if (id == PM_NO_ID) return len;
    pm_sm_t m;
    float nf = rdf(r + 0x54) + 1.5f;
    m.id = (uint16_t)id, m.nframes = (uint16_t)(nf >= 1.0f && nf < 65535.0f ? (unsigned)nf : 0), m.frame = rdf(r + 0x4C), m.speed = rdf(r + 0x5C), m.flags = rd32(r + 0xC) & ~0x20000u;
    if (!isfinite(m.frame)) m.frame = 0;
    if (!isfinite(m.speed)) m.speed = 1.0f;
    (void)len;   /* door.inc filled all 31 bytes (byte 16 = 0: not co-op, so a receiver of any version treats the event as a plain set model's) */
    pm_sm_enc(body + PM_SM_OFF, &m);
    g_pm_sm_tx++;
    return PM_SM_OFF + PM_SM_LEN;
}
/* receiver: door.inc did not replay this step (it needs the operating character); the handle plays the sender's motion on layer 0 (no action, no operator) */
static void pm_sm_play(unsigned o, int kind, int act, const cp_event *e, int a, unsigned b, unsigned c)
{
    pm_sm_t m;
    if (!pm_sm_wanted(kind, act) || !pm_sm_dec(e->body, e->len, &m)) return;
    g_pm_sm_rx++;
    if (!g_pm_sm) return;
    const char *why = NULL;
    unsigned fn = rd32(rd32(o) + 0x6C), fr = pm_motion_frames(o, m.id);
    if (fn != PM_SM_SLOT27) why = "its slot 27 is not the verified uSetModel 0x81BB70";
    else if (!rd32(o + 0x4154)) why = "no motion tracker (+0x4154)";
    else if (!fr) why = "no such motion in its bank";
    else if (m.nframes && fr != m.nframes) why = "another frame count";
    else if (!pm_storage_ok(o, 0)) why = "no layer storage";
    if (why) {
        g_pm_sm_refused++;
        if (g_pm_sm_logs++ < 40) r2_log(W_LOG "motion: set model (%d,%u,%u) action %d: the partner's handle motion %04x left alone: %s (fn %08x, %u frames here, %u there)", a, b, c, act, m.id, why, fn, fr, m.nframes);
        return;
    }
    float blend = 0.0f;
    unsigned bb, fb, sb;
    memcpy(&bb, &blend, 4), memcpy(&fb, &m.frame, 4), memcpy(&sb, &m.speed, 4);
    unsigned args[7] = {o, 0, m.id, bb, fb, sb, m.flags};
    do_call(2, fn, args, 7);
    g_pm_sm_applied++;
    pm_lab_burst("a set model's handle plays the partner's motion");
    if (g_pm_sm_logs++ < 40)
        r2_log(W_LOG "motion: set model (%d,%u,%u) action %d not replayed, its handle plays the partner's motion %04x from frame %.1f at speed %.2f (now %04x frame %.1f)", a, b, c, act, m.id, (double)m.frame, (double)m.speed,
               pm_rd16(pm_rec(o, 0) + 4), (double)rdf(pm_rec(o, 0) + 0x4C));
}

/* ---------------------------------------------------------------- console */
static void pm_command(const char *rest, char *out, int size)
{
    while (*rest == ' ') rest++;
    if (!strncmp(rest, "dump", 4)) {   /* net motion dump [me|pup]: the 16 layers (id frame weight speed), bank 15, action, ctrl of my human or of the puppet (lab evidence) */
        const char *w = rest + 4;
        while (*w == ' ') w++;
        int pup = !strncmp(w, "pup", 3);
        unsigned p = player_obj(pup ? g_rem_slot : g_my_slot);
        if (!p || (pup && g_rem_slot < 0)) {
            snprintf(out, size, "motion dump: no %s", pup ? "puppet" : "character");
            return;
        }
        int o = snprintf(out, size, "motion dump %s %08x: action %d ctrl %08x layers %u bank15 %08x |", pup ? "pup" : "me", p, (int)rd32(p + 0x1A54), rd32(p + 0x78E8), rd32(p + 0x494), rd32(p + PM_BANKS_OFF + PM_BANK_EVENT * 4));
        for (unsigned l = 0; l < PM_LAYERS && o < size - 48; l++) {
            unsigned r = pm_rec(p, l), id = pm_rd16(r + 4);
            if (id == PM_NO_ID) continue;
            o += snprintf(out + o, size - o, " %u:%04x f%.1f/%.0f w%.2f s%.2f", l, id, (double)rdf(r + 0x4C), (double)rdf(r + 0x54) + 1.0, (double)rdf(r + 0x48), (double)rdf(r + 0x5C));
        }
        return;
    }
    if (!strncmp(rest, "shots", 5)) {
        int n = 0, ms = 250;
        if (sscanf(rest + 5, "%d %d", &n, &ms) >= 1) g_pm_shots_n = n < 0 ? 0 : n > 40 ? 40 : n, g_pm_shots_ms = ms < 60 ? 60 : ms > 2000 ? 2000 : ms;
    } else if (!strncmp(rest, "sm", 2)) {
        if (rest[2]) g_pm_sm = atoi(rest + 2) != 0;
    } else if (!strncmp(rest, "hold", 4)) {
        if (rest[4]) g_pm_hold = atoi(rest + 4) >= 2 ? 2 : 1;
    } else if (!strncmp(rest, "layers", 6)) {
        if (rest[6]) g_pm_layers = (unsigned)strtoul(rest + 6, NULL, 16) & 0xFFFFu;
    } else if (rest[0] == '0' || rest[0] == '1') {
        int v = rest[0] == '1';
        if (!v && g_pm_on && g_pmr.on) {   /* switched off while it runs: give back now */
            unsigned p = g_pmr.pup;
            if (p && g_rem_slot >= 0 && player_obj(g_rem_slot) == p) pm_exit(p, 0, g_pmr.pup ? rd32(p + 0x1A54) : 0);
            g_pmr.on = 0;
        }
        g_pm_on = v;
    }
    snprintf(out, size,
             "motion mirror %d (set models %d, hold %d, layers %04x) | sender: tails %u, on %u, off %u, enemy-held %u, unknown bank 15 %u, source scans %u | receiver: %s, entries %u, exits %u, drops %u, holds %u + %u, "
             "set %u, re-set %u, resync %u, keep %u, stopped %u, gave up %u, refused motion %u / frames %u / storage %u / bank %u, slot 27 refused %u | bank 15: attaches %u, restores %u, busy %u, keep released %u, ops %u, waits %u, forced %u, %s | "
             "handles: sent %u, received %u, played %u, refused %u | lab bursts %u (%d x %d ms)",
             g_pm_on, g_pm_sm, g_pm_hold, g_pm_layers, g_pmt.n_tails, g_pmt.n_on, g_pmt.n_off, g_pmt.n_enemy, g_pmt.n_nosrc, g_pm_src_scans, g_pmr.on ? "ON" : "off", g_pmr.n_enter, g_pmr.n_exit, g_pmr.n_drop,
             g_pmr.n_hold, g_pmr.n_rehold, g_pmr.n_set, g_pmr.n_reset, g_pmr.n_resync, g_pmr.n_keep, g_pmr.n_stop, g_pmr.n_gaveup, g_pmr.n_ref_motion, g_pmr.n_ref_frames, g_pmr.n_ref_storage, g_pmr.n_ref_bank,
             g_pm_vf_refused, g_pmr.bank.n_attach, g_pmr.bank.n_restore, g_pmr.bank.n_busy, g_pmr.bank.n_keep_rel, g_pm_bank_ops, g_pmr.n_bank_wait, g_pmr.n_bank_forced, g_pmr.bank.active ? "ours in" : "none of ours", g_pm_sm_tx, g_pm_sm_rx, g_pm_sm_applied, g_pm_sm_refused, g_pm_shots_taken, g_pm_shots_n, g_pm_shots_ms);
}
