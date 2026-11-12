/* story.inc: stage 6b, shared story state: inventories (grants and pickups), the puppet's equipment, the game's own cutscene barrier points, the flag table digest, enemy death detection.
 * Included from world.inc (after the hooks it builds on, before w_install). Design: notes\stage6.md section 6b (private), public summary docs\world.md.
 *
 * Mirror model: both PCs run the story FSM, so a scripted grant happens on both PCs by itself once the trigger fires on both; what has to be shared is (1) the triggers (flags, interactions, cutscene
 * starts), (2) what a HUMAN does to the world (pickups), (3) the personal inventories of both slots (the puppet's slot must hold what its owner holds), (4) the shared enemy state.
 *   inventory   FUN_006ECE30(player [this], out*, item id, count, uItem object, a6, a7) is the one place where every grant and pickup ends (AddItem, the story's AddWeaponToPlayer op 0x874DE0, the
 *               interactive pickup action all call it); hooked here: a pickup of the local human (object not null) -> PICKUP, a unique item without object (weapon / key item / file) -> GRANT (the receiver
 *               skips it when the slot already holds the item, so grants both FSMs make are not doubled)
 *   equipment   the puppet's weapon slots / hand follow the owner's STATE (item ids + hand slot) through FUN_00710360(player, inventory index, slot type, equip now), the Raid receiver's own call
 *   barrier     FUN_009F8350(kind) announces "I am at sync point kind" and FUN_009F3B80(kind) asks "are all peers there" in the game's own Raid code (both are no-ops outside a session):
 *               the cutscene start state machine (sCutscene FUN_008444B0) uses kind 8; here the second one waits for the other PC (BARRIER event) with a timeout
 *   flags       the host sends the story flag table every 2 s (WORLD kind 2, rising edges only on the client): late join / lost events
 */
static unsigned w_find_active(unsigned cell, int a, unsigned b, unsigned c);
static void w_sm_reset(void);   /* door.inc */
#include "invmirror_logic.h"   /* combat4: the partner's inventory copy, the GUI hand-over, the puppet's hand, the partner's throw */
static im_grace g_im_grace;     /* ids this PC itself changed in the partner's copy (story grant, PICKUP / GRANT replay, its own GUI hand-over) */
static int g_im_op;             /* inside a mirror operation of this file: no grace, no GRANT */
static int w_session_running(void);

enum { WK_FLAGS = 2, WK_ITEMS = 3 };
static int g_w_equip = 1, g_w_barrier = 1, g_w_barrier_ms = 6000, g_w_grant_sync = 1;
static int g_w_hand = 1;   /* combat4: the puppet's hand also follows an empty hand and the light (`net w story hand 0|1`) */
static unsigned g_w_hand_light, g_w_hand_cleared, g_w_hand_logs;
static unsigned g_w_put_n, g_w_put_rx, g_w_grant_tx, g_w_grant_rx, g_w_grant_skip, g_w_pick_skip, g_w_equip_calls, g_w_equip_miss;
static unsigned g_w_bar_tx, g_w_bar_rx, g_w_bar_passed, g_w_bar_timeouts, g_w_bar_since, g_w_bar_waiting, g_w_bar_wait_ms;
static unsigned g_w_dig_tx, g_w_dig_rx, g_w_dig_applied, g_w_last_digest;
static unsigned g_w_dig_last[128];
static unsigned g_w_death_tx;

/* ---------------------------------------------------------------- inventories */
static unsigned inv_personal(int slot)
{
    unsigned si = rd32(0x157D3F4);
    if (!si || slot < 0) return 0;
    unsigned arr = rd32(si + 0x418), cnt = rd32(si + 0x40C);
    return arr && (unsigned)slot < cnt ? rd32(arr + 4 * (unsigned)slot) : 0;
}

static int inv_find(int slot, unsigned id)
{
    unsigned pi = inv_personal(slot);
    if (!pi || !id) return -1;
    for (unsigned i = 0; i < 0x22; i++) {
        unsigned e = rd32(pi + 0xC + 4 * i);
        if (e && rd32(e + 4) == id) return (int)i;
    }
    return -1;
}

static int inv_count(int slot)
{
    unsigned pi = inv_personal(slot);
    int n = 0;
    for (unsigned i = 0; pi && i < 0x22; i++)
        if (rd32(pi + 0xC + 4 * i)) n++;
    return n;
}

/* ---------------------------------------------------------------- the put-into-inventory hook */
typedef void *(__attribute__((thiscall)) *putitem_t)(void *player, void *out, int item, unsigned cnt, unsigned obj, unsigned a6, unsigned a7);
static putitem_t o_putitem;

static int item_unique(unsigned id) { unsigned c = (id >> 8) & 0xFF; return c == 1 || c == 4 || c == 8; }   /* weapons, key items, files */

static __attribute__((thiscall)) void *h_putitem(void *self, void *out, int item, unsigned cnt, unsigned obj, unsigned a6, unsigned a7)
{
    const void *ret = __builtin_return_address(0);
    unsigned pl = (unsigned)self;
    int slot = w_slot_of_player(pl);
    if (g_w_on && !g_w_replay && !obj && slot >= 0 && item > 0 && item_unique((unsigned)item) && inv_find(slot, (unsigned)item) >= 0) {   /* the other PC's GRANT was here before this PC's own story got to it: not a second copy */
        if (out) memset(out, 0, 12);   /* the story op reads the object the call returns at out+8: all three fields are cleared first, as the real function does */
        g_w_grant_skip++;
        return out;
    }
    void *r = o_putitem(self, out, item, cnt, obj, a6, a7);
    unsigned char ok = 0;
    if (out) r2_safe_read(out, &ok, 1);
    g_w_put_n++;
    if (ok && slot >= 0 && slot == g_rem_slot && !g_im_op && item > 0) im_grace_note(&g_im_grace, (uint16_t)item, GetTickCount());   /* combat4: this PC's own add to the partner's copy (story, PICKUP replay) */
    layout_t l = {0, 0, 0};
    layout_of(obj, &l);
    if ((int)g_w_put_n <= g_w_log_calls * 3)
        r2_log(W_LOG "PutItem(slot %d type %u, item %04x, count %08x, obj %08x layout (%d,%u,%u), a6 %x a7 %x) -> ok %u, caller exe+%x%s", slot, pl ? rd32(pl + 0x7920) : 9, (unsigned)item, cnt, obj, l.a, l.b, l.c, a6, a7, ok,
               (unsigned)ret - 0x400000, g_w_replay ? " [replay]" : "");
    if (g_w_on && !g_w_replay && !g_w_in_additem && ok && slot >= 0 && g_cs && cs_active(g_cs) && item > 0) {   /* inside AddItem the pickup event is sent by its own hook */
        if (obj && slot == g_my_slot) {   /* a pickup of the local human from the world */
            unsigned char b[24];
            memcpy(b, &item, 4), memcpy(b + 4, &cnt, 4), memcpy(b + 8, &l.a, 4), memcpy(b + 12, &l.b, 2), memcpy(b + 14, &l.c, 2);
            b[16] = (unsigned char)slot;
            w_ev_send(EV_W_PICKUP, b, 17);
            g_w_put_tx++;
        } else if (!obj && g_w_grant_sync && item_unique((unsigned)item)) {   /* a story grant (weapon / key item / file): the other PC makes sure its copy of this slot holds it too */
            unsigned char b[16];
            memcpy(b, &item, 4), memcpy(b + 4, &cnt, 4);
            b[8] = (unsigned char)slot;
            w_ev_send(EV_W_GRANT, b, 9);
            g_w_grant_tx++;
        }
    }
    return r;
}

/* the other PC's pickup / grant: put the item into that slot's personal inventory here */
static void story_put(int slot, unsigned item, unsigned cnt, unsigned obj, const char *what)
{
    unsigned p = player_obj(slot);
    if (!p || !o_putitem) return;
    unsigned char out[16];
    memset(out, 0, sizeof out);
    g_w_replay++;
    o_putitem((void *)p, out, (int)item, cnt, obj, 0, 0);
    g_w_replay--;
    if (slot == g_rem_slot && out[0]) im_grace_note(&g_im_grace, (uint16_t)item, GetTickCount());
    r2_log(W_LOG "%s applied: slot %d item %04x count %08x object %08x -> ok %u, inventory now %d items", what, slot, item, cnt, obj, out[0], inv_count(slot));
}

/* ---------------------------------------------------------------- the puppet's equipment */
static unsigned g_eq_want[4], g_eq_t;
static void story_equip(unsigned p, int idx, int type, int now)
{
    unsigned a[4] = {p, (unsigned)idx, (unsigned)type, (unsigned)now};
    g_w_replay++;
    do_call(2, 0x710360, a, 4);   /* FUN_00710360(player, inventory index, weapon slot type, equip now): the Raid receiver's call */
    g_w_replay--;
    g_w_equip_calls++;
}

static void w_apply_equipment(unsigned p, int slot, const cp_state *o)
{
    if (!g_w_on || !g_w_equip || !p || slot < 0) return;
    unsigned now = GetTickCount();
    unsigned want[4] = {o->item[0], o->item[1], o->item[2], o->weapon};
    if (!memcmp(want, g_eq_want, sizeof want) && now - g_eq_t < 1500) return;   /* nothing changed: look again every 1.5 s */
    memcpy(g_eq_want, want, sizeof want), g_eq_t = now;
    for (int t = 1; t <= 3; t++) {
        unsigned id = o->item[t - 1];
        if (!id || equipped_item(p, t) == id) continue;
        int idx = inv_find(slot, id);
        if (idx < 0) {
            g_w_equip_miss++;
            continue;   /* the item is not in the puppet's inventory (yet): the grant events / the story put it there */
        }
        story_equip(p, idx, t, o->weapon == (unsigned)t);
    }
    unsigned a1[1] = {p};
    unsigned hand = do_call(3, 0x6FB060, a1, 1) & 0xFF;   /* FUN_006fb060: weapon slot in the hand (0 none / the default item, 1..3, 4 the light) */
    switch (g_w_hand ? eq_hand_plan(o->weapon, o->item, hand) : (o->weapon >= 1 && o->weapon <= 3 ? eq_hand_plan(o->weapon, o->item, hand) : EQ_HAND_KEEP)) {
    case EQ_HAND_SLOT: {
        int idx = inv_find(slot, o->item[o->weapon - 1]);
        if (idx >= 0) story_equip(p, idx, (int)o->weapon, 1);
        break;
    }
    case EQ_HAND_LIGHT:   /* combat4: FUN_00710360(p, x, 4, 1) puts the puppet's light (FUN_006FB7F0) into its hand */
        story_equip(p, 0, 4, 1);
        g_w_hand_light++;
        if (g_w_hand_logs++ < 20) r2_log(W_LOG "puppet hand: its owner holds the light: the light into the puppet's hand (hand slot was %u, now %u)", hand, do_call(3, 0x6FB060, a1, 1) & 0xFF);
        break;
    case EQ_HAND_DEFAULT: {   /* combat4: the owner's hand is empty (after a throw, a holster): the stock unequip FUN_00710360(p, -1, 0, 1) = the default item +0x7340 */
        unsigned def = rd32(p + 0x7340), held = rd32(p + 0x7380);
        if (def) story_equip(p, -1, 0, 1);
        else if (held) {   /* no default item: holster what the hand holds (FUN_0070C980, what FUN_006F1A30 does to the old weapon) and leave the hand empty */
            unsigned a2[2] = {p, held}, z = 0;
            g_w_replay++;
            do_call(2, 0x70C980, a2, 2);
            g_w_replay--;
            wr(p + 0x7380, &z, 4);
        }
        g_w_hand_cleared++;
        if (g_w_hand_logs++ < 20) r2_log(W_LOG "puppet hand: its owner's hand is empty: the puppet's hand slot %u cleared (%s, default item %08x, held %08x): hand slot now %u", hand, def ? "stock unequip to the default item" : "holstered", def, held,
                                        do_call(3, 0x6FB060, a1, 1) & 0xFF);
        break;
    }
    default:
        break;
    }
}


/* ---------------------------------------------------------------- combat4: the partner's inventory copy mirrors its owner (invmirror_logic.h, notes\combat4.md)
 * The owner (every PC for its own human) sends its entries when they change (checked every 100 ms) and a full set on request; the other PC keeps the table and
 * reconciles its copy of the partner's slot every 250 ms (SET a count, ADD with PutItem, DEL with FUN_00993BC0, USE1 with FUN_007151A0, weapons never removed).
 * The inventory screen of this PC's human (uGUIInventoryCampaign vtable slot 5 = 0x8FA590, every state of the screen runs inside it) may change the partner's copy:
 * the stock hand-over; that difference goes to the owner (INVADJ) and is applied there to the real items. */
enum { EV_W_INV = 0x131, EV_W_INVREQ = 0x132, EV_W_INVADJ = 0x133 };
static int g_im_on = 1, g_im_log = 200;
static im_inv g_im_sent;
static int g_im_full_tx = 1;
static unsigned g_im_owner_t, g_im_rec_t, g_im_req_t, g_im_gui_t, g_im_batt_t, g_im_batt_held;
static im_table g_im_tab;
static uint8_t g_im_seq;
static unsigned g_im_lastlog[64];   /* per id (& 63): the last log line of a plain count change (Moira's light drains / recharges every 100 ms: run combat4-b) */
static int im_log_ok(unsigned id, int count_only)
{
    unsigned now = GetTickCount(), *t = &g_im_lastlog[id & 63];
    if (count_only && *t && now - *t < 2000) return 0;
    *t = now | 1;
    return 1;
}
static unsigned g_im_tx_ev, g_im_tx_full, g_im_rx_ev, g_im_rx_stale, g_im_req_tx, g_im_req_rx, g_im_opn[8], g_im_add_fail, g_im_deferred, g_im_adj_tx, g_im_adj_rx, g_im_gui_calls, g_im_logs, g_im_keepw_logged;

static void im_read(int slot, im_inv *v)
{
    memset(v, 0, sizeof *v);
    unsigned pi = inv_personal(slot), arr[IM_N];
    if (!pi || !r2_safe_read((void *)(pi + 0xC), arr, sizeof arr)) return;
    for (unsigned i = 0; i < IM_N; i++) {
        unsigned e = arr[i], f[6];   /* +4 id, +8, +0xC count, +0x10 stack limit, +0x14, +0x18 */
        if (!e || !r2_safe_read((void *)(e + 4), f, sizeof f) || !f[0] || f[0] > 0xFFFF) continue;
        v->e[i].id = (uint16_t)f[0], v->e[i].count = (int32_t)f[2], v->e[i].max = (int32_t)f[3], v->e[i].p18 = f[5];
    }
}

static int im_fmt(char *o, int size, const im_inv *v, const uint8_t *idx, int n)
{
    int k = 0;
    o[0] = 0;
    for (int i = 0; i < n && k < size - 24; i++) {
        const im_ent *e = &v->e[idx[i] % IM_N];
        k += snprintf(o + k, size - k, " [%u]%04x x%d", idx[i], e->id, e->id ? (int)e->count : 0);
    }
    return k;
}

static void im_send(int full)
{
    im_inv cur;
    im_read(g_my_slot, &cur);
    uint8_t idx[IM_N], b[IM_EV_MAX];
    int n = full ? im_full_list(&cur, idx) : im_changed(&g_im_sent, &cur, idx);
    if (!full && !n) return;
    unsigned tnow = GetTickCount();
    if (!full && !im_send_now(&g_im_sent, &cur, idx, n, tnow, g_im_batt_t)) {   /* a small battery-only change: later (g_im_sent keeps the old value) */
        g_im_batt_held++;
        return;
    }
    if (full || im_has_battery(&cur, idx, n)) g_im_batt_t = tnow | 1;
    for (int s0 = 0; s0 < n || (full && s0 == 0); s0 += (int)IM_EV_ENTS) {
        unsigned cnt = (unsigned)(n - s0) > IM_EV_ENTS ? IM_EV_ENTS : (unsigned)(n - s0), flags = 0;
        if (full && s0 == 0) flags |= IM_F_BEGIN;
        if (full && s0 + (int)IM_EV_ENTS >= n) flags |= IM_F_END;
        w_ev_send(EV_W_INV, b, im_encode(b, g_my_slot, flags, g_im_seq++, &cur, idx + s0, cnt));
        g_im_tx_ev++;
    }
    int count_only = !full && n == 1 && g_im_sent.e[idx[0]].id == cur.e[idx[0]].id && cur.e[idx[0]].id;
    if (full) g_im_tx_full++;
    g_im_sent = cur;
    if (g_im_logs < (unsigned)g_im_log && im_log_ok(n == 1 ? cur.e[idx[0]].id : 0, count_only)) {
        char t[600];
        im_fmt(t, sizeof t, &cur, idx, n);
        g_im_logs++;
        r2_log(W_LOG "INV sent (%s, my human slot %d, %d entries):%s", full ? "full set" : "change", g_my_slot, n, t);
    }
}

static void im_reconcile(unsigned now)
{
    unsigned p = player_obj(g_rem_slot), pi = inv_personal(g_rem_slot);
    if (!p || !pi || !g_im_tab.have || p == player_obj(g_my_slot)) return;
    im_inv loc;
    im_read(g_rem_slot, &loc);
    im_op ops[24];
    int n = im_plan(&g_im_tab.inv, &loc, &g_im_grace, now, ops, 24);
    for (int i = 0; i < n; i++) {
        const im_op *o = &ops[i];
        unsigned e = o->idx < IM_N ? rd32(pi + 0xC + 4 * o->idx) : 0, ok = 0;
        int cur = e ? (int)rd32(e + 0xC) : 0;
        if ((o->op == IM_OP_USE1 || o->op == IM_OP_DEL) && im_is_weapon(o->id)) {   /* the puppet's hand must not hold the object that goes away: the hand rule clears it first */
            unsigned a1[1] = {p};
            unsigned hand = do_call(3, 0x6FB060, a1, 1) & 0xFF;
            if (hand >= 1 && hand <= 3 && equipped_item(p, (int)hand) == o->id) {
                g_im_deferred++;
                continue;
            }
        }
        g_im_op++;
        switch (o->op) {
        case IM_OP_SET:
            if (e && rd32(e + 4) == o->id) ok = wr(e + 0xC, &o->n, 4);
            break;
        case IM_OP_ADD: {
            unsigned char out[16];
            memset(out, 0, sizeof out);
            g_w_replay++;
            o_putitem((void *)p, out, (int)o->id, (unsigned)o->n, 0, o->p18, 0);
            g_w_replay--;
            ok = out[0];
            if (!ok) g_im_add_fail++, im_grace_note(&g_im_grace, o->id, now);   /* no room / refused: try again after the grace */
            break;
        }
        case IM_OP_DEL:
            if (e && rd32(e + 4) == o->id) {
                unsigned a[3] = {pi, e, 1};
                do_call(2, 0x993BC0, a, 3);   /* FUN_00993BC0(entry, 1): what the GUI's use and FUN_00995170 do with an empty stack */
                ok = 1;
            }
            break;
        case IM_OP_USE1: {
            unsigned a[2] = {p, o->id};
            do_call(2, 0x7151A0, a, 2);   /* FUN_007151A0(id): the end of a throw (count - 1, removed below 1, the sub-weapon pointer refreshed) */
            ok = 1;
            break;
        }
        case IM_OP_KEEPW:
            if (g_im_keepw_logged++ < 10) r2_log(W_LOG "INV mirror: the partner (slot %d) no longer has weapon %04x, my copy keeps it (weapons are never removed by the mirror)", g_rem_slot, o->id);
            break;
        }
        g_im_op--;
        g_im_opn[o->op & 7]++;
        if (o->op != IM_OP_KEEPW && g_im_logs < (unsigned)g_im_log && im_log_ok(o->id, o->op == IM_OP_SET)) {
            static const char *const names[] = {"?", "SET", "ADD", "DEL", "USE1", "KEEPW", "?", "?"};
            int was = o->op == IM_OP_ADD ? (int)im_total(&loc, o->id) : cur;
            int now_v = o->op == IM_OP_ADD ? was + (o->n < 0 ? 1 : (int)o->n) : o->op == IM_OP_SET ? (int)o->n : o->op == IM_OP_USE1 ? cur - 1 : 0;
            g_im_logs++;
            r2_log(W_LOG "INV mirror: the partner's (slot %d) copy %s item %04x entry [%u] %d -> %d%s (owner's total %d, my copy's before %d)", g_rem_slot, names[o->op & 7], o->id, o->idx, was, now_v, ok ? "" : " FAILED",
                   (int)im_total(&g_im_tab.inv, o->id), (int)im_total(&loc, o->id));
        }
    }
}

/* the owner: the partner's inventory screen changed my human's items */
static void im_apply_adj(const cp_event *e)
{
    im_op a[9];
    int slot = -1, n = im_adj_decode(e->body, e->len, &slot, a, 9);
    unsigned p = player_obj(g_my_slot), pi = inv_personal(g_my_slot);
    if (n <= 0 || slot != g_my_slot || !p || !pi) return;
    g_im_adj_rx++;
    for (int i = 0; i < n; i++) {
        im_inv v;
        im_read(g_my_slot, &v);
        int before = (int)im_total(&v, a[i].id);
        if (a[i].n > 0) {
            unsigned char out[16];
            memset(out, 0, sizeof out);
            g_w_replay++;
            o_putitem((void *)p, out, (int)a[i].id, (unsigned)a[i].n, 0, a[i].p18, 0);
            g_w_replay--;
        } else if (!im_is_weapon(a[i].id)) {
            unsigned args[3] = {pi, a[i].id, (unsigned)-a[i].n};
            do_call(2, 0x995170, args, 3);   /* FUN_00995170(id, n): remove n of an id over the stacks */
        } else if (im_is_throwable(a[i].id)) {
            for (int k = 0; k < -a[i].n; k++) {
                unsigned args[2] = {p, a[i].id};
                do_call(2, 0x7151A0, args, 2);
            }
        }
        im_read(g_my_slot, &v);
        r2_log(W_LOG "INV hand-over from the partner's inventory screen applied to my human (slot %d): item %04x %+d: %d -> %d", g_my_slot, a[i].id, (int)a[i].n, before, (int)im_total(&v, a[i].id));
    }
}

typedef void(__attribute__((thiscall)) *invgui_t)(void *self);
static invgui_t o_invgui;
static int im_gui_track_ok(void) { return g_im_on && g_w_on && g_cs && cs_active(g_cs) && g_rem_slot >= 0 && g_rem_slot != g_my_slot && w_session_running() && inv_personal(g_rem_slot); }
/* after an inventory-screen pass (or the LAB hand-over below): what changed in the partner's copy goes to its owner */
static void im_pw_resync(void);
static int im_adj_out(const im_op *adj, int n, unsigned self, const char *who);
static int im_gui_after(const im_inv *before, unsigned self, const char *who)
{
    im_inv after;
    im_read(g_rem_slot, &after);
    im_op adj[9];
    int n = im_gui_delta(before, &after, adj, 9);
    im_pw_resync();   /* flashlive: the per-frame pass watcher must not count this difference again */
    if (n <= 0) return 0;
    return im_adj_out(adj, n, self, who);
}
static int im_adj_out(const im_op *adj, int n, unsigned self, const char *who)
{
    unsigned char b[IM_EV_MAX];
    w_ev_send(EV_W_INVADJ, b, im_adj_encode(b, g_rem_slot, adj, (unsigned)n));
    g_im_adj_tx++;
    unsigned now = GetTickCount();
    char t[300];
    int k = 0;
    t[0] = 0;
    for (int i = 0; i < n && k < (int)sizeof t - 16; i++) {
        im_grace_note(&g_im_grace, adj[i].id, now);
        k += snprintf(t + k, sizeof t - k, " %04x %+d", adj[i].id, (int)adj[i].n);
    }
    r2_log(W_LOG "INV hand-over in %s (%08x): the partner's (slot %d) items changed:%s; sent to its owner", who, self, g_rem_slot, t);
    return n;
}
static __attribute__((thiscall)) void h_invgui(void *self)
{
    int track = im_gui_track_ok();
    im_inv before;
    if (track) im_read(g_rem_slot, &before);
    o_invgui(self);
    g_im_gui_calls++, g_im_gui_t = GetTickCount() | 1;
    if (track) im_gui_after(&before, (unsigned)self, "my inventory screen");
}
/* LAB (`net w invm give <hex id> [n]`): the inventory screen's own hand-over calls on my human's item, inside the same tracking: a full stack FUN_009938F0 (a copy into the
 * partner's inventory) + FUN_00993BC0 (removed from mine) as at 0x8F5185 / 0x8F5191; part of a stack FUN_009934B0(partner, id, n) + my count - n as at 0x8F51A7 */
static void im_lab_give(unsigned id, int n, char *out, int size)
{
    unsigned pm = inv_personal(g_my_slot), pp = inv_personal(g_rem_slot);
    int idx = inv_find(g_my_slot, id);
    if (!im_gui_track_ok() || !pm || !pp || idx < 0) {
        snprintf(out, size, "LAB give: no session / inventory / item %04x in my slot %d (index %d)", id, g_my_slot, idx);
        return;
    }
    unsigned e = rd32(pm + 0xC + 4 * (unsigned)idx);
    int cnt = (int)rd32(e + 0xC);
    if (n <= 0 || n > cnt) n = cnt;
    im_inv before;
    im_read(g_rem_slot, &before);
    if (n >= cnt) {
        unsigned a1[2] = {pp, e}, a2[3] = {pm, e, 1};
        do_call(2, 0x9938F0, a1, 2);
        do_call(2, 0x993BC0, a2, 3);
    } else {
        unsigned a1[5] = {pp, id, (unsigned)n, 0, 0};
        do_call(2, 0x9934B0, a1, 5);
        int left = cnt - n;
        wr(e + 0xC, &left, 4);
    }
    int sent = im_gui_after(&before, 0, "the LAB hand-over");
    snprintf(out, size, "LAB give %04x x%d (of %d) to the partner (slot %d): ADJ sent for %d ids; partner's copy now %d, mine %d", id, n, cnt, g_rem_slot, sent, inv_find(g_rem_slot, id) >= 0 ? (int)rd32(rd32(pp + 0xC + 4 * (unsigned)inv_find(g_rem_slot, id)) + 0xC) : 0,
             inv_find(g_my_slot, id) >= 0 ? (int)rd32(rd32(pm + 0xC + 4 * (unsigned)inv_find(g_my_slot, id)) + 0xC) : 0);
}

/* flashlive (CH1): the per-frame pass watcher (invmirror_logic.h im_pass_delta): the real inventory screen's pass runs outside the vf5 bracket of h_invgui */
static im_inv g_im_pw_mine, g_im_pw_part;
static int g_im_pw_have;
static unsigned g_im_pw_found;
static void im_pw_resync(void)
{
    if (!im_gui_track_ok()) { g_im_pw_have = 0; return; }
    im_read(g_my_slot, &g_im_pw_mine), im_read(g_rem_slot, &g_im_pw_part), g_im_pw_have = 1;
}
static void im_pw_tick(void)
{
    if (!im_gui_track_ok()) { g_im_pw_have = 0; return; }
    im_inv m, pt;
    im_read(g_my_slot, &m), im_read(g_rem_slot, &pt);
    if (g_im_pw_have) {
        im_op adj[9];
        int n = im_pass_delta(&g_im_pw_mine, &m, &g_im_pw_part, &pt, adj, 9);
        if (n > 0) g_im_pw_found++, im_adj_out(adj, n, 0, "my inventory screen (pass watcher)");
    }
    g_im_pw_mine = m, g_im_pw_part = pt, g_im_pw_have = 1;
}

static void story_inv_tick(void)
{
    if (!g_im_on || !w_session_running() || g_my_slot < 0 || g_rem_slot < 0 || g_my_slot == g_rem_slot) return;
    unsigned now = GetTickCount();
    if (now - g_im_owner_t >= 100) {
        g_im_owner_t = now;
        int full = g_im_full_tx;
        g_im_full_tx = 0;
        im_send(full);
    }
    if (!g_im_tab.have && now - g_im_req_t >= 2000) {
        unsigned char b[1] = {(unsigned char)g_my_slot};
        g_im_req_t = now;
        if (cs_send_event(g_cs, EV_W_INVREQ, b, 1)) g_im_req_tx++;
    }
    im_pw_tick();   /* flashlive: before the reconcile, which would take a pass back */
    if (g_im_tab.have && now - g_im_rec_t >= 250) {
        g_im_rec_t = now;
        im_reconcile(now);
        if (g_im_pw_have) im_read(g_rem_slot, &g_im_pw_part);   /* the mirror's own operations are not a hand-over */
    }
}

static void im_status(char *out, int size)
{
    snprintf(out, size,
             "inventory mirror %s: sent %u events (%u full sets, battery-only changes held %u), received %u (stale epoch %u, table %s, %u full sets, %u dropped, %u bad), requests tx %u rx %u; ops on the partner's copy: set %u add %u del %u use1 %u keep-weapon %u, add failed %u, deferred %u; GUI hand-overs sent %u received %u (screen calls %u, pass watcher %u); puppet hand: light %u cleared %u",
             g_im_on ? "on" : "off", g_im_tx_ev, g_im_tx_full, g_im_batt_held, g_im_rx_ev, g_im_rx_stale, g_im_tab.have ? "complete" : "not yet", g_im_tab.n_full, g_im_tab.n_dropped, g_im_tab.n_bad, g_im_req_tx, g_im_req_rx, g_im_opn[IM_OP_SET],
             g_im_opn[IM_OP_ADD], g_im_opn[IM_OP_DEL], g_im_opn[IM_OP_USE1], g_im_opn[IM_OP_KEEPW], g_im_add_fail, g_im_deferred, g_im_adj_tx, g_im_adj_rx, g_im_gui_calls, g_im_pw_found, g_w_hand_light, g_w_hand_cleared);
}

/* ---------------------------------------------------------------- the game's sync points (cutscene start barrier) */
typedef void(__attribute__((thiscall)) *syncset_t)(void *self, unsigned kind);
typedef unsigned(__attribute__((thiscall)) *synccheck_t)(void *self, unsigned kind);
static syncset_t o_syncset;
static synccheck_t o_synccheck;

static int w_session_running(void) { return g_w_on && g_cs && cs_active(g_cs) && cs_phase(g_cs) == CP_PHASE_PLAYING && g_state == G_RUN; }

static __attribute__((thiscall)) void h_syncset(void *self, unsigned kind)
{
    o_syncset(self, kind);
    if ((kind & 0xFF) == 8 && g_w_barrier && w_session_running()) {
        unsigned char b[8];
        b[0] = 8;
        w_ev_send(EV_W_BARRIER, b, 1);
        g_w_bar_tx++;
        g_w_bar_since = GetTickCount();
        g_w_bar_waiting = 1;
        r2_log(W_LOG "cutscene sync point %u reached here (tx %u, remote arrivals %u, passed %u)", kind & 0xFF, g_w_bar_tx, g_w_bar_rx, g_w_bar_passed);
    }
}

static __attribute__((thiscall)) unsigned h_synccheck(void *self, unsigned kind)
{
    if ((kind & 0xFF) != 8 || !g_w_barrier || !w_session_running() || !g_w_bar_waiting) return o_synccheck(self, kind);
    unsigned now = GetTickCount();
    int open = g_w_bar_rx > g_w_bar_passed;
    if (!open && now - g_w_bar_since > (unsigned)g_w_barrier_ms) {
        g_w_bar_timeouts++;
        g_w_bar_passed = g_w_bar_rx;   /* a late arrival of the other PC must not open the next barrier */
        open = 2;
        r2_log(W_LOG "cutscene barrier timed out after %u ms: going on alone", now - g_w_bar_since);
    }
    if (!open) return 0;
    if (open == 1) g_w_bar_passed++;
    g_w_bar_waiting = 0;
    g_w_bar_wait_ms = now - g_w_bar_since;
    r2_log(W_LOG "cutscene barrier open after %u ms (remote arrivals %u)", g_w_bar_wait_ms, g_w_bar_rx);
    return o_synccheck(self, kind);
}

/* ---------------------------------------------------------------- flag table digest (host -> client, rising edges) */
static void story_send_digest(void)
{
    unsigned now = GetTickCount();
    if (now - g_w_last_digest < 2000) return;
    g_w_last_digest = now;
    unsigned story = rd32(0x157D120);
    if (!story) return;
    for (unsigned part = 0; part < 2; part++) {   /* 2 x 64 dwords = bits 0..4095 */
        unsigned char buf[2 + 64 * 4];
        buf[0] = (unsigned char)(part * 64), buf[1] = 64;
        unsigned any = 0;
        for (unsigned i = 0; i < 64; i++) {
            unsigned v = rd32(story + 0xEF0 + 4 * (part * 64 + i));
            memcpy(buf + 2 + 4 * i, &v, 4), any |= v;
        }
        if (any && cs_send_world(g_cs, WK_FLAGS, buf, sizeof buf)) g_w_dig_tx++;
    }
}

static void story_apply_digest(const unsigned char *p, unsigned len)
{
    if (len < 2 || !o_flagset) return;
    unsigned first = p[0], n = p[1], story = rd32(0x157D120);
    if (!story || n > 64 || len < 2 + 4 * n || first + n > 128) return;
    g_w_dig_rx++;
    for (unsigned i = 0; i < n; i++) {
        unsigned v;
        memcpy(&v, p + 2 + 4 * i, 4);
        unsigned rising = v & ~g_w_dig_last[first + i];
        g_w_dig_last[first + i] = v;
        for (unsigned bit = 0; bit < 32 && rising; bit++) {
            if (!(rising & (1u << bit))) continue;
            unsigned id = (first + i) * 32 + bit;
            if (!w_flag_get(story, id)) {
                g_w_replay++;
                o_flagset((void *)story, id, 1);
                g_w_replay--;
                g_w_dig_applied++;
                r2_log(W_LOG "flag %u (0x%x) set from the host's table digest", id, id);
            }
        }
    }
}

/* ---------------------------------------------------------------- enemy death detection (host): HP crossing zero inside the enemy's update */
typedef struct { unsigned obj; int hp; int killer; unsigned f28, f2c; unsigned char sent, used; } wdeath_t;
static wdeath_t g_wdeath[WREC_LIVE_MAX];   /* 0.3.1 R2: as many as the host lists (was 48) */

static wdeath_t *wdeath_slot(unsigned obj)
{
    wdeath_t *free_slot = NULL;
    for (int i = 0; i < (int)WREC_LIVE_MAX; i++) {
        if (g_wdeath[i].used && g_wdeath[i].obj == obj) return &g_wdeath[i];
        if (!g_wdeath[i].used && !free_slot) free_slot = &g_wdeath[i];
    }
    if (!free_slot) {   /* table full: drop the oldest finished one */
        for (int i = 0; i < (int)WREC_LIVE_MAX; i++)
            if (g_wdeath[i].sent) {
                free_slot = &g_wdeath[i];
                break;
            }
    }
    if (free_slot) memset(free_slot, 0, sizeof *free_slot), free_slot->used = 1, free_slot->obj = obj, free_slot->hp = 0x7FFFFFFF, free_slot->killer = -1;
    return free_slot;
}

static void story_note_hit(unsigned obj, int aslot, unsigned f28, unsigned f2c)
{
    wdeath_t *d = wdeath_slot(obj);
    if (d && aslot >= 0) d->killer = aslot, d->f28 = f28, d->f2c = f2c;
}

static void story_scan_deaths(unsigned *list, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned o = list[i];
        wdeath_t *d = wdeath_slot(o);
        if (!d) continue;
        int hp = (int)rd32(o + 0x1A08);
        if (d->hp > 0 && hp <= 0 && !d->sent) {
            layout_t l;
            layout_of(o, &l);
            unsigned char b[24];
            memcpy(b, &l.a, 4), memcpy(b + 4, &l.b, 2), memcpy(b + 6, &l.c, 2);
            b[8] = (unsigned char)(d->killer < 0 ? 255 : d->killer);
            unsigned act = rd32(o + 0x1A54);
            memcpy(b + 9, &act, 4), memcpy(b + 13, &d->f28, 4), memcpy(b + 17, &d->f2c, 4);
            w_ev_send(EV_W_DEATH, b, 21);
            d->sent = 1;
            g_w_death_tx++;
            r2_log(W_LOG "DEATH sent (scan): layout (%d,%u,%u), killer slot %d, action %u", l.a, l.b, l.c, d->killer, act);
        }
        d->hp = hp;
    }
}

/* ---------------------------------------------------------------- scene (demo) starts
 * Every real-time scene the story plays is a request struct handed to one of three sDemo submit functions (thiscall on [0x1581788]: FUN_00A9FB30 mini demo, FUN_00A9F720 linked demo, FUN_00A9F960 type 3);
 * the story FSM builds it from the scenario parameters (FUN_00876DF0, FUN_00874A60, FUN_00876640). Both PCs run the FSM, so most scenes start on both by themselves; a scene that only one PC's trigger
 * starts (an interaction of its human) is announced here with the request bytes (SCENE, chunked over the reliable events) and started through the same submit function on the other PC, unless the
 * other PC submitted the same request in the last 5 s (hash of the bytes). sDemo refuses a second request while one plays, which also dedupes. */
typedef unsigned(__attribute__((thiscall)) *demosub_t)(void *self, void *req);
static demosub_t o_demosub[3];
static const unsigned demosub_va[3] = {0xA9FB30, 0xA9F720, 0xA9F960};
static int g_w_scene = 1;
static unsigned g_scene_last_t;
static unsigned g_w_scene_len = 0xC8, g_scene_tx, g_scene_rx, g_scene_applied, g_scene_skip, g_scene_id;
static struct { unsigned hash, t; } g_scene_recent[8];

static unsigned scene_hash(const unsigned char *p, unsigned n)
{
    unsigned h = 2166136261u;
    for (unsigned i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int scene_recent_has(unsigned h)
{
    unsigned now = GetTickCount();
    for (int i = 0; i < 8; i++)
        if (g_scene_recent[i].hash == h && g_scene_recent[i].t && now - g_scene_recent[i].t < 5000) return 1;
    return 0;
}

static void scene_recent_add(unsigned h)
{
    static int next;
    g_scene_recent[next & 7].hash = h, g_scene_recent[next & 7].t = GetTickCount() | 1, next++;
}

static unsigned demosub_common(int idx, void *self, void *req)
{
    unsigned r = o_demosub[idx](self, req);
    if (!g_w_on || g_w_replay || !g_w_scene || !req || r == 0xFFFFFFFFu) return r;
    unsigned char buf[0x400];
    unsigned n = g_w_scene_len > 0x400 ? 0x400 : g_w_scene_len;
    if (!r2_safe_read(req, buf, n)) return r;
    unsigned h = scene_hash(buf, n);
    g_scene_last_t = GetTickCount() | 1;
    char stk[300] = "";
    probe_stack((unsigned)__builtin_frame_address(0) + 4, stk, sizeof stk);
    r2_log(W_LOG "scene submit %d (request %08x, vtable %08x, %u bytes, hash %08x) -> %u%s, stack:%s", idx, (unsigned)req, rd32((unsigned)req), n, h, r, g_cs && cs_active(g_cs) ? "" : " [no session]", stk);
    scene_recent_add(h);
    if (!g_cs || !cs_active(g_cs)) return r;
    unsigned id = ++g_scene_id & 0xFF, total = (n + 79) / 80;
    for (unsigned c = 0; c < total; c++) {
        unsigned char b[96];
        unsigned len = n - c * 80 > 80 ? 80 : n - c * 80;
        b[0] = (unsigned char)idx, b[1] = (unsigned char)total, b[2] = (unsigned char)c, b[3] = (unsigned char)id;
        memcpy(b + 4, buf + c * 80, len);
        w_ev_send(EV_W_SCENE, b, 4 + len);
    }
    g_scene_tx++;
    return r;
}
static __attribute__((thiscall)) unsigned h_demo0(void *self, void *req) { return demosub_common(0, self, req); }
static __attribute__((thiscall)) unsigned h_demo1(void *self, void *req) { return demosub_common(1, self, req); }
static __attribute__((thiscall)) unsigned h_demo2(void *self, void *req) { return demosub_common(2, self, req); }

static void scene_on_chunk(const cp_event *e)
{
    static unsigned char asm_buf[0x400 + 0x100];
    static unsigned asm_mask, asm_id = 0x1000, asm_total;
    if (e->len < 5 || !g_w_scene) return;
    const unsigned char *b = e->body;
    unsigned idx = b[0], total = b[1], c = b[2], id = b[3], len = e->len - 4;
    if (idx > 2 || total == 0 || total > 8 || c >= total || len > 80) return;
    if (asm_id != id || asm_total != total) asm_id = id, asm_total = total, asm_mask = 0, memset(asm_buf, 0, sizeof asm_buf);
    memcpy(asm_buf + c * 80, b + 4, len);
    asm_mask |= 1u << c;
    if (asm_mask != (1u << total) - 1) return;
    asm_mask = 0, asm_id = 0x1000;
    g_scene_rx++;
    unsigned n = total * 80 > g_w_scene_len ? g_w_scene_len : total * 80;
    unsigned h = scene_hash(asm_buf, n);
    unsigned sdemo = rd32(0x1581788);
    if (scene_recent_has(h) || !sdemo || !o_demosub[idx]) {
        g_scene_skip++;
        r2_log(W_LOG "SCENE %u skipped: %s (hash %08x)", idx, scene_recent_has(h) ? "this PC submitted the same request already" : "no sDemo / hook", h);
        return;
    }
    static unsigned char req[0x600];
    memset(req, 0, sizeof req);
    memcpy(req, asm_buf, n);
    g_w_replay++;
    unsigned r = o_demosub[idx]((void *)sdemo, req);
    g_w_replay--;
    scene_recent_add(h);
    g_scene_applied++;
    g_scene_last_t = GetTickCount() | 1;
    r2_log(W_LOG "SCENE %u applied from the other PC (vtable %08x, hash %08x) -> %u", idx, rd32((unsigned)req), h, r);
}

/* a scene is playing or was submitted lately: the game turns the split-screen coop flag off for it, the glue's watchdog must not treat that as a lost partner */
static int w_demo_running(void)   /* sDemo itself reports a running demo (stage 6f: the glue's scene gate uses this alone, without the 40 s memory below) */
{
    unsigned sd = rd32(0x1581788);
    if (sd) {
        unsigned st = rd32(sd + 0x38) & 0xFF, st2 = rd32(sd + 0xCC8) & 0xFF;
        if (st == 1 || st == 2 || st2 == 1 || st2 == 2) return 1;
    }
    return 0;
}

static int w_demo_recent(void)
{
    if (w_demo_running()) return 1;
    return g_scene_last_t && GetTickCount() - g_scene_last_t < 40000;
}

/* ---------------------------------------------------------------- item liveness (both directions, WORLD kind 3)
 * An item the story consumes (the table scene's weapon) disappears on the PC whose story ran the consume; the other PC still has the object. Every 2 s each PC sends the layout ids of its placed items
 * (layout a >= 0); a placed item here that the sender's last 3 digests did not hold is consumed through the object's own virtual (slot 98, +0x188: what AddItem calls for an item that is taken). */
static unsigned g_w_item_last_send, g_w_item_tx, g_w_item_rx, g_w_item_killed;
static struct { unsigned obj; unsigned char absent; } g_w_item_abs[32];
static unsigned g_w_item_tried[16], g_w_item_tried_n;

static unsigned story_placed_items(unsigned *objs, layout_t *ls, unsigned max)
{
    unsigned m = rd32(SITEM_CELL), n = 0;
    if (!m) return 0;
    for (int k = 0; k < 2; k++) {
        unsigned c = rd32(m + (k ? 0x44 : 0x30)), a = rd32(m + (k ? 0x50 : 0x3C));
        for (unsigned i = 0; i < c && i < 64 && a && n < max; i++) {
            unsigned o = rd32(a + 4 * i);
            layout_t l;
            if (!o || !layout_of(o, &l) || l.a < 0 || l.a > 0x7FFF) continue;
            int dup = 0;
            for (unsigned j = 0; j < n; j++)
                if (objs[j] == o) dup = 1;
            if (!dup) objs[n] = o, ls[n] = l, n++;
        }
    }
    return n;
}

static void story_send_items(void)
{
    unsigned now = GetTickCount();
    if (now - g_w_item_last_send < 2000 || !w_session_running()) return;
    g_w_item_last_send = now;
    unsigned objs[40];
    layout_t ls[40];
    unsigned n = story_placed_items(objs, ls, 40);
    unsigned char buf[2 + 40 * 8];
    buf[0] = (unsigned char)n;
    buf[1] = 1;
    for (unsigned i = 0; i < n; i++) memcpy(buf + 2 + 8 * i, &ls[i].a, 4), memcpy(buf + 6 + 8 * i, &ls[i].b, 2), memcpy(buf + 8 + 8 * i, &ls[i].c, 2);
    if (cs_send_world(g_cs, WK_ITEMS, buf, 2 + 8 * n)) g_w_item_tx++;
}

static void story_apply_items(const unsigned char *p, unsigned len)
{
    if (len < 2 || !w_session_running()) return;
    unsigned n = p[0];
    if (n > 40 || len < 2 + 8 * n) return;
    g_w_item_rx++;
    unsigned objs[40];
    layout_t ls[40];
    unsigned mine = story_placed_items(objs, ls, 40);
    for (unsigned i = 0; i < mine; i++) {
        int found = 0;
        for (unsigned j = 0; j < n; j++) {
            int a;
            unsigned b = p[6 + 8 * j] | (p[7 + 8 * j] << 8), c = p[8 + 8 * j] | (p[9 + 8 * j] << 8);
            memcpy(&a, p + 2 + 8 * j, 4);
            if (a == ls[i].a && b == ls[i].b && c == ls[i].c) found = 1;
        }
        int slot = -1, tried = 0;
        for (int k = 0; k < 16; k++)
            if (g_w_item_tried[k] == objs[i]) tried = 1;
        if (tried) continue;   /* consumed once: the virtual did not remove this object (a scripted item); the story's own removal is not mirrored, see docs */
        for (int k = 0; k < 32; k++)
            if (g_w_item_abs[k].obj == objs[i]) slot = k;
        if (found) {
            if (slot >= 0) g_w_item_abs[slot].obj = 0;
            continue;
        }
        if (slot < 0)
            for (int k = 0; k < 32 && slot < 0; k++)
                if (!g_w_item_abs[k].obj) slot = k, g_w_item_abs[k].obj = objs[i], g_w_item_abs[k].absent = 0;
        if (slot < 0) continue;
        if (++g_w_item_abs[slot].absent < 3) continue;   /* three digests in a row (about 6 s) */
        unsigned fn = rd32(rd32(objs[i]) + 0x188);
        r2_log(W_LOG "item (%d,%u,%u) %08x is gone on the other PC: consuming it here (virtual %08x), once", ls[i].a, ls[i].b, ls[i].c, objs[i], fn);
        g_w_item_abs[slot].obj = 0;
        g_w_item_tried[g_w_item_tried_n++ & 15] = objs[i];
        if (fn > 0x400000 && fn < 0x1400000) {
            unsigned a1[1] = {objs[i]};
            g_w_replay++;
            do_call(2, fn, a1, 1);
            g_w_replay--;
            g_w_item_killed++;
        }
    }
}

/* a new level instance (load, retry, set-up): pointers and counters of the old one are void */
static void w_level_reset(void)
{
    memset(g_wdeath, 0, sizeof g_wdeath);
    memset(g_w_dig_last, 0, sizeof g_w_dig_last);
    memset(g_eq_want, 0, sizeof g_eq_want);
    g_w_bar_rx = g_w_bar_passed = 0, g_w_bar_waiting = 0;
    g_w_last_digest = 0;
    memset(g_w_item_abs, 0, sizeof g_w_item_abs);
    memset(g_w_item_tried, 0, sizeof g_w_item_tried);
    g_w_item_last_send = 0;
    im_table_reset(&g_im_tab);   /* combat4: a new level instance (a retry restores each PC's own save): the owner's table is asked for again */
    memset(&g_im_sent, 0, sizeof g_im_sent), memset(&g_im_grace, 0, sizeof g_im_grace);
    g_im_full_tx = 1, g_im_req_t = 0;
    w_sm_reset();
    w_orphan_reset();
}

/* ---------------------------------------------------------------- install / events / tick / status */
static void story_install(int *ok)
{
    static const unsigned char b_put[] = {0x83, 0xEC, 0x08, 0x55, 0x8B, 0x6C, 0x24, 0x10};
    static const unsigned char b_set[] = {0xA1, 0x00, 0xAE, 0x57, 0x01};   /* mov eax, [0x157ae00] */
    void *t;
    if ((t = w_tramp(0x6ECE30, b_put, 8))) o_putitem = (putitem_t)t, *ok += w_jmp(0x6ECE30, 8, (void *)h_putitem);
    if ((t = w_tramp(0x9F8350, b_set, 5))) o_syncset = (syncset_t)t, *ok += w_jmp(0x9F8350, 5, (void *)h_syncset);
    if ((t = w_tramp(0x9F3B80, b_set, 5))) o_synccheck = (synccheck_t)t, *ok += w_jmp(0x9F3B80, 5, (void *)h_synccheck);
    static const unsigned char b_demo[3][6] = {{0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C}, {0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x38}, {0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x38}};
    void *hd[3] = {(void *)h_demo0, (void *)h_demo1, (void *)h_demo2};
    int nd = 0;
    for (int i = 0; i < 3; i++)
        if ((t = w_tramp(demosub_va[i], b_demo[i], 6))) o_demosub[i] = (demosub_t)t, *ok += w_jmp(demosub_va[i], 6, hd[i]), nd++;
    static const unsigned char b_gui[] = {0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1};   /* sub esp, 0x10; push esi; mov esi, ecx */
    if ((t = w_tramp(0x8FA590, b_gui, 6))) o_invgui = (invgui_t)t, *ok += w_jmp(0x8FA590, 6, (void *)h_invgui);
    char v[8];
    if (r2_env("REV2_INVMIRROR", v, sizeof v) && v[0] == '0') g_im_on = 0;
    r2_log(W_LOG "story hooks: PutItem %s, sync-set %s, sync-check %s, demo submits %d of 3, inventory screen %s (mirror %s)", o_putitem ? "yes" : "NO", o_syncset ? "yes" : "NO", o_synccheck ? "yes" : "NO", nd, o_invgui ? "yes" : "NO",
           g_im_on ? "on" : "off");
}

static void story_on_event(const cp_event *e)
{
    const unsigned char *b = e->body;
    switch (e->kind) {
    case EV_W_GRANT:   /* a story grant on the other PC: make sure this PC's copy of that slot holds the item */
        if (e->len >= 9) {
            unsigned item, cnt;
            memcpy(&item, b, 4), memcpy(&cnt, b + 4, 4);
            int slot = b[8];
            if (slot > 1) break;
            g_w_grant_rx++;
            if (inv_find(slot, item) >= 0) {
                g_w_grant_skip++;
                break;
            }
            story_put(slot, item, cnt, 0, "GRANT");
        }
        break;
    case EV_W_SCENE:
        scene_on_chunk(e);
        break;
    case EV_W_INV:   /* combat4: the owner's entries of its own human */
        if (e->len >= 4 && b[0] == (unsigned)g_rem_slot && g_im_on) {
            if (e->epoch != (uint8_t)cs_epoch(g_cs)) {
                g_im_rx_stale++;
                break;
            }
            int had = g_im_tab.have;
            if (im_table_apply(&g_im_tab, b, e->len)) g_im_rx_ev++;
            if (!had && g_im_tab.have && g_im_logs < (unsigned)g_im_log) {
                uint8_t idx[IM_N];
                char t[600];
                int n = im_full_list(&g_im_tab.inv, idx);
                im_fmt(t, sizeof t, &g_im_tab.inv, idx, n);
                g_im_logs++;
                r2_log(W_LOG "INV: the partner's (slot %d) full set received (%d entries):%s", g_rem_slot, n, t);
            }
        }
        break;
    case EV_W_INVREQ:
        if (e->epoch == (uint8_t)cs_epoch(g_cs)) g_im_full_tx = 1, g_im_req_rx++;
        break;
    case EV_W_INVADJ:
        if (g_im_on) im_apply_adj(e);
        break;
    case EV_W_BARRIER:
        if (e->len >= 1 && b[0] == 8) {
            g_w_bar_rx++;
            r2_log(W_LOG "cutscene sync point reached by the other PC (rx %u, passed %u, waiting here %u)", g_w_bar_rx, g_w_bar_passed, g_w_bar_waiting);
        }
        break;
    default:
        break;
    }
}

static void story_status(char *out, int size)
{
    snprintf(out, size,
             "story: PutItem %u (pickup tx %u rx %u, grant tx %u rx %u skipped %u, pickup skipped %u), equipment calls %u (item missing %u), cutscene barrier %s (arrived here %u, remote %u, passed %u, timeouts %u, last wait %u ms), flag digests tx %u rx %u applied %u, death scan tx %u, scenes %s tx %u rx %u applied %u skipped %u, item digests tx %u rx %u consumed %u\n",
             g_w_put_n, g_w_put_tx, g_w_put_rx, g_w_grant_tx, g_w_grant_rx, g_w_grant_skip, g_w_pick_skip, g_w_equip_calls, g_w_equip_miss, g_w_barrier ? "on" : "off", g_w_bar_tx, g_w_bar_rx, g_w_bar_passed, g_w_bar_timeouts, g_w_bar_wait_ms,
             g_w_dig_tx, g_w_dig_rx, g_w_dig_applied, g_w_death_tx, g_w_scene ? "on" : "off", g_scene_tx, g_scene_rx, g_scene_applied, g_scene_skip, g_w_item_tx, g_w_item_rx, g_w_item_killed);
    int o = (int)strlen(out);
    if (o < size - 400) im_status(out + o, size - o);
}
