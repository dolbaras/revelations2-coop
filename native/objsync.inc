/* objsync.inc: stage 10 (gap G2 of the Episode 1 maps), the transforms of movable set models. Included from world.inc after door.inc (it uses door.inc's table of learned set models).
 *
 * The movable family is uSmMoveBox: push boxes uSmPress (uSm1805: 101 (101,3,3/4), 151 (151,3,3/4)) and carried objects uSmInteractive (uSm1160: the room-4 box (151,1,0) and the box that frees
 * Natalia's hole (154,1,4); bricks uSm1814 / uSm1928). Both PCs run the story, but a push or a carry is one human's action on one PC: the other PC's copy stays where it was, so the partner's PC has no box
 * at the stairs to climb, its box checker (uSmMoveBoxPosChecker: a zone test of the box's root matrix, then SF stage*1000+n or EpisodeFlag 99000+n) never fires, and the FSM's AreaHit on the box (154,
 * `Sleep_sm_NatHole`) keeps Natalia's holes asleep there.
 * PROP: the PC whose human is the actor of the object's current action (action instance +0xC: the pusher / the carrier) sends the transform at most 10 times a second while it changes, keeps sending
 * for 2 s after the human let go (the end of a push snaps the box to the ground, FUN_007DFD20) and then once more as the final rest. The other PC writes the position (+0x40), the rotation quaternion
 * (+0x50), the root matrix translation (+0xE0, the matrix vf +0x50 returns +0xB0 for joint -1) and, for a push box at rest, the two rest positions FUN_007DFD20 writes at the end of a push (+0x4360,
 * +0x4370). Its own checker / AreaHit then see the box. A PC whose own human holds the same object keeps its own state (logic: objsync_logic.h, tests/test_objsync.c).
 * Console: `net w props [on|off|list]`. */
static int g_os_on = 1;
static unsigned g_os_scan_t, g_os_tx, g_os_final_tx, g_os_rx, g_os_applied, g_os_ignored, g_os_missing, g_os_logs;

static void os_read_xf(unsigned o, os_xf_t *x)
{
    for (int i = 0; i < 3; i++) x->p[i] = rdf(o + 0x40 + 4 * (unsigned)i);
    for (int i = 0; i < 4; i++) x->q[i] = rdf(o + 0x50 + 4 * (unsigned)i);
}

static void os_sm_learn_xf(wsm_t *e, unsigned o)
{
    os_xf_t x;
    if (!os_is_move(e->kind)) return;
    os_read_xf(o, &x);
    os_prop_learn(&e->prop, &x);
}

static void os_level_reset(void) {}   /* the per-object state lives in door.inc's table, cleared with it */

static void os_write_xf(unsigned o, int kind, const os_xf_t *x, int rest)
{
    float pos[4] = {x->p[0], x->p[1], x->p[2], 0.0f};
    wr(o + 0x40, pos, 16);
    wr(o + 0x50, x->q, 16);
    wr(o + 0xE0, x->p, 12);
    if (rest && kind == OS_MOVE_PRESS) wr(o + 0x4360, pos, 16), wr(o + 0x4370, pos, 16);
}

static void os_tick(void)
{
    if (!g_os_on || !w_session_running()) return;
    unsigned now = GetTickCount();
    if (now - g_os_scan_t < OS_PROP_MS) return;
    g_os_scan_t = now;
    if ((int)(now - g_w_sm_quiet_until) < 0) return;   /* objects settle after a load */
    for (int j = 0; j < (int)g_w_sm_cap; j++) {
        wsm_t *e = &g_wsm[j];
        if (!e->used || !os_is_move(e->kind) || now - e->seen > 400) continue;   /* only objects of the last door scan (a stale pointer of an unloaded block is never touched) */
        unsigned o = e->obj;
        os_xf_t cur, raw;
        os_read_xf(o, &raw);
        int rel = w_sm_actor_rel(o);
        if (e->kind == OS_MOVE_CARRY) {   /* held: +0x40 is relative to the holder (os_carry_sample, pair run wpB3c 22:40:14) */
            float mt[3] = {rdf(o + 0xE0), rdf(o + 0xE4), rdf(o + 0xE8)}, hp[3];
            const float *hold = NULL;
            int act = (int)rd32(o + 0x1A54);
            unsigned inst = rd32(o + 0x1A38 + 0x30), actor = (inst >= 0x10000 && inst < 0x7FFF0000u) ? rd32(inst + 0xC) : 0;
            if (actor >= 0x10000 && actor < 0x7FFF0000u) hp[0] = rdf(actor + 0x40), hp[1] = rdf(actor + 0x44), hp[2] = rdf(actor + 0x48), hold = hp;
            if (!os_carry_sample(e->kind, act, &raw, mt, hold, &e->prop.base, &cur)) cur = e->prop.base;
            rel = os_carry_actor(e->kind, act, rel, &raw, mt);   /* the actor stays after the put-down (wpB3d): only an attached box is held */
        } else {
            cur = raw;
        }
        int f = os_prop_tick(&e->prop, &cur, rel, now);
        if (!f) continue;
        os_prop_msg_t m;
        memset(&m, 0, sizeof m);
        m.a = e->a, m.b = e->b, m.c = e->c, m.flags = f, m.slot = g_my_slot, m.seq = e->prop.seq_tx, m.xf = e->prop.base, m.act = (int)rd32(o + 0x1A54);
        unsigned char b[OS_PROP_LEN];
        w_ev_send(EV_W_PROP, b, (unsigned)os_prop_pack(&m, b));
        g_os_tx++;
        if (f & OS_PF_FINAL) g_os_final_tx++;
        if ((f & OS_PF_FINAL) || g_os_logs < 30) {
            g_os_logs++;
            r2_log(W_LOG "PROP sent: (%d,%u,%u) kind %u at (%.1f,%.1f,%.1f) action %d %s seq %u", e->a, e->b, e->c, e->kind, m.xf.p[0], m.xf.p[1], m.xf.p[2], m.act,
                   (f & OS_PF_FINAL) ? "final rest" : (f & OS_PF_REST) ? "settling" : "moving", m.seq);
        }
    }
}

static void os_prop_event(const cp_event *ev)
{
    os_prop_msg_t m;
    if (!g_os_on || !os_prop_unpack(ev->body, ev->len, &m)) return;
    g_os_rx++;
    unsigned o = w_find_active(SSETMODEL_CELL, m.a, m.b, m.c);
    if (!o) {
        g_os_missing++;
        if (g_os_missing < 20) r2_log(W_LOG "PROP: no set model (%d,%u,%u) here", m.a, m.b, m.c);
        return;
    }
    wsm_t *e = w_sm_entry(m.a, m.b, m.c);
    if (!e || e->obj != o) {   /* not learned yet (the scan runs at 10 Hz): learn now, without sending */
        w_sm_scan(0);
        e = w_sm_entry(m.a, m.b, m.c);
    }
    if (!e || e->obj != o || !os_is_move(e->kind)) {
        g_os_ignored++;
        return;
    }
    unsigned now = GetTickCount();
    int rel = w_sm_actor_rel(o);
    if (e->kind == OS_MOVE_CARRY) {   /* a box this PC's human put down keeps him as its actor (wpB3d 23:21:42): held only while attached */
        os_xf_t raw;
        float mt[3] = {rdf(o + 0xE0), rdf(o + 0xE4), rdf(o + 0xE8)};
        os_read_xf(o, &raw);
        rel = os_carry_actor(e->kind, (int)rd32(o + 0x1A54), rel, &raw, mt);
    }
    if (!os_prop_accept(&e->prop, m.seq, rel, now)) {
        g_os_ignored++;
        return;
    }
    float before[3] = {rdf(o + 0x40), rdf(o + 0x44), rdf(o + 0x48)};
    os_write_xf(o, e->kind, &m.xf, (m.flags & OS_PF_REST) != 0);
    os_prop_applied(&e->prop, &m.xf);
    g_os_applied++;
    if ((m.flags & OS_PF_FINAL) || g_os_applied <= 30)
        r2_log(W_LOG "PROP applied: (%d,%u,%u) kind %u from (%.1f,%.1f,%.1f) to (%.1f,%.1f,%.1f), the sender's action %d%s", m.a, m.b, m.c, e->kind, before[0], before[1], before[2], m.xf.p[0], m.xf.p[1], m.xf.p[2], m.act,
               (m.flags & OS_PF_FINAL) ? ", final rest" : "");
}

static void os_command(const char *arg, char *out, int size)
{
    int o = 0;
    if (!strncmp(arg, "on", 2)) g_os_on = 1;
    else if (!strncmp(arg, "off", 3)) g_os_on = 0;
    o += snprintf(out + o, size - o, "props (movable set models): %s; sent %u (final rests %u), received %u, applied %u, ignored %u (own human holds it / older / not movable), object missing %u", g_os_on ? "ON" : "off", g_os_tx, g_os_final_tx, g_os_rx,
                  g_os_applied, g_os_ignored, g_os_missing);
    if (!strncmp(arg, "list", 4))
        for (int j = 0; j < (int)g_w_sm_cap && o < size - 160; j++) {
            wsm_t *e = &g_wsm[j];
            if (!e->used || !os_is_move(e->kind)) continue;
            o += snprintf(out + o, size - o, "\n  (%d,%u,%u) %08x kind %u act %d at (%.1f,%.1f,%.1f) owner %u seq tx %u rx %u actor %d", e->a, e->b, e->c, e->obj, e->kind, e->act, rdf(e->obj + 0x40), rdf(e->obj + 0x44), rdf(e->obj + 0x48),
                          e->prop.owner, e->prop.seq_tx, e->prop.seq_rx, w_sm_actor_rel(e->obj));
        }
}
