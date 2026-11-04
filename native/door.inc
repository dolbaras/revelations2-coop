/* door.inc: stage 6c, set model action mirror (doors, lockers, crates, gates). Included from world.inc after story.inc.
 *
 * Every placed set model (sSetModel, manager cell 0x1581920, objects carry a LayoutID like enemies and items) keeps its state machine in an embedded action object at +0x1A38 whose current
 * action id is read at +0x1A54 (previous +0x1A58); the vtable slot 46 (+0xB8) is the ChangeAction that switches it (same argument list as the enemy one: id, 1, 0, -1, 0). A cell door
 * (`uSm0453 : uSmSlideDoor : uSmDoor`) is closed in action 2 and open in action 3, the story's own opening is a ChangeAction too.
 * Both PCs run the story, so story-driven door changes happen on both. What has to be shared is what a HUMAN does (the E prompt on a door, a locker, a crate): the PC whose human did it sees the set
 * model's action change, sends DOOR {layout, new action, previous action}, and the other PC applies the same action through ChangeAction unless its copy is already there. Applying sets the
 * baseline of the scan, so a change made by a replay is never sent back. Optimistic on both sides (no host arbitration: a door has one state, last writer wins).
 * Console: `net w doors [on|off|list|log N]`. */
#define SSETMODEL_CELL 0x1581920u
typedef struct { unsigned obj; int a; unsigned b, c; int act; unsigned char used, ok; } wsm_t;
static wsm_t g_wsm[192];
static int g_w_sm_sync = 1, g_w_sm_log = 200;
static unsigned g_w_sm_tx, g_w_sm_rx, g_w_sm_applied, g_w_sm_missing, g_w_sm_same, g_w_sm_scan_t, g_w_sm_first, g_w_sm_n, g_w_sm_quiet_until, g_w_sm_throttled, g_w_sm_skipped_coop;
static unsigned g_w_sm_cap = 192;
static int g_w_sm_coop;   /* `net w doors coop 1`: co-op set models (uSmCoopBase family) are mirrored too, with the sender's human as the action's actor (stage 6d, research, off by default) */
static unsigned g_w_sm_coop_tx, g_w_sm_coop_rx, g_w_sm_coop_applied;
static unsigned g_w_sm_snap_at, g_w_sm_snap_tx;   /* host: one snapshot of every learned set model 6 s after a level (re)load: a joiner / retry that is behind gets the host's door states */

/* Co-op set models (uSmCoopBase family: uSmDoorCoop, uSmShutterCoop, uSmCoopDemo ...) run player-side events (cSmCoopBaseEventPl_*, started through the player's vtable +0x36C) and need the partner objects: a bare ChangeAction on one
 * crashed the game (AV at 0x74E035, esi = the shutter `uSm0418 (153,160,0)`, hardware run s6c3). They are neither sent nor applied by the mirror; their interplay is the players' business (puppet answer, open item). */
static int w_sm_class_ok(unsigned o)
{
    static char big[4096];
    classes_name(o, big, sizeof big - 1);
    big[sizeof big - 1] = 0;
    return strstr(big, "Coop") == NULL;
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
                g_wsm[free_slot].used = 1, g_wsm[free_slot].ok = (unsigned char)w_sm_class_ok(o), g_wsm[free_slot].obj = o, g_wsm[free_slot].a = l.a, g_wsm[free_slot].b = l.b, g_wsm[free_slot].c = l.c, g_wsm[free_slot].act = act;
                g_w_sm_first++;
                continue;
            }
            wsm_t *e = &g_wsm[slot];
            if (e->obj != o) {   /* the object was re-created: learn */
                e->obj = o, e->act = act, e->ok = (unsigned char)w_sm_class_ok(o);
                continue;
            }
            if (e->act == act) continue;
            int prev = e->act;
            e->act = act;
            if (!e->ok && !g_w_sm_coop) continue;   /* co-op set model: not mirrored unless `net w doors coop 1` */
            if (!send || (int)(now - g_w_sm_quiet_until) < 0) continue;
            static unsigned win_t, win_n;   /* a set model that flickers (fans, lights) must not flood the reliable channel: at most 12 events per second */
            if (now - win_t > 1000) win_t = now, win_n = 0;
            if (++win_n > 12) {
                g_w_sm_throttled++;
                continue;
            }
            char cn[100];
            if ((int)g_w_sm_tx < g_w_sm_log) r2_log(W_LOG "set model %s layout (%d,%u,%u) action %d -> %d (sent)", w_class(o, cn, sizeof cn), l.a, l.b, l.c, prev, act);
            unsigned char b[18];
            memcpy(b, &l.a, 4), memcpy(b + 4, &l.b, 2), memcpy(b + 6, &l.c, 2), memcpy(b + 8, &act, 4), memcpy(b + 12, &prev, 4);
            b[16] = e->ok ? 0 : 1, b[17] = (unsigned char)g_my_slot;   /* co-op set model: the receiver gives the action the sender's puppet as its actor */
            w_ev_send(EV_W_DOOR, b, e->ok ? 16 : 18);
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
    if (g_w_sm_snap_at && (int)(now - g_w_sm_snap_at) >= 0 && g_cs && cs_is_host(g_cs)) {
        g_w_sm_snap_at = 0;
        for (int j = 0; j < (int)g_w_sm_cap; j++) {
            if (!g_wsm[j].used || !g_wsm[j].ok || g_wsm[j].act < 0) continue;
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
    if (!o) {
        g_w_sm_missing++;
        if ((int)g_w_sm_missing < 20) r2_log(W_LOG "DOOR: no set model (%d,%u,%u) here (action %d)", a, b, c, act);
        return;
    }
    int coop = !w_sm_class_ok(o);
    if (coop && g_w_sm_coop >= 2 && e->len >= 18 && e->body[16]) {   /* 6e: the host's press chain (probe 0x710570): the pressing human gets the event cSmEventPL_ActPre (DTI 0x156B824, owner = the door); its handler creates the door's coop action instance WITH the actor and starts the Sync events on both partners. Replay exactly that on the presser's puppet; the door's later action changes (back to 0 ...) are its own business here */
        unsigned presser = player_obj(e->body[17] < 2 ? e->body[17] : g_rem_slot);
        g_w_sm_coop_rx++;
        if (act != 1 || !presser || presser == player_obj(g_my_slot)) {
            g_w_sm_skipped_coop++;
            return;
        }
        /* 6f (first hardware run): the bare event start (0x710570) made the puppet's event end after 30 ms and the door never changed. The press the game itself makes is the door's Act virtual
         * (vtable +0x170 = 0x81BC40, `uSmCoopDemo__Act`: Act(slot, 1, 0) starts the event ActPre on that player, sets player+0x3785 / +0x6F60 = the door and calls the door's +0x174 that creates the
         * action instance with the actor). Same call here for the presser's puppet. */
        if ((int)rd32(o + 0x1A54) != 0) {   /* the door is out of its waiting action already (this PC's own press, or the echo of the replay we just made on the other PC): a second press on the puppet cancelled the running chain (first run of the +0x170 replay: the host's chain aborted after 0.3 s) */
            g_w_sm_skipped_coop++;
            return;
        }
        int pslot = e->body[17] < 2 ? e->body[17] : g_rem_slot;
        unsigned act_fn = rd32(rd32(o) + 0x170);   /* the real press (0x74FA00..: the Wait action's trigger handler) calls door vtable +0x170 = 0x81BC40 with (slot, 1, 0); +0x218 was a wrong guess of the first 6f run */
        g_w_replay++;
        if (act_fn == 0x81BC40u) {
            unsigned aa[4] = {o, (unsigned)pslot, 1, 0};
            do_call(2, act_fn, aa, 4);
        } else {
            unsigned pa[6] = {presser, 0x156B824u, o, 0, 0, 0};
            do_call(2, 0x710570, pa, 6);
        }
        g_w_replay--;
        g_w_sm_coop_applied++;
        r2_log(W_LOG "DOOR coop: press replayed for the puppet %08x (slot %d) on the door (%d,%u,%u) through %s", presser, pslot, a, b, c, act_fn == 0x81BC40u ? "the door's Act virtual" : "the bare event start");
        return;
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
    else if (sscanf(arg, "log %d", &v) == 1) g_w_sm_log = v;
    o += snprintf(out + o, size - o, "doors: sync %s, learned %u, sent %u, received %u (applied %u, already same %u, object missing %u), throttled %u, co-op set models skipped %u", g_w_sm_sync ? "on" : "off", g_w_sm_first, g_w_sm_tx, g_w_sm_rx, g_w_sm_applied, g_w_sm_same, g_w_sm_missing, g_w_sm_throttled, g_w_sm_skipped_coop);
    o += snprintf(out + o, size - o, "; co-op mirror %s (sent %u, received %u, applied %u)", g_w_sm_coop ? "ON" : "off", g_w_sm_coop_tx, g_w_sm_coop_rx, g_w_sm_coop_applied);
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
    if (!strncmp(arg, "list", 4)) {
        for (int j = 0; j < (int)g_w_sm_cap && o < size - 120; j++)
            if (g_wsm[j].used) o += snprintf(out + o, size - o, "\n  (%d,%u,%u) %08x act %d", g_wsm[j].a, g_wsm[j].b, g_wsm[j].c, g_wsm[j].obj, g_wsm[j].act);
    }
}
