/* door.inc: stage 6c, set model action mirror (doors, lockers, crates, gates). Included from world.inc after story.inc.
 *
 * Every placed set model (sSetModel, manager cell 0x1581920, objects carry a LayoutID like enemies and items) keeps its state machine in an embedded action object at +0x1A38 whose current
 * action id is read at +0x1A54 (previous +0x1A58); the vtable slot 46 (+0xB8) is the ChangeAction that switches it (same argument list as the enemy one: id, 1, 0, -1, 0). A cell door
 * (`uSm0453 : uSmSlideDoor : uSmDoor`) is closed in action 2 and open in action 3, the story's own opening is a ChangeAction too.
 * Both PCs run the story, so story-driven door changes happen on both. What has to be shared is what a HUMAN does (the E prompt on a door, a locker, a crate): the PC whose human did it sees the set
 * model's action change, sends DOOR {layout, new action, previous action}, and the other PC applies the same action through ChangeAction unless its copy is already there. Applying sets the
 * baseline of the scan, so a change made by a replay is never sent back. Optimistic on both sides (no host arbitration: a door has one state, last writer wins).
 * Console: `net w doors [on|off|list|log N]`.
 *
 * Stage 10 (objsync_logic.h, notes\objsync.md): every learned set model carries its kind (co-op door / shutter / pair, push box, carried object, plain). The movable kinds (uSmMoveBox family) are no
 * longer replayed with ChangeAction (the push box's Press start calls the actor's virtual, the actor of a bare ChangeAction is 0): their transform is mirrored by objsync.inc instead; carried objects keep
 * Break / Broken. A co-op change whose actor is the partner's puppet is this PC's replay and is not sent back. Shutters and pair co-op models get the stock press handler's steps before Act. */
#define SSETMODEL_CELL 0x1581920u
#include "objsync_logic.h"
typedef struct { unsigned obj; int a; unsigned b, c; int act; unsigned char used, ok, kind; unsigned seen; os_prop_t prop; unsigned rp_t; } wsm_t;   /* seen: tick of the last scan that found the object; rp_t: when the partner's press was replayed on it here (os_replay_echo) */
static wsm_t g_wsm[192];
static void os_sm_learn_xf(wsm_t *e, unsigned o);   /* objsync.inc */
static void os_level_reset(void);
static int g_w_sm_access = 1;   /* `net w doors access 0|1`: hold mAccessType at 0 while a press of the partner is replayed (the presser's PC accepted it) */
static unsigned g_w_sm_echo_skip, g_w_sm_move_skip, g_w_sm_move_refused, g_w_sm_alias, g_w_sm_full, g_w_sm_access_ovr, g_w_sm_replay_refused;
static struct { unsigned obj, since; } g_w_acc_hold[8];
static struct { unsigned obj, t; int a; unsigned b, c; } g_w_rp_check;   /* the last replayed press: still in the waiting action 0.5 s later = refused on this PC */
static int g_w_sm_sync = 1, g_w_sm_log = 200;
static unsigned g_w_sm_tx, g_w_sm_rx, g_w_sm_applied, g_w_sm_missing, g_w_sm_same, g_w_sm_scan_t, g_w_sm_first, g_w_sm_n, g_w_sm_quiet_until, g_w_sm_throttled, g_w_sm_skipped_coop;
static unsigned g_w_sm_cap = 192;
static int g_w_sm_coop;   /* `net w doors coop 1`: co-op set models (uSmCoopBase family) are mirrored too, with the sender's human as the action's actor (stage 6d, research, off by default) */
static unsigned g_w_sm_coop_tx, g_w_sm_coop_rx, g_w_sm_coop_applied;
static unsigned g_w_sm_snap_at, g_w_sm_snap_tx;   /* host: one snapshot of every learned set model 6 s after a level (re)load: a joiner / retry that is behind gets the host's door states */

/* Co-op set models (uSmCoopBase family: uSmDoorCoop, uSmShutterCoop, uSmCoopDemo ...) run player-side events (cSmCoopBaseEventPl_*, started through the player's vtable +0x36C) and need the partner objects: a bare ChangeAction on one
 * crashed the game (AV at 0x74E035, esi = the shutter `uSm0418 (153,160,0)`, hardware run s6c3). They are neither sent nor applied by the mirror; their interplay is the players' business (puppet answer, open item). */
static int w_sm_kind(unsigned o)
{
    static char big[4096];
    classes_name(o, big, sizeof big - 1);
    big[sizeof big - 1] = 0;
    return os_kind_of(big);
}

static int w_sm_class_ok(unsigned o) { return !os_is_coop(w_sm_kind(o)); }

/* stage 10: who acts on the set model now: the current action instance (action manager +0x1A38, +0x30) holds the actor at +0xC (co-op actions: 6f smtrace "act+C"; the push box's Press start 0x7DBCF0
 * and the carried object's Pickup start 0x7D2A90 read it there too). The local human = OS_ACT_LOCAL, another player slot (the partner's puppet) = OS_ACT_REMOTE, nothing / no player = OS_ACT_NONE. */
static int w_sm_actor_rel(unsigned o)
{
    unsigned inst = rd32(o + 0x1A38 + 0x30);
    if (inst < 0x10000 || inst >= 0x7FFF0000u) return OS_ACT_NONE;
    unsigned actor = rd32(inst + 0xC);
    if (!actor) return OS_ACT_NONE;
    if (actor == player_obj(g_my_slot)) return OS_ACT_LOCAL;
    for (int s = 0; s < 8; s++)
        if (player_obj(s) == actor) return OS_ACT_REMOTE;
    return OS_ACT_NONE;
}

/* the learned entry of a set model (NULL when it was not seen by a scan yet) */
static wsm_t *w_sm_entry(int a, unsigned b, unsigned c)
{
    for (int j = 0; j < (int)g_w_sm_cap; j++)
        if (g_wsm[j].used && g_wsm[j].a == a && g_wsm[j].b == b && g_wsm[j].c == c) return &g_wsm[j];
    return NULL;
}

static int w_sm_kind_of_obj(unsigned o, int a, unsigned b, unsigned c)
{
    wsm_t *e = w_sm_entry(a, b, c);
    return e && e->obj == o ? e->kind : w_sm_kind(o);
}

/* stage 10, a co-op event for a layout id that is not active here: the same physical set model under its other stage's id (border shutters: (153,160,0) = (151,160,135), (100,161,160) = (101,160,105),
 * (101,160,142) = (102,160,102)), found by kind and position */
static unsigned w_sm_alias(const unsigned char *pos, int kind_sent)
{
    unsigned m = rd32(SSETMODEL_CELL);
    if (!m) return 0;
    float px, py, pz;
    memcpy(&px, pos, 4), memcpy(&py, pos + 4, 4), memcpy(&pz, pos + 8, 4);
    for (int k = 0; k < 2; k++) {
        unsigned n = rd32(m + (k ? 0x44 : 0x30)), arr = rd32(m + (k ? 0x50 : 0x3C));
        for (unsigned i = 0; i < n && i < 160 && arr; i++) {
            unsigned o = rd32(arr + 4 * i);
            if (!o) continue;
            float dx = rdf(o + 0x40) - px, dy = rdf(o + 0x44) - py, dz = rdf(o + 0x48) - pz;
            if (dx * dx + dy * dy + dz * dz > OS_ALIAS_DIST * OS_ALIAS_DIST) continue;
            if (os_alias_ok(kind_sent, w_sm_kind(o), dx, dy, dz)) return o;
        }
    }
    return 0;
}

static void w_sm_acc_hold(unsigned o)
{
    for (int i = 0; i < 8; i++)
        if (!g_w_acc_hold[i].obj || g_w_acc_hold[i].obj == o) {
            g_w_acc_hold[i].obj = o, g_w_acc_hold[i].since = GetTickCount();
            return;
        }
}

/* the replayed press has left the waiting action (the partner check of the SyncPostion start ran) or 2 s passed: mAccessType gets the FSM's current value back (sSetModel +0x2B0, the SmCoopAccess order) */
static void w_sm_acc_tick(unsigned now)
{
    unsigned m = rd32(SSETMODEL_CELL);
    for (int i = 0; i < 8; i++) {
        unsigned o = g_w_acc_hold[i].obj;
        if (!o) continue;
        if (!os_access_restore_due(g_w_acc_hold[i].since, now, (int)rd32(o + 0x1A54))) continue;
        unsigned v = m ? rd32(m + 0x2B0) : 0;
        wr(o + 0x43A4, &v, 4);
        g_w_acc_hold[i].obj = 0;
        r2_log(W_LOG "DOOR coop: mAccessType of %08x back to the FSM's value %u", o, v);
    }
}

/* one scan: new objects are only learned (first sighting is never an event), a changed action id of a known object is sent */
static void w_sm_scan(int send)
{
    unsigned m = rd32(SSETMODEL_CELL);
    if (!m) return;
    unsigned now = GetTickCount();
    for (int k = 0; k < 2; k++) {
        unsigned c = rd32(m + (k ? 0x44 : 0x30)), a = rd32(m + (k ? 0x50 : 0x3C));
        for (unsigned i = 0; i < c && i < 160 && a; i++) {
            unsigned o = rd32(a + 4 * i);
            layout_t l;
            if (!o || !layout_of(o, &l) || l.a < 0 || l.a > 0x7FFF) continue;
            int act = (int)rd32(o + 0x1A54), slot = -1, free_slot = -1;
            for (int j = 0; j < (int)g_w_sm_cap; j++) {
                if (g_wsm[j].used && g_wsm[j].a == l.a && g_wsm[j].b == l.b && g_wsm[j].c == l.c) {
                    slot = j;
                    break;
                }
                if (!g_wsm[j].used && free_slot < 0) free_slot = j;
            }
            if (slot < 0) {   /* first sighting (or a new instance of the same layout id after a reload: the pointer differs, the state is learned again) */
                if (free_slot < 0) continue;
                wsm_t *n = &g_wsm[free_slot];
                memset(n, 0, sizeof *n);
                n->used = 1, n->kind = (unsigned char)w_sm_kind(o), n->ok = !os_is_coop(n->kind), n->obj = o, n->a = l.a, n->b = l.b, n->c = l.c, n->act = act, n->seen = now;
                os_sm_learn_xf(n, o);   /* objsync.inc: the transform baseline of a movable set model */
                g_w_sm_first++;
                continue;
            }
            wsm_t *e = &g_wsm[slot];
            e->seen = now;
            if (e->obj != o) {   /* the object was re-created: learn */
                e->obj = o, e->act = act, e->kind = (unsigned char)w_sm_kind(o), e->ok = !os_is_coop(e->kind);
                os_sm_learn_xf(e, o);
                continue;
            }
            if (e->act == act) continue;
            int prev = e->act;
            e->act = act;
            if (!send || (int)(now - g_w_sm_quiet_until) < 0) continue;
            int actor = os_is_coop(e->kind) || os_is_move(e->kind) ? w_sm_actor_rel(o) : OS_ACT_NONE;
            if (os_replay_echo(e->kind, act, actor, e->rp_t, now)) {   /* our own replay of the partner's press, whatever the action instance says (osPB 07:22:56) */
                g_w_sm_echo_skip++;
                r2_log(W_LOG "DOOR coop: (%d,%u,%u) action %d -> %d right after the replay of the partner's press (actor %d): not sent back", l.a, l.b, l.c, prev, act, actor);
                continue;
            }
            if (!os_door_send(e->kind, act, actor, g_w_sm_coop)) {   /* co-op without `doors coop`, the echo of our own replay, a push / pickup (the transform goes by PROP) */
                if (os_is_coop(e->kind) && actor == OS_ACT_REMOTE) g_w_sm_echo_skip++;
                else if (os_is_move(e->kind)) g_w_sm_move_skip++;
                continue;
            }
            static unsigned win_t, win_n;   /* a set model that flickers (fans, lights) must not flood the reliable channel: at most 12 events per second */
            if (now - win_t > 1000) win_t = now, win_n = 0;
            if (++win_n > 12) {
                g_w_sm_throttled++;
                continue;
            }
            char cn[100];
            if ((int)g_w_sm_tx < g_w_sm_log) r2_log(W_LOG "set model %s layout (%d,%u,%u) action %d -> %d (sent)", w_class(o, cn, sizeof cn), l.a, l.b, l.c, prev, act);
            unsigned char b[31];
            memcpy(b, &l.a, 4), memcpy(b + 4, &l.b, 2), memcpy(b + 6, &l.c, 2), memcpy(b + 8, &act, 4), memcpy(b + 12, &prev, 4);
            b[16] = e->ok ? 0 : 1, b[17] = (unsigned char)g_my_slot;   /* co-op set model: the receiver gives the action the sender's puppet as its actor */
            float pos[3] = {rdf(o + 0x40), rdf(o + 0x44), rdf(o + 0x48)};   /* stage 10 tail: position + kind, for the alias of a border shutter (older receivers read 18 bytes) */
            memcpy(b + 18, pos, 12), b[30] = e->kind;
            w_ev_send(EV_W_DOOR, b, e->ok ? 16 : 31);
            g_w_sm_tx++;
            if (!e->ok) g_w_sm_coop_tx++;
        }
    }
}

static void w_sm_tick(void)
{
    if (!g_w_sm_sync || !w_session_running()) return;
    unsigned now = GetTickCount();
    if (now - g_w_sm_scan_t < 100) return;   /* 10 Hz is plenty for a door */
    g_w_sm_scan_t = now;
    w_sm_scan(1);
    w_sm_acc_tick(now);
    if (g_w_rp_check.obj && now - g_w_rp_check.t >= 500) {
        int act = (int)rd32(g_w_rp_check.obj + 0x1A54);
        if (act == 0) {
            g_w_sm_replay_refused++;
            r2_log(W_LOG "DOOR coop: the replayed press on (%d,%u,%u) did not take here (still in the waiting action after 0.5 s: busy lock, partner check or hint)", g_w_rp_check.a, g_w_rp_check.b, g_w_rp_check.c);
        }
        g_w_rp_check.obj = 0;
    }
    if (g_w_sm_snap_at && (int)(now - g_w_sm_snap_at) >= 0 && g_cs && cs_is_host(g_cs)) {
        g_w_sm_snap_at = 0;
        for (int j = 0; j < (int)g_w_sm_cap; j++) {
            if (!g_wsm[j].used || !g_wsm[j].ok || os_is_move(g_wsm[j].kind) || g_wsm[j].act < 0) continue;   /* movable set models: their action is a human's interaction (stage 10) */
            unsigned char b[16];
            int prev = -1;
            memcpy(b, &g_wsm[j].a, 4), memcpy(b + 4, &g_wsm[j].b, 2), memcpy(b + 6, &g_wsm[j].c, 2), memcpy(b + 8, &g_wsm[j].act, 4), memcpy(b + 12, &prev, 4);
            w_ev_send(EV_W_DOOR, b, 16);
            g_w_sm_snap_tx++;
        }
        r2_log(W_LOG "door snapshot sent: %u set model states", g_w_sm_snap_tx);
    }
}

static void w_sm_event(const cp_event *e)
{
    if (e->len < 16 || !g_w_sm_sync) return;
    int a, act, prev;
    memcpy(&a, e->body, 4), memcpy(&act, e->body + 8, 4), memcpy(&prev, e->body + 12, 4);
    unsigned b = e->body[4] | (e->body[5] << 8), c = e->body[6] | (e->body[7] << 8);
    g_w_sm_rx++;
    unsigned o = w_find_active(SSETMODEL_CELL, a, b, c);
    int coop_ev = e->len >= 18 && e->body[16];
    if (!o && coop_ev && e->len >= 31 && (o = w_sm_alias(e->body + 18, e->body[30])) != 0) {   /* stage 10: the other stage's id of a border shutter */
        g_w_sm_alias++;
        layout_t al = {0, 0, 0};
        layout_of(o, &al);
        r2_log(W_LOG "DOOR coop: (%d,%u,%u) is not active here, the same set model is (%d,%u,%u) %08x (kind %u, within %.0f units)", a, b, c, al.a, al.b, al.c, o, e->body[30], OS_ALIAS_DIST);
        a = al.a, b = al.b, c = al.c;
    }
    if (!o) {
        g_w_sm_missing++;
        if ((int)g_w_sm_missing < 20) r2_log(W_LOG "DOOR: no set model (%d,%u,%u) here (action %d)", a, b, c, act);
        return;
    }
    int kind = w_sm_kind_of_obj(o, a, b, c);
    int coop = os_is_coop(kind);
    if (coop && g_w_sm_coop >= 2 && coop_ev) {   /* 6e: the host's press chain (probe 0x710570): the pressing human gets the event cSmEventPL_ActPre (DTI 0x156B824, owner = the door); its handler creates the door's coop action instance WITH the actor and starts the Sync events on both partners. Replay exactly that on the presser's puppet; the door's later action changes (back to 0 ...) are its own business here */
        int pslot = e->body[17] < 2 ? e->body[17] : g_rem_slot;
        unsigned presser = player_obj(pslot);
        g_w_sm_coop_rx++;
        /* 6f: a second press on the puppet while the door is out of its waiting action (this PC's own press, the echo of our replay) cancelled the running chain: os_coop_plan skips it (busy).
         * Stage 10: the action tables of the door / shutter / pair co-op classes are the same (0 waiting, 1 SyncPostion = the press, 2 LoadWait, 3 Opening), so the trigger "action 1" holds for all of them. */
        int local_act = (int)rd32(o + 0x1A54);
        int plan = os_coop_plan(kind, act, local_act, presser && presser != player_obj(g_my_slot), g_w_sm_coop);
        if (plan < OS_RP_ACT) {
            g_w_sm_skipped_coop++;
            if (plan == OS_RP_SKIP_BUSY && g_w_sm_skipped_coop < 40) r2_log(W_LOG "DOOR coop: (%d,%u,%u) press of slot %d not replayed: this copy is in action %d (its own press or chain runs)", a, b, c, pslot, local_act);
            return;
        }
        /* 6f (first hardware run): the bare event start (0x710570) made the puppet's event end after 30 ms and the door never changed. The press the game itself makes is the door's Act virtual
         * (vtable +0x170 = 0x81BC40, `uSmCoopDemo__Act`: Act(slot, 1, 0) starts the event ActPre on that player, sets player+0x3785 / +0x6F60 = the door and calls the door's +0x174 that creates the
         * action instance with the actor). Same call here for the presser's puppet.
         * Stage 10: the stock press handler FUN_0074F9E0 (the action-button callback of every uSmCoopBase, registered by vf +0x1EC) does more before Act: +0x4230[slot] = 1 and the virtual +0x1F4
         * (uSmShutterCoop 0x753F90: the opening side +0x43C4 = 0, computed again from the lead's side). OS_RP_FULL does those two steps too (shutters and pair co-op; the door keeps the proven call). */
        unsigned act_fn = rd32(rd32(o) + 0x170);   /* the real press (0x74FA00..: the Wait action's trigger handler) calls door vtable +0x170 = 0x81BC40 with (slot, 1, 0); +0x218 was a wrong guess of the first 6f run */
        int acc = (int)rd32(o + 0x43A4), ovr = os_access_override(acc, g_w_sm_access);
        if (ovr) {   /* SmCoopAccess(1) here only (the FSM's presence tests are per PC, gap G5): the partner check of the SyncPostion start (FUN_007548A0) would refuse what the presser's PC accepted */
            unsigned z = 0;
            wr(o + 0x43A4, &z, 4);
            w_sm_acc_hold(o);
            g_w_sm_access_ovr++;
        }
        g_w_replay++;
        if (act_fn == 0x81BC40u) {
            if (plan == OS_RP_FULL) {
                unsigned char one = 1;
                if (pslot >= 0 && pslot < 8) wr(o + 0x4230 + (unsigned)pslot, &one, 1);
                unsigned f = rd32(rd32(o) + 0x1F4);
                if (f > 0x400000 && f < 0x1400000) {
                    unsigned a1[1] = {o};
                    do_call(2, f, a1, 1);
                }
                g_w_sm_full++;
            }
            unsigned aa[4] = {o, (unsigned)pslot, 1, 0};
            do_call(2, act_fn, aa, 4);
        } else {
            unsigned pa[6] = {presser, 0x156B824u, o, 0, 0, 0};
            do_call(2, 0x710570, pa, 6);
        }
        g_w_replay--;
        g_w_sm_coop_applied++;
        wsm_t *we = w_sm_entry(a, b, c);   /* the scan must not send this replay's own transition back (os_replay_echo) */
        if (we) we->rp_t = GetTickCount() ? GetTickCount() : 1;
        g_w_rp_check.obj = o, g_w_rp_check.t = GetTickCount(), g_w_rp_check.a = a, g_w_rp_check.b = b, g_w_rp_check.c = c;   /* w_sm_tick looks again after 0.5 s */
        char cn[100];
        r2_log(W_LOG "DOOR coop: press replayed for the puppet %08x (slot %d) on %s (%d,%u,%u) through %s%s; action right after the call %d", presser, pslot, w_class(o, cn, sizeof cn), a, b, c,
               act_fn == 0x81BC40u ? (plan == OS_RP_FULL ? "the stock press steps + Act" : "the Act virtual") : "the bare event start", ovr ? ", mAccessType held at 0" : "", (int)rd32(o + 0x1A54));
        return;
    }
    if (!coop) {   /* stage 10: push boxes / carried objects: only what needs no actor (os_door_apply) */
        int la = os_is_move(kind) ? w_sm_actor_rel(o) : OS_ACT_NONE;
        if (!os_door_apply(kind, act, la)) {
            g_w_sm_move_refused++;
            if (g_w_sm_move_refused < 20) r2_log(W_LOG "DOOR: (%d,%u,%u) action %d not replayed: a movable set model (kind %d) moves by its transform (PROP), not by a ChangeAction without its actor", a, b, c, act, kind);
            return;
        }
    }
    if (coop) {   /* co-op set model: its action (cSmCoopBaseAction, embedded at +0x1A38, owner at +0x20, ACTOR at +0xC) dereferences the actor at once: a bare ChangeAction crashed the host (AV 0x74E035). Only with the switch, and with the sender's puppet as the actor */
        if (!g_w_sm_coop || e->len < 18 || !e->body[16]) {
            g_w_sm_skipped_coop++;
            return;
        }
        g_w_sm_coop_rx++;
        unsigned actor = player_obj(e->body[17] < 2 ? e->body[17] : g_rem_slot), action = o + 0x1A38;
        if (!actor || rd32(action + 0x20) != o) {
            r2_log(W_LOG "DOOR coop: (%d,%u,%u) action object does not look as expected (owner %08x, set model %08x, actor %08x): not applied", a, b, c, rd32(action + 0x20), o, actor);
            return;
        }
        wr(action + 0xC, &actor, 4);
        g_w_sm_coop_applied++;
    }
    if ((int)rd32(o + 0x1A54) == act) {
        g_w_sm_same++;
        return;
    }
    unsigned vt = rd32(o), fn = rd32(vt + 0xB8);
    if (fn < 0x400000 || fn > 0x1400000) return;
    unsigned args[6] = {o, (unsigned)act, 1, 0, 0xFFFFFFFFu, 0};
    char cn[100];
    int before = (int)rd32(o + 0x1A54);
    g_w_replay++;
    do_call(2, fn, args, 6);
    g_w_replay--;
    int after = (int)rd32(o + 0x1A54);
    for (int j = 0; j < (int)g_w_sm_cap; j++)   /* the scan must not send what this replay changed */
        if (g_wsm[j].used && g_wsm[j].a == a && g_wsm[j].b == b && g_wsm[j].c == c) g_wsm[j].act = after;
    g_w_sm_applied++;
    if ((int)g_w_sm_applied < g_w_sm_log) r2_log(W_LOG "DOOR applied: %s layout (%d,%u,%u) action %d -> %d (asked %d, sender had %d before)", w_class(o, cn, sizeof cn), a, b, c, before, after, act, prev);
}

/* Stage 6f: `net w smtrace <a> <b> <c> [lines]` / `net w smtrace off`: a per-frame change log of ONE set model (the co-op door chain, 6e left no timeline): the words of its embedded action manager
 * (+0x1A38, 16 words), the lock word +0x37B8, the hint / state block +0x4350..+0x43CF (the per-slot hint states at +0x4374 are written by the partner check 0x7548A0), the CURRENT action instance
 * the manager points to (manager +0x30: vtable, refcount, +0xC actor, +0x14 state, +0x20 owner ...), and for both human slots the action id, the lock word and the position. One line per frame in which anything
 * changed, relative time in ms, so the order "press -> partner check -> Sync events -> door action" can be read from the log of each PC. */
#define SMT_RANGES 4
static struct {
    int on, a;
    unsigned b, c, obj, t0, lines, maxlines, cur;
    unsigned prev[16 + 1 + 32 + 12 + 1];
    unsigned pact[2], pst[2];
    float ppos[2][3];
    int have;
} g_smt;

/* the set model's four "action trigger" sub-objects (+0x4188 + 0x18 * i; the Wait action of a co-op door polls them with FUN_0081A410(i): flags (obj+0xC) & 7 == 2 and bit 0x400 = the prompt is up for somebody):
 * bit i = FUN_0081A410(i) true, bits 8+4i.. = (flags & 7) of the object (0 = no object) */
static unsigned smt_hints(unsigned so)
{
    unsigned bits = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned obj = rd32(so + 0x4188 + 0x18 * i), idv = rd32(so + 0x418C + 0x18 * i);
        if (idv != 0xFFFFFFFFu) {
            unsigned a[2] = {so, i};
            if (do_call(2, 0x81A410, a, 2) & 0xFF) bits |= 1u << i;
        }
        bits |= (obj ? (rd32(obj + 0xC) & 7) : 0) << (8 + 4 * i);
    }
    return bits;
}

static unsigned smt_read(unsigned so, unsigned *w)
{
    unsigned n = 0;
    for (unsigned k = 0; k < 16; k++) w[n++] = rd32(so + 0x1A38 + 4 * k);
    w[n++] = rd32(so + 0x37B8);
    for (unsigned k = 0; k < 32; k++) w[n++] = rd32(so + 0x4350 + 4 * k);
    unsigned cur = rd32(so + 0x1A38 + 0x30);
    for (unsigned k = 0; k < 12; k++) w[n++] = cur > 0x10000 && cur < 0x7FFF0000u ? rd32(cur + 4 * k) : 0;
    w[n++] = smt_hints(so);
    return cur;
}

static void smt_name(unsigned idx, char *b, int n)
{
    if (idx < 16) snprintf(b, n, "mgr+%X", 0x1A38 + 4 * idx);
    else if (idx == 16) snprintf(b, n, "lock+37B8");
    else if (idx < 49) snprintf(b, n, "blk+%X", 0x4350 + 4 * (idx - 17));
    else if (idx < 61) snprintf(b, n, "act+%X", 4 * (idx - 49));
    else snprintf(b, n, "hints");
}

static void smtrace_tick(void)
{
    if (!g_smt.on) return;
    unsigned so = w_find_active(SSETMODEL_CELL, g_smt.a, g_smt.b, g_smt.c);
    if (!so) return;
    unsigned now = GetTickCount();
    unsigned w[16 + 1 + 32 + 12 + 1];
    unsigned cur = smt_read(so, w);
    char line[900];
    int o = 0;
    if (!g_smt.have || so != g_smt.obj) {
        memcpy(g_smt.prev, w, sizeof w);
        g_smt.obj = so, g_smt.have = 1, g_smt.cur = cur;
        for (int i = 0; i < 2; i++) {
            unsigned p = player_obj(i);
            g_smt.pact[i] = p ? rd32(p + 0x1A54) : 0, g_smt.pst[i] = p ? rd32(p + 0x37B8) : 0;
            for (int k = 0; k < 3; k++) g_smt.ppos[i][k] = p ? rdf(p + 0x40 + 4 * k) : 0;
        }
        return;
    }
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++)
        if (i != 11 && w[i] != g_smt.prev[i] && o < (int)sizeof line - 60) {   /* word 11 = mgr+0x1A64 is a float timer that changes every frame */
            char nm[24];
            smt_name(i, nm, sizeof nm);
            o += snprintf(line + o, sizeof line - o, " %s %08x>%08x", nm, g_smt.prev[i], w[i]);
        }
    if (cur != g_smt.cur) o += snprintf(line + o, sizeof line - o, " [current action object %08x>%08x]", g_smt.cur, cur);
    for (int i = 0; i < 2; i++) {
        unsigned p = player_obj(i);
        if (!p) continue;
        unsigned act = rd32(p + 0x1A54), st = rd32(p + 0x37B8);
        float dx = rdf(p + 0x40) - g_smt.ppos[i][0], dz = rdf(p + 0x48) - g_smt.ppos[i][2];
        if (act != g_smt.pact[i] || st != g_smt.pst[i] || dx * dx + dz * dz > 25.0f) {
            o += snprintf(line + o, sizeof line - o, " | slot%d act %d>%d pst %08x>%08x pos (%.0f,%.0f,%.0f)", i, (int)g_smt.pact[i], (int)act, g_smt.pst[i], st, rdf(p + 0x40), rdf(p + 0x44), rdf(p + 0x48));
            g_smt.pact[i] = act, g_smt.pst[i] = st;
            for (int k = 0; k < 3; k++) g_smt.ppos[i][k] = rdf(p + 0x40 + 4 * k);
        }
    }
    if (o > 0 && g_smt.lines < g_smt.maxlines) {
        r2_log(W_LOG "smtrace +%u ms:%s", now - g_smt.t0, line);
        g_smt.lines++;
    }
    memcpy(g_smt.prev, w, sizeof w);
    g_smt.cur = cur;
}

static void smtrace_command(const char *arg, char *out, int size)
{
    int a = 0, n = 0;
    unsigned b = 0, c = 0, lines = 400;
    if (!strncmp(arg, "off", 3)) {
        g_smt.on = 0;
        snprintf(out, size, "smtrace off (%u lines)", g_smt.lines);
        return;
    }
    if ((n = sscanf(arg, "%d %u %u %u", &a, &b, &c, &lines)) < 3) {
        snprintf(out, size, "usage: net w smtrace <a> <b> <c> [max lines] | off; now %s (%u lines)", g_smt.on ? "ON" : "off", g_smt.lines);
        return;
    }
    memset(&g_smt, 0, sizeof g_smt);
    g_smt.on = 1, g_smt.a = a, g_smt.b = b, g_smt.c = c, g_smt.maxlines = lines, g_smt.t0 = GetTickCount();
    snprintf(out, size, "smtrace on for (%d,%u,%u), up to %u lines", a, b, c, lines);
}

static void w_sm_reset(void)
{
    memset(g_wsm, 0, sizeof g_wsm);
    memset(g_w_acc_hold, 0, sizeof g_w_acc_hold);   /* the objects of the old level are gone */
    g_w_rp_check.obj = 0;
    os_level_reset();   /* objsync.inc */
    g_w_sm_quiet_until = GetTickCount() + 4000;   /* objects settle their first actions after a load */
    g_w_sm_snap_at = GetTickCount() + 6000;
    g_w_sm_snap_tx = 0;
}

static void w_sm_command(const char *arg, char *out, int size)
{
    int o = 0, v;
    if (!strncmp(arg, "coop", 4)) g_w_sm_coop = atoi(arg + 4);   /* 0 off, 1 actor written into the action manager (6d, does not work: the actor lives in the action instance), 2 the press event is started on the presser's puppet (6e) */
    else if (!strncmp(arg, "on", 2)) g_w_sm_sync = 1;
    else if (!strncmp(arg, "off", 3)) g_w_sm_sync = 0;
    else if (!strncmp(arg, "access", 6)) g_w_sm_access = atoi(arg + 6) != 0;   /* stage 10: mAccessType held at 0 for a replayed press */
    else if (sscanf(arg, "log %d", &v) == 1) g_w_sm_log = v;
    o += snprintf(out + o, size - o, "doors: sync %s, learned %u, sent %u, received %u (applied %u, already same %u, object missing %u), throttled %u, co-op set models skipped %u", g_w_sm_sync ? "on" : "off", g_w_sm_first, g_w_sm_tx, g_w_sm_rx, g_w_sm_applied, g_w_sm_same, g_w_sm_missing, g_w_sm_throttled, g_w_sm_skipped_coop);
    o += snprintf(out + o, size - o, "; co-op mirror %s (mode %d: 2 = stock press steps for shutters / pair co-op, 3 = for all; sent %u, received %u, applied %u, full press %u, refused here %u, echoes not sent %u, alias %u, access held %u (switch %d))",
                  g_w_sm_coop ? "ON" : "off", g_w_sm_coop, g_w_sm_coop_tx, g_w_sm_coop_rx, g_w_sm_coop_applied, g_w_sm_full, g_w_sm_replay_refused, g_w_sm_echo_skip, g_w_sm_alias, g_w_sm_access_ovr, g_w_sm_access);
    o += snprintf(out + o, size - o, "; movable set models: actions not sent %u, not replayed %u", g_w_sm_move_skip, g_w_sm_move_refused);
    if (!strncmp(arg, "info", 4)) {   /* net w doors info <a> <b> <c>: the set model and its embedded action object (+0x1A38): words 0..0x30 and the class chain */
        int a = 0;
        unsigned b = 0, c = 0;
        sscanf(arg + 4, "%d %u %u", &a, &b, &c);
        unsigned so = w_find_active(SSETMODEL_CELL, a, b, c);
        if (!so) o += snprintf(out + o, size - o, " | no such set model");
        else {
            char cn[100];
            o += snprintf(out + o, size - o, " | %s %08x action id %d prev %d, embedded action %08x:", w_class(so, cn, sizeof cn), so, (int)rd32(so + 0x1A54), (int)rd32(so + 0x1A58), so + 0x1A38);
            for (unsigned k = 0; k < 14; k++) o += snprintf(out + o, size - o, " %08x", rd32(so + 0x1A38 + 4 * k));
            /* 6f: the four "action trigger" sub-objects (+0x4188 + 0x18 * i: object, id, ...) and the first words of each object: the zone in which a role's human gets the prompt is in there (floats) */
            for (unsigned i = 0; i < 4; i++) {
                unsigned ob = rd32(so + 0x4188 + 0x18 * i);
                o += snprintf(out + o, size - o, "\n  trigger %u: slot words %08x %08x %08x %08x %08x %08x", i, rd32(so + 0x4188 + 0x18 * i), rd32(so + 0x418C + 0x18 * i), rd32(so + 0x4190 + 0x18 * i), rd32(so + 0x4194 + 0x18 * i), rd32(so + 0x4198 + 0x18 * i), rd32(so + 0x419C + 0x18 * i));
                if (ob > 0x10000 && ob < 0x7FFF0000u) {
                    o += snprintf(out + o, size - o, "\n    object %08x:", ob);
                    for (unsigned k = 0; k < 28; k++) o += snprintf(out + o, size - o, " %08x", rd32(ob + 4 * k));
                    o += snprintf(out + o, size - o, "\n    as floats:");
                    for (unsigned k = 0; k < 28; k++) {
                        float fv = rdf(ob + 4 * k);
                        if (fv > -100000.0f && fv < 100000.0f && fv != 0.0f && (fv > 0.001f || fv < -0.001f)) o += snprintf(out + o, size - o, " +%X:%.1f", 4 * k, fv);
                    }
                }
            }
        }
    }
    if (!strncmp(arg, "kinds", 5)) {   /* stage 10: net w doors kinds [all]: every active set model with its kind (objsync_logic.h) from the class chain, also without a session (the scan learns only in one); without `all` the plain ones are counted only */
        unsigned m = rd32(SSETMODEL_CELL);
        int all = strstr(arg, "all") != NULL, cnt[8] = {0};
        for (int k = 0; m && k < 2; k++) {
            unsigned n = rd32(m + (k ? 0x44 : 0x30)), arr = rd32(m + (k ? 0x50 : 0x3C));
            for (unsigned i = 0; i < n && i < 160 && arr; i++) {
                unsigned so = rd32(arr + 4 * i);
                layout_t l;
                if (!so || !layout_of(so, &l)) continue;
                int kd = w_sm_kind(so);
                cnt[kd & 7]++;
                if ((kd != OS_PLAIN || all) && o < size - 220) {
                    char cn[100];
                    o += snprintf(out + o, size - o, "\n  (%d,%u,%u) %08x %s kind %d act %d actor %d at (%.1f,%.1f,%.1f) q (%.3f,%.3f,%.3f,%.3f) mAccessType %d", l.a, l.b, l.c, so, w_class(so, cn, sizeof cn), kd, (int)rd32(so + 0x1A54),
                                  w_sm_actor_rel(so), rdf(so + 0x40), rdf(so + 0x44), rdf(so + 0x48), rdf(so + 0x50), rdf(so + 0x54), rdf(so + 0x58), rdf(so + 0x5C), os_is_coop(kd) ? (int)rd32(so + 0x43A4) : -1);
                }
            }
        }
        o += snprintf(out + o, size - o, "\n  kinds: plain %d, co-op door %d, shutter %d, pair %d, other co-op %d, push box %d, carried %d, other movable %d", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[7]);
    }
    if (!strncmp(arg, "list", 4)) {
        for (int j = 0; j < (int)g_w_sm_cap && o < size - 120; j++)
            if (g_wsm[j].used) o += snprintf(out + o, size - o, "\n  (%d,%u,%u) %08x act %d kind %u", g_wsm[j].a, g_wsm[j].b, g_wsm[j].c, g_wsm[j].obj, g_wsm[j].act, g_wsm[j].kind);
    }
}
