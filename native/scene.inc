/* scene.inc: scene032, the cutscene sync between the two PCs (docs\world.md "Cutscenes (0.3.2)"; private analysis notes\cutscene-sync.md). Included from world.inc after story.inc.
 * Logic (and its tests): scene_logic.h / tests/test_scene.c. `net scene 0` (or REV2_SCENE=0) switches all of it off: the behaviour of rel032 (story.inc's kind-8 wait while
 * the glue runs, the byte-hash scene dedupe, the plain door mirror, the stock one-PC skip), and no event of this file goes out.
 *
 *  start   the stock sync points (story.inc hooks 0x9F8350 set / 0x9F3B80 check): kind 7 = the end of the stage loader before play, kind 8 = a pre-rendered movie is
 *          prepared. Both hold until the partner reports the same point (READY), in every glue state (the movies play inside loads). Limits 300 s / 30 s; at once when the
 *          partner is gone, has no scene sync, or answers that it is in play. The held PC shows the stock "Waiting for <name> to finish loading..." strip.
 *  skip    RE6 style, no popup and no pause in co-op: a press of the stock skip inputs on a skippable scene is this player's vote; the scene ends when both voted, through
 *          the stock end of its kind (movies: the loader's query 0x426AD0; mini demos: the vote bit 0x9F4DF0 that 0x9F3200 / sDemo 0xAA0440 read; player demos:
 *          sDemo+0x1960 that the uSmPlayerDemo action turns into its own skip). Strips: LT_SKIP_WAIT / LT_SKIP_PARTNER.
 *  starts  one start per in-engine scene and PC (sc_dd): the scene key is the LayoutID of the scene's camera director (the request's +0x70 is the director + 0x4214)
 *  door    a scene set model (uSmPlayerDemo / uSmOtherDemo) moves only forward (door.inc calls scene_door_veto)
 *  clock   ping / pong every 2 s, the start of every point / scene stamped and the gap logged on both PCs ("scene: start gap ...") */
#include "scene_logic.h"
#include "objsync_logic.h"   /* os_has_class (door.inc includes it again: guarded) */

enum { EV_SC = 0x172 };   /* CP_EV_USER + 0x72 SCENE_SYNC (docs\protocol.md): handled in every glue state (coop_game.inc handle_notes) */
enum { SCO_CAPS = 1, SCO_READY = 2, SCO_INPLAY = 3, SCO_VOTE = 4, SCO_SKEY = 5, SCO_PING = 6, SCO_PONG = 7, SCO_STAMP = 8, SCO_LOADING = 9, SCO_SREADY = 10, SCO_TRIG = 11 };
enum { SC_CAPS_VER = 2 };   /* CAPS byte 2: 1 = the barrier, votes, dedupe; 2 = also the gate of the first scene after a load (SREADY) */
enum { SCV_MOVIE = 1, SCV_MINI = 2, SCV_PDEMO = 3 };
enum { SCT_PLAY = 0, SCT_MOVIE = 1, SCT_SCENE = 2, SCT_MOVIE_END = 3, SCT_N = 4 };
static const char *const sct_names[SCT_N] = {"play start after the load", "movie start", "in-engine scene start", "movie end"};
static const char *const scv_names[4] = {"?", "movie", "mini demo", "player demo"};

static int g_sc_on = 1;
static sc_bar g_sc_bar;
static sc_vote g_sc_vote[4];
static sc_dd g_sc_dd;
static sc_clock g_sc_clk;
static uint32_t g_sc_caps_t, g_sc_ping_t, g_sc_ready_t, g_sc_banner_t, g_sc_hold_log_t, g_sc_strip_t, g_sc_inplay_since;
static int g_sc_strip;            /* the skip strip this file shows (0 none) */
static int g_sc_caps_seen;        /* the partner's CAPS were logged */
static int g_sc_key_quiet;   /* sc_req_key from the per-frame slot start: no log line */
static unsigned g_sc_holds, g_sc_rel[8], g_sc_votes_tx, g_sc_votes_rx, g_sc_skipped, g_sc_dd_sup, g_sc_dd_skip, g_sc_door_veto, g_sc_gaps, g_sc_key_logs;
static unsigned g_sc_set3a8;      /* the aGame whose +0x3A8 this file set (the stock popup does not ask then) */
static int g_sc_mini_applied, g_sc_movie_state, g_sc_mini_state;
static char g_sc_skey[256][SC_KEY_MAX];   /* the key of the partner's SCENE id (SKEY comes before the chunks) */
static char g_sc_cur_movie[SC_KEY_MAX];
static struct { char key[SC_KEY_MAX]; uint32_t t; } g_sc_pstart;   /* the partner's last in-engine scene start (its STAMP) */
static unsigned g_sc_hold_serial, g_sc_hold_n, g_sc_stamped[8], g_sc_stamped_i, g_sc_scene_holds;
static uint32_t g_sc_hold_t0;
static int g_sc_hold_on = 0;   /* `net scene hold 0|1` (LAB): the first-start hold of in-engine scenes. Off: live sc032c showed that the camera director and the scene's
                                   motion run on while the slot start waits (N100: camera frame 0.0 -> 65.4 and the player moved in a 1073 ms hold, 5.7 -> 66.7 in 1018 ms),
                                   so the held PC only lost the scene's first second; the first scene after a load is a measured limit instead */
static unsigned g_sc_kc_slot, g_sc_kc_serial;
static char g_sc_kc_key[SC_KEY_MAX];   /* the key of the slot being started (asked every frame while the slot waits) */
static float g_sc_movie_frame;
static struct { int have; char key[SC_KEY_MAX]; uint32_t t; int extra; } g_sc_st_mine[SCT_N], g_sc_st_theirs[SCT_N];
static sc_gate g_sc_gate;          /* the gate of the first in-engine scene after a load (scene_logic.h) */
static int g_sc_gate_on = 0;       /* `net scene gate 0|1` (LAB). Off: live sc032f, the earlier PC waited 10.9 s and its director's camera motion ran on during the wait
                                      (camera frame 0.0 at wait start and 4.4 s in, 137.2 at the release): its scene started 2.3 s into the camera path although both starts
                                      were 50 ms apart; the first scene after a load stays a measured limit */
static int g_sc_gate_shot;         /* LAB `net scene gateshot 0|1`: one back-buffer shot 1.5 s into a wait (what the waiting PC shows) */
static int g_sc_pcaps_ver;         /* the partner's CAPS byte 2 (0 = not heard) */
static uint32_t g_sc_gate_arm_t, g_sc_gate_tx_t, g_sc_gate_banner_t;
static int g_sc_gate_shot_done, g_sc_gate_logged;
static unsigned g_sc_gate_waits, g_sc_gate_rel[SC_GATE_N];
static unsigned g_sc_ds_last_d, g_sc_ds_logs, g_sc_ds_calls;   /* the directors' submit attempts (h_dsub's log) */
static int g_sc_ds_last_r = -1;

static uint32_t sc_now(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint32_t)(c.QuadPart * 1000 / f.QuadPart);
}

static int sc_active(void) { return g_sc_on && g_w_on && g_cs && cs_active(g_cs) && g_state >= G_SETUP; }
static int sc_partner(char *name, int size)
{
    if (name && size) name[0] = 0;
    if (!g_cs || !cs_active(g_cs)) return 0;
    int pid = cs_remote_pid(g_cs);
    cs_player_info pi;
    if (pid < 0 || !cs_player(g_cs, pid, &pi) || !pi.connected) return 0;
    if (name && size) snprintf(name, (size_t)size, "%s", pi.name);
    return 1;
}
/* the partner is there and runs the scene sync: the co-op rules apply (otherwise the stock behaviour) */
static int sc_coop(void) { return sc_active() && sc_partner(NULL, 0) && g_sc_bar.caps > 0; }

static void sc_send(const unsigned char *b, unsigned len)
{
    if (g_sc_on && g_cs && cs_active(g_cs)) cs_send_event(g_cs, EV_SC, b, len);
}
static void sc_send_key(int op, int a, const char *key)
{
    unsigned char b[4 + SC_KEY_MAX];
    size_t n = key ? strlen(key) : 0;
    if (n > SC_KEY_MAX - 1) n = SC_KEY_MAX - 1;
    b[0] = (unsigned char)op, b[1] = (unsigned char)a;
    if (n) memcpy(b + 2, key, n);
    b[2 + n] = 0;
    sc_send(b, (unsigned)(3 + n));
}
static void sc_send_caps(void)
{
    unsigned char b[3] = {SCO_CAPS, (unsigned char)(g_sc_on != 0), (unsigned char)(g_sc_gate_on ? SC_CAPS_VER : 1)};
    if (g_cs && cs_active(g_cs)) cs_send_event(g_cs, EV_SC, b, 3);   /* CAPS goes out even when off: the partner then lets its holds go at once */
}
static void sc_get_key(const cp_event *e, unsigned at, char *key)
{
    unsigned n = e->len > at ? e->len - at : 0;
    if (n > SC_KEY_MAX - 1) n = SC_KEY_MAX - 1;
    memcpy(key, e->body + at, n);
    key[n] = 0;
}

/* the point's key: kind 7 = the story area ("a100"), kind 8 = the movie's name ("d1000", sCutscene+0x58 = its MtString, characters at +8) */
static void sc_point_key(unsigned kind, char *key)
{
    key[0] = 0;
    if (kind == SC_KIND_LOAD) {
        snprintf(key, SC_KEY_MAX, "a%u", story_area_id());
        return;
    }
    unsigned cs = rd32(0x1579134), s = cs ? rd32(cs + 0x58) : 0;
    char nm[SC_KEY_MAX] = "";
    if (s && r2_safe_read((void *)(s + 8), nm, SC_KEY_MAX - 1)) nm[SC_KEY_MAX - 1] = 0;
    for (int i = 0; nm[i]; i++)
        if ((unsigned char)nm[i] < 0x20 || (unsigned char)nm[i] > 0x7E) nm[i] = 0;
    snprintf(key, SC_KEY_MAX, "%s", nm[0] ? nm : "movie");
}

/* ---------------------------------------------------------------- stamps: the start of a point / scene on both PCs, the gap */
static void sc_gap_try(int what)
{
    if (!g_sc_st_mine[what].have || !g_sc_st_theirs[what].have || !sc_key_eq(g_sc_st_mine[what].key, g_sc_st_theirs[what].key)) return;
    if (!g_sc_clk.have) {
        r2_log(W_LOG "scene: start gap of the %s (%s): no clock sample of the partner yet", sct_names[what], g_sc_st_mine[what].key);
    } else {
        int32_t gap = sc_gap(&g_sc_clk, g_sc_st_mine[what].t, g_sc_st_theirs[what].t);
        if (gap > -600000 && gap < 600000) {
            g_sc_gaps++;
            r2_log(W_LOG "scene: start gap of the %s (%s): %+d ms (+ = this PC later; here %u, partner %u on its clock, clock offset %d ms, round trip %u ms, %u samples)%s", sct_names[what],
                   g_sc_st_mine[what].key, (int)gap, g_sc_st_mine[what].t, g_sc_st_theirs[what].t, (int)g_sc_clk.offset, g_sc_clk.rtt, g_sc_clk.samples,
                   what == SCT_MOVIE_END ? "" : (gap > 500 || gap < -500 ? " OVER 0.5 s" : " within 0.5 s"));
            if (what == SCT_MOVIE_END) r2_log(W_LOG "scene: movie %s ended here at frame %d, on the partner's PC at frame %d", g_sc_st_mine[what].key, g_sc_st_mine[what].extra, g_sc_st_theirs[what].extra);
        }
    }
    g_sc_st_mine[what].have = g_sc_st_theirs[what].have = 0;
}
static void sc_stamp(int what, const char *key, uint32_t t, int extra)
{
    g_sc_st_mine[what].have = 1, sc_key_set(g_sc_st_mine[what].key, key), g_sc_st_mine[what].t = t, g_sc_st_mine[what].extra = extra;
    unsigned char b[12 + SC_KEY_MAX];
    size_t n = strlen(key);
    if (n > SC_KEY_MAX - 1) n = SC_KEY_MAX - 1;
    b[0] = SCO_STAMP, b[1] = (unsigned char)what;
    memcpy(b + 2, &t, 4), memcpy(b + 6, &extra, 4);
    memcpy(b + 10, key, n), b[10 + n] = 0;
    sc_send(b, (unsigned)(11 + n));
    sc_gap_try(what);
}

/* ---------------------------------------------------------------- the start barrier (story.inc h_syncset / h_synccheck call these while g_sc_on) */
static void scene_sync_set(unsigned kind)
{
    if (kind != SC_KIND_LOAD && kind != SC_KIND_MOVIE) return;
    if (!sc_active()) return;   /* no session: the stock point passes */
    char key[SC_KEY_MAX], name[40];
    sc_point_key(kind, key);
    uint32_t now = sc_now();
    if (!sc_bar_arrive(&g_sc_bar, kind, key, now)) return;
    g_sc_holds++, g_sc_ready_t = now, g_sc_hold_log_t = now, g_sc_banner_t = 0;
    sc_send_key(SCO_READY, (int)kind, key);
    if (g_sc_bar.caps == 0) sc_send_caps();
    int p = sc_partner(name, sizeof name);
    const sc_report *r = &g_sc_bar.ready[kind == SC_KIND_MOVIE];
    r2_log(W_LOG "scene: %s point (%s) reached here: HOLD for the partner (%s%s, scene sync %s, its last READY %s)", kind == SC_KIND_LOAD ? "loader end (kind 7)" : "movie start (kind 8)", key,
           p ? "partner " : "no partner", p ? name : "", g_sc_bar.caps > 0 ? "on" : g_sc_bar.caps < 0 ? "off" : "not heard yet", r->have ? r->key : "none");
}

/* 1 = the point is open here (the stock check runs), 0 = hold this frame */
static int scene_sync_check(unsigned kind)
{
    if (kind != SC_KIND_LOAD && kind != SC_KIND_MOVIE) return 1;
    if (!g_sc_bar.holding || g_sc_bar.kind != kind) return 1;
    uint32_t now = sc_now();
    char name[40];
    int partner = sc_partner(name, sizeof name);
    int r = sc_bar_decide(&g_sc_bar, now, partner, sc_active(), g_state == G_RUN);
    if (r == SC_HOLD) {
        if (!g_sc_banner_t || now - g_sc_banner_t >= 1000) g_sc_banner_t = now ? now : 1, menu_banner(MB_WAIT, name, 2500);
        if (now - g_sc_ready_t >= 3000) g_sc_ready_t = now, sc_send_key(SCO_READY, (int)kind, g_sc_bar.key);   /* the partner may have missed it or answers INPLAY now */
        if (now - g_sc_hold_log_t >= 10000) {
            g_sc_hold_log_t = now;
            r2_log(W_LOG "scene: still holding at the %s point (%s) for %s: %u s", kind == SC_KIND_LOAD ? "loader end" : "movie start", g_sc_bar.key, name[0] ? name : "the partner", (now - g_sc_bar.since) / 1000);
        }
        return 0;
    }
    char key[SC_KEY_MAX];
    sc_key_set(key, g_sc_bar.key);
    uint32_t held = now - g_sc_bar.since;
    g_sc_rel[r & 7]++;
    r2_log(W_LOG "scene: %s point (%s) RELEASED after %u ms: %s", kind == SC_KIND_LOAD ? "loader end (kind 7)" : "movie start (kind 8)", key, held, sc_rel_names[r]);
    sc_bar_release(&g_sc_bar, now);
    if (g_sc_banner_t) menu_banner(MB_NONE, NULL, 0), g_sc_banner_t = 0;
    sc_stamp(kind == SC_KIND_LOAD ? SCT_PLAY : SCT_MOVIE, key, now, 0);
    if (kind == SC_KIND_LOAD) sc_gate_arm(&g_sc_gate), g_sc_gate_arm_t = now;   /* the gate's window counts from the loader end (live sc032e: the glue of the session's first
                                                                                    level was in set-up since the session start, the window had run out before the opening scene) */
    if (kind == SC_KIND_MOVIE) sc_vote_begin(&g_sc_vote[SCV_MOVIE], key, now), sc_key_set(g_sc_cur_movie, key);
    return 1;
}

/* ---------------------------------------------------------------- the skip inputs of the stock popups (0x42B95x / 0xA9E6xx): the human's pad BACK / Start bit 8,
 * the keyboard's skip key in keyboard mode (thiscall [0x15DE918] 0x99E1E0(3, 0x32, 2)), sBioMain+0x28624 */
static int g_sc_lab_press;   /* LAB `net scene vote`: the next skip query counts as a press (labelled shortcut of the tests) */
static const char *const sc_press_names[5] = {"-", "the pad's skip button", "the keyboard's skip key", "sBioMain+0x28624", "LAB net scene vote"};
static int sc_skip_pressed(void)   /* 0 none, 1 pad, 2 keyboard, 3 sBioMain+0x28624, 4 LAB */
{
    if (g_sc_lab_press) {
        g_sc_lab_press = 0;
        return 4;
    }
    unsigned pad = rd32(0x15DE5DC), bm = rd32(0x15DE6F8), sb = rd32(0x157D120), kb = rd32(0x15DE918);
    unsigned p = player_obj(g_my_slot);
    if (pad) {
        if (p && rd32(p + 0x7920) == 0) {
            unsigned a[3] = {pad, rd32(p + 0x7924) & 0xFF, 0};
            if (do_call(2, 0xA96970, a, 3) & 8) return 1;
        } else if (!p) {   /* no player objects yet (a movie at the start of a half): the logical pads, as the loader's own check */
            for (unsigned i = 0; i < 2; i++) {
                unsigned a[3] = {pad, i, 0};
                if (do_call(2, 0xA96970, a, 3) & 8) return 1;
            }
        }
    }
    if (sb && rd32(sb + 0x15C4BC) == 1 && kb) {
        unsigned a[4] = {kb, 3, 0x32, 2};
        if (do_call(2, 0x99E1E0, a, 4) & 0xFF) return 2;
    }
    return bm && (rd32(bm + 0x28624) & 0xFF) ? 3 : 0;
}

static void sc_vote_press(int cls, const char *key, int how)
{
    sc_vote *v = &g_sc_vote[cls];
    if (!sc_vote_mine(v)) return;
    g_sc_votes_tx++;
    sc_send_key(SCO_VOTE, cls, v->key);
    char name[40];
    sc_partner(name, sizeof name);
    r2_log(W_LOG "scene: skip pressed here (%s %s, %s): vote sent%s", scv_names[cls], key, sc_press_names[how >= 1 && how <= 4 ? how : 0], v->theirs ? ", the partner voted already" : ", waiting for the partner");
}

/* the loader's movie skip query FUN_00426AD0 (fastcall, ECX = aGame): only called from the loader (aGame state 8 sub 1) */
typedef unsigned char(__attribute__((fastcall)) *skipq_t)(unsigned ag);
static skipq_t o_skipq;
static __attribute__((fastcall)) unsigned char h_skipq(unsigned ag)
{
    sc_vote *v = &g_sc_vote[SCV_MOVIE];
    unsigned gm = rd32(0x157AE00);
    if (!sc_coop() || (gm && rd32(gm + 0x2C) == 4)) {
        if (g_sc_set3a8 == ag && !v->mine) {   /* the partner went away mid-movie: the stock popup asks again (this player did not vote) */
            unsigned char z = 0;
            wr(ag + 0x3A8, &z, 1);
        }
        g_sc_set3a8 = 0;
        return o_skipq(ag);
    }
    unsigned char one = 1;
    wr(ag + 0x3A8, &one, 1);   /* the stock popup (and its pause of the movie on this PC only) asks only while +0x3A8 == 0 */
    g_sc_set3a8 = ag;
    unsigned sc = rd32(0x1579134);
    unsigned a1[1] = {sc};
    int skippable = sc && (do_call(3, 0x8436D0, a1, 1) & 0xFF);   /* the stock rule: past frame 59 */
    if (!v->active) {
        char key[SC_KEY_MAX];
        sc_point_key(SC_KIND_MOVIE, key);
        sc_vote_begin(v, key, sc_now());
    }
    int how = skippable ? sc_skip_pressed() : 0;
    if (how) sc_vote_press(SCV_MOVIE, v->key, how);
    if (!skippable || !sc_vote_skip(v, 1)) return 0;
    g_sc_skipped++;
    r2_log(W_LOG "scene: both players skipped the movie %s: it ends through the loader's own skip", v->key);
    return 1;
}

/* the network vote bit FUN_009F4DF0 (ECX = sNetwork): in co-op only reached from aGame state 9 (a mini demo; the loader's query above does not call it) */
typedef void(__attribute__((fastcall)) *netvote_t)(unsigned net);
static netvote_t o_netvote;
static __attribute__((fastcall)) void h_netvote(unsigned net)
{
    if (!sc_coop()) {
        o_netvote(net);
        return;
    }
    sc_vote *v = &g_sc_vote[SCV_MINI];
    if (!v->active) {
        char key[SC_KEY_MAX];
        snprintf(key, sizeof key, "mini@%u", story_area_id());
        sc_vote_begin(v, key, sc_now());
    }
    sc_vote_press(SCV_MINI, v->key, 1);
    if (sc_vote_skip(v, 1) && !g_sc_mini_applied) {
        g_sc_mini_applied = 1, g_sc_skipped++;
        r2_log(W_LOG "scene: both players skipped the mini demo (%s): the stock vote bit is set", v->key);
        o_netvote(net);
    }
}

/* the player-demo skip popup FUN_00A9E5C0 (ECX = sDemo), every frame from sDemo's update: in co-op the popup never opens (+0x195F hidden from it), the press is the vote */
typedef void(__attribute__((fastcall)) *pdpop_t)(unsigned sd);
static pdpop_t o_pdpop;
static __attribute__((fastcall)) void h_pdpop(unsigned sd)
{
    if (!sc_coop()) {
        o_pdpop(sd);
        return;
    }
    unsigned skippable = rd32(sd + 0x195F) & 0xFF;
    sc_vote *v = &g_sc_vote[SCV_PDEMO];
    if (skippable && !(rd32(sd + 0x1960) & 0xFF)) {
        if (!v->active) {
            char key[SC_KEY_MAX];
            snprintf(key, sizeof key, "pd@%u", story_area_id());
            sc_vote_begin(v, key, sc_now());
        }
        int how = sc_skip_pressed();
        if (how) sc_vote_press(SCV_PDEMO, v->key, how);
        if (sc_vote_skip(v, 1)) {
            unsigned char one = 1;
            wr(sd + 0x1960, &one, 1);   /* the stock "yes" of the popup: the uSmPlayerDemo action sets its own skip */
            g_sc_skipped++;
            r2_log(W_LOG "scene: both players skipped the player demo (%s): sDemo+0x1960 set (the stock skip)", v->key);
        }
    }
    if (skippable) {
        unsigned char z = 0;
        wr(sd + 0x195F, &z, 1);
    }
    o_pdpop(sd);
    if (skippable) {
        unsigned char s = (unsigned char)skippable;
        wr(sd + 0x195F, &s, 1);
    }
}

/* ---------------------------------------------------------------- one start per in-engine scene (story.inc demosub_common / scene_on_chunk) */
static int sc_playing(uint32_t serial)
{
    unsigned sd = rd32(0x1581788);
    if (!sd) return 0;
    unsigned a[3] = {sd, 0xFFFFFFFFu, serial};
    return (int)(do_call(2, 0xA9E400, a, 3) & 0xFF);   /* FUN_00A9E400(player -1, serial): a type-3 slot of that serial in state 1 / 2 */
}

/* the set model that owns a scene unit: a scene set (uSmPlayerDemo) lists its units in two levels at +0x436C (FUN_00773EA0 binds each of them); the set models are the
 * two lists of sSetModel (cell 0x1581920, +0x30 / +0x3C and +0x44 / +0x50, as door.inc scans them). Bounded, safe reads; 0 when none lists it */
static unsigned sc_owner_sm(unsigned unit)
{
    unsigned m = rd32(0x1581920);
    if (!m || !unit) return 0;
    for (int k = 0; k < 2; k++) {
        unsigned c = rd32(m + (k ? 0x44 : 0x30)), a = rd32(m + (k ? 0x50 : 0x3C));
        for (unsigned i = 0; i < c && i < 512 && a; i++) {
            unsigned o = rd32(a + 4 * i), l1 = o ? rd32(o + 0x436C) : 0, n1 = l1 ? rd32(l1 + 4) : 0, v1 = l1 ? rd32(l1 + 0x10) : 0;
            if (!n1 || n1 > 16 || !v1) continue;
            for (unsigned j = 0; j < n1; j++) {
                unsigned l2 = rd32(v1 + 4 * j), n2 = l2 ? rd32(l2 + 4) : 0, v2 = l2 ? rd32(l2 + 0x10) : 0;
                if (!n2 || n2 > 64 || !v2) continue;
                for (unsigned q = 0; q < n2; q++)
                    if (rd32(v2 + 4 * q) == unit) return o;
            }
        }
    }
    return 0;
}

/* the key of a type-3 request: its camera director (+0x70 = director + 0x4214, cDemoCameraControllerActionBase 0x75D3A0) by its own LayoutID, else (live sc032a: the
 * directors have none) by the LayoutID of the scene set model that lists it ("S100,105,0" = uSmPlayerDemo c1s0500e00_WakeUp); "" when neither is found */
static void sc_dir_key(unsigned d, char *key);
static void sc_req_key(int idx, const void *req, char *key)
{
    key[0] = 0;
    if (idx != 2 || !req) return;
    unsigned d = rd32((unsigned)req + 0x70);
    if (d < 0x10000u + 0x4214u) return;
    sc_dir_key(d - 0x4214u, key);
}
/* the key of a camera director (the object itself) */
static void sc_dir_key(unsigned d, char *key)
{
    key[0] = 0;
    if (d < 0x10000u) return;
    unsigned vt = rd32(d);
    if (vt < 0x1000000u || vt > 0x1500000u) return;
    layout_t l;
    unsigned sm = 0;
    if (layout_of(d, &l) && l.a > 0 && l.a <= 0x7FFF) snprintf(key, SC_KEY_MAX, "L%d,%u,%u", l.a, l.b, l.c);
    else if ((sm = sc_owner_sm(d)) && layout_of(sm, &l) && l.a > 0 && l.a <= 0x7FFF) snprintf(key, SC_KEY_MAX, "S%d,%u,%u", l.a, l.b, l.c);
    if (!g_sc_key_quiet && g_sc_key_logs++ < 30) {
        char cn[100], cs[100];
        r2_log(W_LOG "scene: key of the scene: %s (camera director %08x %s, layout %d,%u,%u; owner set model %08x %s)", key[0] ? key : "none: no dedupe for it", d, w_class(d, cn, sizeof cn),
               (int)rd32(d + 0x19CC), rd32(d + 0x19D0) & 0xFFFF, rd32(d + 0x19D0) >> 16, sm, sm ? w_class(sm, cs, sizeof cs) : "-");
    }
}

/* this PC's own submit, before it goes to sDemo: return the serial of the partner's copy that plays (no restart), or 0 */
static unsigned scene_own_submit_check(int idx, void *req)
{
    if (!sc_coop() || idx != 2) return 0;
    char key[SC_KEY_MAX];
    sc_req_key(idx, req, key);
    uint32_t now = sc_now();
    if (!sc_dd_own_suppress(&g_sc_dd, key, now, sc_playing)) return 0;
    int i = sc_dd_find(&g_sc_dd, key, SC_FROM_PARTNER, now);
    unsigned s = g_sc_dd.e[i].serial;
    g_sc_dd_sup++, g_sc_dd.suppressed++;
    r2_log(W_LOG "scene: own submit of %s suppressed: the partner's copy of this scene plays here (serial %u, started %u ms ago): no restart", key, s, now - g_sc_dd.e[i].t);
    return s ? s : 1;
}
/* this PC's own submit went to sDemo (serial r); id = the SCENE id its chunks carry */
static void scene_own_started(int idx, void *req, unsigned r, unsigned id)
{
    if (!g_sc_on || idx != 2 || r == 0xFFFFFFFFu) return;
    char key[SC_KEY_MAX];
    sc_req_key(idx, req, key);
    uint32_t now = sc_now();
    sc_dd_note(&g_sc_dd, key, SC_FROM_OWN, r, now);
    if (!sc_active()) return;
    sc_send_key(SCO_SKEY, (int)(id & 0xFF), key);   /* (the start stamp comes from the slot start, h_slotstart) */
}
/* the partner's SCENE id is complete: skip it? */
static int scene_gate_keeps_own(unsigned id)   /* the partner's SCENE id is complete while the gate is armed: not submitted (scene_logic.h sc_gate_keeps_own) */
{
    if (!g_sc_gate_on || !sc_coop()) return 0;
    uint32_t now = sc_now();
    if (!sc_gate_keeps_own(&g_sc_gate, now, g_sc_gate_arm_t, g_sc_pcaps_ver >= SC_CAPS_VER)) return 0;
    g_sc_dd_skip++;
    r2_log(W_LOG "scene: the partner's SCENE %u (%s) not submitted: the first scene after the load starts from this PC's own story (gate armed %u ms after the load)", id & 0xFF,
           g_sc_skey[id & 0xFF][0] ? g_sc_skey[id & 0xFF] : "no key", now - g_sc_gate_arm_t);
    return 1;
}
static int scene_partner_scene_skip(unsigned id)
{
    if (!sc_coop()) return 0;
    const char *key = g_sc_skey[id & 0xFF];
    uint32_t now = sc_now();
    if (!sc_dd_partner_skip(&g_sc_dd, key, now, sc_playing)) return 0;
    int i = sc_dd_find(&g_sc_dd, key, SC_FROM_OWN, now);
    g_sc_dd_skip++, g_sc_dd.skipped++;
    r2_log(W_LOG "scene: the partner's SCENE %u (%s) not submitted: this PC's own copy plays (serial %u, started %u ms ago): no restart", id & 0xFF, key, g_sc_dd.e[i].serial, now - g_sc_dd.e[i].t);
    return 1;
}
static struct { unsigned serial; char key[SC_KEY_MAX]; uint32_t t; int checked, probes; } g_sc_ap;   /* the partner's copy submitted here last: its sDemo serial and the key its SKEY named */

/* this PC's own camera director of a scene key "S<a>,<b>,<c>": the uDemoCameraController among the units of the scene set model with that LayoutID; 0 when none */
static unsigned sc_find_dir(const char *key)
{
    if (!key || key[0] != 'S') return 0;
    char *e;
    long a = strtol(key + 1, &e, 10);
    if (*e != ',') return 0;
    unsigned long b = strtoul(e + 1, &e, 10);
    if (*e != ',') return 0;
    unsigned long c = strtoul(e + 1, &e, 10);
    unsigned m = rd32(0x1581920);
    if (!m) return 0;
    for (int k = 0; k < 2; k++) {
        unsigned cnt = rd32(m + (k ? 0x44 : 0x30)), arr = rd32(m + (k ? 0x50 : 0x3C));
        for (unsigned i = 0; i < cnt && i < 512 && arr; i++) {
            unsigned o = rd32(arr + 4 * i);
            layout_t l;
            if (!o || !layout_of(o, &l) || l.a != (int)a || l.b != b || l.c != c) continue;
            unsigned l1 = rd32(o + 0x436C), n1 = l1 ? rd32(l1 + 4) : 0, v1 = l1 ? rd32(l1 + 0x10) : 0;
            for (unsigned j = 0; j < n1 && j < 16 && v1; j++) {
                unsigned l2 = rd32(v1 + 4 * j), n2 = l2 ? rd32(l2 + 4) : 0, v2 = l2 ? rd32(l2 + 0x10) : 0;
                for (unsigned q = 0; q < n2 && q < 64 && v2; q++) {
                    unsigned u = rd32(v2 + 4 * q);
                    char cn[100];
                    if (u && strstr(w_class(u, cn, sizeof cn), "DemoCamera")) return u;
                }
            }
            return 0;
        }
    }
    return 0;
}
/* this PC's scene set model (uSmPlayerDemo / uSmOtherDemo) of a key "S<a>,<b>,<c>"; 0 when none (block not loaded) or another class */
static unsigned sc_find_sm(const char *key)
{
    if (!key || key[0] != 'S') return 0;
    char *e;
    long a = strtol(key + 1, &e, 10);
    if (*e != ',') return 0;
    unsigned long b = strtoul(e + 1, &e, 10);
    if (*e != ',') return 0;
    unsigned long c = strtoul(e + 1, &e, 10);
    unsigned m = rd32(0x1581920);
    if (!m) return 0;
    for (int k = 0; k < 2; k++) {
        unsigned cnt = rd32(m + (k ? 0x44 : 0x30)), arr = rd32(m + (k ? 0x50 : 0x3C));
        for (unsigned i = 0; i < cnt && i < 512 && arr; i++) {
            unsigned o = rd32(arr + 4 * i);
            layout_t l;
            if (!o || !layout_of(o, &l) || l.a != (int)a || l.b != b || l.c != c) continue;
            static char big[4096];
            classes_name(o, big, sizeof big - 1);
            big[sizeof big - 1] = 0;
            const char *p = strstr(big, ": ");
            p = p ? p + 2 : big;
            return os_has_class(p, "uSmPlayerDemo") || os_has_class(p, "uSmOtherDemo") ? o : 0;
        }
    }
    return 0;
}

/* the start of a scene set model's directors FUN_00773EA0 (thiscall set model, player index; ret 4): for every unit at +0x436C unit vtable +0x160(set model, player)
 * and ChangeAction(2, 0, 0, -1, 0), then +0x37B8 |= 4, +0x43EC = 1, +0x43F0 = player. Called by the play action FUN_007750E0 in its state 2; scene.inc calls it for a
 * received SCENE (scene_rx_takes) and skips the stock's second start of a set model it started (no restart of the running directors) */
typedef void(__attribute__((thiscall)) *smstart_t)(void *sm, unsigned player);
static smstart_t o_smstart;
static struct { unsigned sm; char key[SC_KEY_MAX]; uint32_t t; int probes; unsigned player; } g_sc_ls;   /* the scene set model this PC started from a received SCENE */
static unsigned g_sc_ls_starts, g_sc_ls_dup, g_sc_ls_logs;
static __attribute__((thiscall)) void h_smstart(void *self, unsigned player)
{
    unsigned sm = (unsigned)self;
    if (g_sc_on && sm && sm == g_sc_ls.sm && sc_now() - g_sc_ls.t < 120000u && (rd32(sm + 0x43EC) & 0xFF)) {
        g_sc_ls_dup++;
        r2_log(W_LOG "scene: the story's own start of scene set model %08x (%s, player %u) skipped: this PC started it %u ms ago from the partner's SCENE (no restart)", sm,
               g_sc_ls.key, player, sc_now() - g_sc_ls.t);
        return;
    }
    if (g_sc_on && g_sc_ls_logs++ < 40) {
        layout_t l;
        layout_of(sm, &l);
        r2_log(W_LOG "scene: scene set model %08x (%d,%u,%u) starts its directors here (the story's own start, player %u)", sm, l.a, l.b, l.c, player);
    }
    o_smstart(self, player);
}

static void sc_local_dir_log(const char *what)   /* the received copy's scene on this PC: does this PC's own director of it move */
{
    unsigned ld = sc_find_dir(g_sc_ap.key), cm = ld ? rd32(ld + 0x41E0) : 0, sm = ld ? sc_owner_sm(ld) : 0;
    r2_log(W_LOG "scene: the partner's copy of %s %s: this PC's own director %08x camera frame %.1f, +0x4204 %u, set model action %d, submit calls %u", g_sc_ap.key, what, ld,
           cm ? rdf(cm + 0x34) : -1.0f, ld ? rd32(ld + 0x4204) : 0, sm ? (int)rd32(sm + 0x1A54) : -9, g_sc_ds_calls);
}
static void sc_ls_log(const char *what)   /* the scene this PC started from a received SCENE: its own director's state */
{
    unsigned ld = sc_find_dir(g_sc_ls.key), cm = ld ? rd32(ld + 0x41E0) : 0, sm = g_sc_ls.sm;
    r2_log(W_LOG "scene: this PC's own run of %s %s: director %08x camera frame %.1f, set model action %d (+0x43EC %u), director submit attempts %u", g_sc_ls.key, what, ld,
           cm ? rdf(cm + 0x34) : -1.0f, sm ? (int)rd32(sm + 0x1A54) : -9, sm ? rd32(sm + 0x43EC) & 0xFF : 0, g_sc_ds_calls);
}
/* ---------------------------------------------------------------- the sequence of a scene set model (live evidence of the camera clock) */
/* a scene set model's state: the sequence FUN_00778690 (+0x43E8: 3 = waits for a trigger, 4-5 fade, 6 start, 7 runs, 8.. end), its action (0 Wait, 1 Move, 2 End), +0x45AD
 * (the Move action waits for it), the trigger (+0x43EC, +0x37B8 bit 4, player +0x43F0), and its scheduler +0x4360 = the camera directors' clock (frame +0x34, held +0x40,
 * the bits +0xC & 0xC00 that the Move action's FUN_007781D0 sets before the directors start) */
static void sc_sm_state(unsigned sm, char *out, int size)
{
    unsigned sch = rd32(sm + 0x4360);
    snprintf(out, (size_t)size, "sequence state %d, action %d, +0x45AD %u, trigger +0x43EC %u bit4 %u player %d, scheduler %08x frame %.1f held %u bits %03x",
             (int)rd32(sm + 0x43E8), (int)rd32(sm + 0x1A54), rd32(sm + 0x45AD) & 0xFF, rd32(sm + 0x43EC) & 0xFF, (rd32(sm + 0x37B8) >> 2) & 1, (int)rd32(sm + 0x43F0), sch,
             sch ? rdf(sch + 0x34) : -1.0f, sch ? rd32(sch + 0x40) & 0xFF : 0, sch ? rd32(sch + 0xC) & 0xC00 : 0);
}
/* the scene set model triggered here (own trigger, the partner's TRIG, the local start from a SCENE): one line whenever the sequence state, the action, +0x45AD, the trigger
 * or the scheduler's held flag / bits change (at most 40 lines, up to 60 s or the end of its run) */
static struct { unsigned sm; char key[SC_KEY_MAX]; uint32_t t0, ul_t, vw_t; int lines, ul_n, vw_n; const char *how; int seq, act, f45, ec, held, bits; } g_sc_wt;
static void sc_units_log(unsigned sm, const char *key, const char *what);
static unsigned sc_find_dir(const char *key);
/* what this PC's viewport shows while a watched scene runs (probe of the guest's framing, sc6c): the sBioCamera viewport cameras (+0x34 + i * 400) and the players they
 * follow (+0xCF0 / +0xCF4), the scheduler camera the slot start hands them (area +0x3830 -> +0x1D8), the director's motion camera (+0x4264: bound camera +0x130, blend +0x134,
 * +0x1BC) and slot bits / viewport switch (+0x4260, +0x4270 / +0x4278 / +0x4280 / +0x4288 / +0x4290), both players' cameras (+0x4780) and positions */
static void sc_view_log(const char *key, uint32_t ms)
{
    unsigned bc = rd32(0x15DE88C), ar = rd32(0x15DE720), sa = ar ? rd32(ar + 0x3830) : 0, d = sc_find_dir(key), mc = d ? rd32(d + 0x4264) : 0;
    unsigned v0 = bc ? rd32(bc + 0x34) : 0, v1 = bc ? rd32(bc + 0x34 + 400) : 0, p0 = player_obj(0), p1 = player_obj(1), b = mc ? rd32(mc + 0x130) : 0;
    r2_log(W_LOG "scene: view %s +%u ms: viewport cams %08x / %08x following players %d / %d; scheduler of the slots %08x; director %08x bits %x switch %d/%d/%d/%d mode %d; "
                 "motion cam %08x at (%.0f,%.0f,%.0f) bound %08x blend %.2f 1bc %u; player cams %08x / %08x; cam0 at (%.0f,%.0f,%.0f); players (%.0f,%.0f,%.0f) act %d / (%.0f,%.0f,%.0f) act %d",
           key, ms, v0, v1, bc ? (int)rd32(bc + 0xCF0) : -9, bc ? (int)rd32(bc + 0xCF4) : -9, sa ? rd32(sa + 0x1D8) : 0, d, d ? rd32(d + 0x4260) : 0, d ? (int)rd32(d + 0x4270) : -9,
           d ? (int)rd32(d + 0x4278) : -9, d ? (int)rd32(d + 0x4280) : -9, d ? (int)rd32(d + 0x4288) : -9, d ? (int)rd32(d + 0x4290) : -9, mc, mc ? rdf(mc + 0x40) : 0.0f,
           mc ? rdf(mc + 0x44) : 0.0f, mc ? rdf(mc + 0x48) : 0.0f, b, mc ? rdf(mc + 0x134) : -1.0f, mc ? rd32(mc + 0x1BC) & 0xFF : 0, p0 ? rd32(p0 + 0x4780) : 0, p1 ? rd32(p1 + 0x4780) : 0,
           v0 ? rdf(v0 + 0x40) : 0.0f, v0 ? rdf(v0 + 0x44) : 0.0f, v0 ? rdf(v0 + 0x48) : 0.0f, p0 ? rdf(p0 + 0x40) : 0.0f, p0 ? rdf(p0 + 0x44) : 0.0f, p0 ? rdf(p0 + 0x48) : 0.0f,
           p0 ? (int)rd32(p0 + 0x1A54) : -1, p1 ? rdf(p1 + 0x40) : 0.0f, p1 ? rdf(p1 + 0x44) : 0.0f, p1 ? rdf(p1 + 0x48) : 0.0f, p1 ? (int)rd32(p1 + 0x1A54) : -1);
    char c0[100] = "-", c1[100] = "-";   /* the current action instance of each player (action manager +0x1A38, current +0x30): the demo action or not */
    unsigned a0 = p0 ? rd32(p0 + 0x1A68) : 0, a1 = p1 ? rd32(p1 + 0x1A68) : 0;
    if (a0) w_class(a0, c0, sizeof c0);
    if (a1) w_class(a1, c1, sizeof c1);
    unsigned gm = rd32(0x157AE00);
    r2_log(W_LOG "scene: view %s +%u ms: players' current actions %08x %s / %08x %s; types %u / %u; local slot %d; sGameMan viewport slots +0x8F8 %d / %d, main slot +0x7A8 %d, "
                 "mode +0x8F0 %d", key, ms, a0, c0, a1, c1, p0 ? rd32(p0 + 0x7920) : 9, p1 ? rd32(p1 + 0x7920) : 9, g_my_slot, gm ? (int)rd32(gm + 0x8F8) : -9,
           gm ? (int)rd32(gm + 0x8FC) : -9, gm ? (int)rd32(gm + 0x7A8) : -9, gm ? (int)rd32(gm + 0x8F0) : -9);
}
static void sc_elist(const char *tag, char *out, int size);
static struct { char key[SC_KEY_MAX]; uint32_t t; } g_sc_el;   /* the enemy list 30 s after a watched scene's units were all ready */
static void sc_elist_auto(const char *key, const char *what)   /* the enemy list of this moment into the log (both PCs do it at the same points of their own scene) */
{
    static char buf[3000];
    char tag[24];
    snprintf(tag, sizeof tag, "%.12s %s", key, what);
    sc_elist(tag, buf, sizeof buf);
}
static void sc_watch_begin(unsigned sm, const char *key, const char *how)
{
    memset(&g_sc_wt, 0, sizeof g_sc_wt);
    g_sc_wt.sm = sm, sc_key_set(g_sc_wt.key, key), g_sc_wt.key[0] = key[0], g_sc_wt.t0 = sc_now(), g_sc_wt.how = how, g_sc_wt.seq = -99;
    sc_elist_auto(g_sc_wt.key, "trig");
}
static int sc_sm_listed(unsigned sm)   /* the set model is still in sSetModel's lists (the End action deletes a one-shot scene set model: FUN_00AA6C70 + vf+0x3C) */
{
    unsigned m = rd32(0x1581920);
    for (int k = 0; m && k < 2; k++) {
        unsigned c = rd32(m + (k ? 0x44 : 0x30)), a = rd32(m + (k ? 0x50 : 0x3C));
        for (unsigned i = 0; i < c && i < 512 && a; i++)
            if (rd32(a + 4 * i) == sm) return 1;
    }
    return 0;
}
static void sc_watch_frame(uint32_t now)
{
    unsigned sm = g_sc_wt.sm;
    if (!sm) return;
    if (!sc_sm_listed(sm)) {
        r2_log(W_LOG "scene: watch %s (%s) ends after %u ms (%d lines): the set model left sSetModel's lists (deleted at its end)", g_sc_wt.key, g_sc_wt.how, now - g_sc_wt.t0,
               g_sc_wt.lines);
        g_sc_wt.sm = 0;
        return;
    }
    unsigned sch = rd32(sm + 0x4360);
    int seq = (int)rd32(sm + 0x43E8), act = (int)rd32(sm + 0x1A54), f45 = (int)(rd32(sm + 0x45AD) & 0xFF), ec = (int)(rd32(sm + 0x43EC) & 0xFF);
    int held = sch ? (int)(rd32(sch + 0x40) & 0xFF) : -1, bits = sch ? (int)(rd32(sch + 0xC) & 0xC00) : -1;
    if (seq != g_sc_wt.seq || act != g_sc_wt.act || f45 != g_sc_wt.f45 || ec != g_sc_wt.ec || held != g_sc_wt.held || bits != g_sc_wt.bits) {
        int was = g_sc_wt.seq;
        g_sc_wt.seq = seq, g_sc_wt.act = act, g_sc_wt.f45 = f45, g_sc_wt.ec = ec, g_sc_wt.held = held, g_sc_wt.bits = bits;
        if (was != seq && (seq == 7 || seq == 8) && g_sc_wt.ul_n < 12)   /* the units when the sequence starts to wait for them (7) and when all are ready (8) */
            g_sc_wt.ul_n++, g_sc_wt.ul_t = now, sc_units_log(sm, g_sc_wt.key, seq == 7 ? "as the sequence waits for them (state 7)" : "all ready (state 8)");
        if (was != seq && seq == 8) {
            sc_elist_auto(g_sc_wt.key, "ready");
            sc_key_set(g_sc_el.key, g_sc_wt.key), g_sc_el.key[0] = g_sc_wt.key[0], g_sc_el.t = now ? now : 1;
        }
        if (g_sc_wt.lines++ < 40) {
            char st[300];
            sc_sm_state(sm, st, sizeof st);
            r2_log(W_LOG "scene: watch %s (%s) +%u ms: %s", g_sc_wt.key, g_sc_wt.how, now - g_sc_wt.t0, st);
        }
    }
    if (seq == 7 && now - g_sc_wt.ul_t >= 1000u && g_sc_wt.ul_n < 12)   /* still waiting: which unit is not ready */
        g_sc_wt.ul_n++, g_sc_wt.ul_t = now, sc_units_log(sm, g_sc_wt.key, "still waited for (state 7)");
    if (seq == 8 && act == 1 && now - g_sc_wt.vw_t >= 1000u && g_sc_wt.vw_n < 10)   /* the scene runs: what this PC's viewport shows (probe of the guest's framing) */
        g_sc_wt.vw_n++, g_sc_wt.vw_t = now, sc_view_log(g_sc_wt.key, now - g_sc_wt.t0);
    if (now - g_sc_wt.t0 > 60000u || (act == 2 && (seq == 0 || seq >= 0x10) && now - g_sc_wt.t0 > 3000u)) {
        r2_log(W_LOG "scene: watch %s (%s) ends after %u ms (%d lines)", g_sc_wt.key, g_sc_wt.how, now - g_sc_wt.t0, g_sc_wt.lines);
        g_sc_wt.sm = 0;
    }
}
/* every in-engine scene that starts here: the camera frame of this PC's own director 1, 3 and 5 s after its start (both PCs log the same moments of their own start; the
 * guest's camera clock must advance as the host's), with LAB `net scene shots 1` also this PC's own back-buffer shot at those moments */
static struct { char key[SC_KEY_MAX]; uint32_t t0; int n; } g_sc_tl;
static int g_sc_shots;
static void sc_tl_frame(uint32_t now)
{
    static const unsigned at[3] = {1000, 3000, 5000};
    if (!g_sc_tl.key[0] || g_sc_tl.n >= 3 || now - g_sc_tl.t0 < at[g_sc_tl.n]) return;
    unsigned d = sc_find_dir(g_sc_tl.key), sch = d ? rd32(d + 0x41E0) : 0, sm = sc_find_sm(g_sc_tl.key);
    r2_log(W_LOG "scene: %s %u s after its start here: camera frame %.1f (director %08x, its scheduler %08x held %d bits %03x), set model action %d, sequence state %d%s", g_sc_tl.key,
           at[g_sc_tl.n] / 1000, sch ? rdf(sch + 0x34) : -1.0f, d, sch, sch ? (int)(rd32(sch + 0x40) & 0xFF) : -1, sch ? rd32(sch + 0xC) & 0xC00 : 0, sm ? (int)rd32(sm + 0x1A54) : -9,
           sm ? (int)rd32(sm + 0x43E8) : -9, g_sc_shots ? "; LAB shot requested" : "");
    if (g_sc_shots) d3d_request_shot();
    g_sc_tl.n++;
}

/* ---------------------------------------------------------------- the trigger of a scene set model (TRIG) */
/* FUN_00773F40 (cdecl: set model, player slot, 0, 0, 0): the stock trigger of a uSmPlayerDemo, the callback of its press prompt (the Wait action's FUN_0040FCD0) and the
 * call of vf122 for zone / flag scenes (local humans only): +0x37B8 |= 4, +0x43EC = 1, +0x43F0 = player, area +0x5D0 = 1. The set model's sequence FUN_00778690 waits for
 * it in state 3 and runs from there: fade, HUD, scheduler frame 0, +0x45AD (state 6), the Wait -> Move change, the Move action's FUN_007781D0 + FUN_00773EA0 (directors),
 * the end. A trigger here goes to the partner (TRIG: player slot, key); the partner's TRIG triggers this PC's own set model of that key (scene_logic.h sc_trig_decide) */
typedef void(__attribute__((cdecl)) *trig_t)(unsigned sm, unsigned player, unsigned a3, unsigned a4, unsigned a5);
static trig_t o_trig;
static int g_sc_trig_in, g_sc_trig_on = 1;   /* this file's own call (not sent back); LAB `net scene trig 0|1` */
static int g_sc_lab_trigdelay;   /* LAB `net scene trigdelay <ms>` (0 = off, the default): the partner's TRIG waits that long here before the pending path applies it (forces the order
                                   in which the partner's scene ends first and its GRANT comes while this PC's own scene still plays; 0.3.2 GRANT hold proof) */
static unsigned g_sc_trig_tx, g_sc_trig_rx, g_sc_trig_res[SC_TRIG_N], g_sc_trig_refused, g_sc_rx_sched;
static const char *const sc_trig_names[SC_TRIG_N] = {"triggered here through the stock trigger", "no scene set model of that key here", "this PC's own one was triggered already",
                                                     "the first scene after the load starts from this PC's own story", "no player object of that slot here",
                                                     "its sequence does not wait for a trigger", "it waits for this PC's own sequence"};
static void __attribute__((cdecl)) h_trig(unsigned sm, unsigned player, unsigned a3, unsigned a4, unsigned a5)
{
    unsigned before = sm ? rd32(sm + 0x43EC) & 0xFF : 1;
    o_trig(sm, player, a3, a4, a5);
    if (!g_sc_on || g_sc_trig_in || !sm || before || !(rd32(sm + 0x43EC) & 0xFF)) return;   /* not newly triggered (refused, or a second call) */
    layout_t l;
    char key[SC_KEY_MAX] = "", st[300];
    if (layout_of(sm, &l) && l.a > 0 && l.a <= 0x7FFF) snprintf(key, SC_KEY_MAX, "S%d,%u,%u", l.a, l.b, l.c);
    sc_watch_begin(sm, key, "triggered here");
    int tx = key[0] && g_sc_trig_on && sc_coop();
    if (tx) sc_send_key(SCO_TRIG, (int)(player & 0xFF), key), g_sc_trig_tx++;
    sc_sm_state(sm, st, sizeof st);
    r2_log(W_LOG "scene: scene set model %08x (%s) triggered here by player %u%s: %s", sm, key[0] ? key : "no key", player, tx ? ", sent to the partner (TRIG)" : "", st);
}
/* the units of a scene set model (+0x436C, two levels, as FUN_00773EA0 walks them): class, action, +0x4170 (1 = the unit releases the scheduler in its run), the readiness
 * bytes +0x4174..+0x4177 (prepared, -, ready = the sequence's state 7 waits for all, go) and +0x4208 / +0x4209 (a character controller: its interpolation to the start pose
 * is done / pending) */
static void sc_units_log(unsigned sm, const char *key, const char *what)
{
    char line[900];
    int o = 0, n = 0;
    unsigned l1 = rd32(sm + 0x436C), n1 = l1 ? rd32(l1 + 4) : 0, v1 = l1 ? rd32(l1 + 0x10) : 0;
    for (unsigned j = 0; j < n1 && j < 16 && v1 && o < (int)sizeof line - 160; j++) {
        unsigned l2 = rd32(v1 + 4 * j), n2 = l2 ? rd32(l2 + 4) : 0, v2 = l2 ? rd32(l2 + 0x10) : 0;
        for (unsigned q = 0; q < n2 && q < 64 && v2 && o < (int)sizeof line - 160; q++) {
            unsigned u = rd32(v2 + 4 * q);
            if (!u) continue;
            char cn[100];
            unsigned r4 = rd32(u + 0x4174), r8 = rd32(u + 0x4208);
            o += snprintf(line + o, sizeof line - (size_t)o, "%s[%s act %d 4170=%d rdy %u%u%u%u 4208=%u 4209=%u", n++ ? " " : "", w_class(u, cn, sizeof cn), (int)rd32(u + 0x1A54),
                          (int)rd32(u + 0x4170), r4 & 0xFF, (r4 >> 8) & 0xFF, (r4 >> 16) & 0xFF, r4 >> 24, r8 & 0xFF, (r8 >> 8) & 0xFF);
            if (!strncmp(cn, "uDemoControllerEm", 17) && o < (int)sizeof line - 40)   /* 0.3.2: the enemy it names (the actor rule) */
                o += snprintf(line + o, sizeof line - (size_t)o, " control %d names (%d,%d,%d)", (int)rd32(u + 0x42B0), (int)rd32(u + 0x42BC), (int)rd32(u + 0x42C0), (int)rd32(u + 0x42C4));
            if (o < (int)sizeof line - 2) line[o++] = ']', line[o] = 0;
        }
    }
    r2_log(W_LOG "scene: units of %s %s (%d): %s", key, what, n, n ? line : "none");
}

static struct { char key[SC_KEY_MAX]; int slot, scene_seen; uint32_t t; unsigned sm; } g_sc_tp;   /* a TRIG that waits for this PC's own sequence (state 3) */
static unsigned g_sc_tp_res[4];
static int g_sc_actor_on = 1;
static unsigned g_sc_actor_allowed;

/* the partner's trigger applied here: the stock trigger of this PC's own set model with the slot of the partner's puppet (local objects only); 1 = it took */
static int sc_trig_apply(unsigned sm, const char *key, int slot, const char *how)
{
    char st[300];
    g_sc_trig_in = 1;
    o_trig(sm, (unsigned)slot, 0, 0, 0);
    g_sc_trig_in = 0;
    sc_sm_state(sm, st, sizeof st);
    if (!(rd32(sm + 0x43EC) & 0xFF)) {
        unsigned ag = game_area();
        g_sc_trig_refused++;
        r2_log(W_LOG "scene: the partner's trigger of %s (player %d): the stock trigger refused it here (area +0x5D0 %u +0x5D2 %u +0x6DC %u; or a human in a door / co-op event): "
                     "the partner's SCENE starts it: %s", key, slot, ag ? rd32(ag + 0x5D0) & 0xFF : 0, ag ? rd32(ag + 0x5D2) & 0xFF : 0, ag ? rd32(ag + 0x6DC) : 0, st);
        return 0;
    }
    g_sc_trig_res[SC_TRIG_APPLY]++;
    sc_watch_begin(sm, key, "the partner's TRIG");
    r2_log(W_LOG "scene: the partner's trigger of %s (player %d, puppet %08x): %s: %s", key, slot, player_obj(slot), how, st);
    return 1;
}

/* this PC's own set model of a scene starts its directors as its Move action does (FUN_007781D0, then FUN_00773EA0) with the partner's puppet as the player: the
 * received SCENE when no trigger took here (fix C), or a waiting TRIG that expired after the SCENE came */
static void sc_local_start(unsigned sm, const char *key, int pl, const char *why)
{
    unsigned po = (pl == 0 || pl == 1) ? player_obj(pl) : 0;
    if (!po) {   /* never a start without the partner's puppet here */
        r2_log(W_LOG "scene: %s: %s not started here: no puppet of the partner here (slot %d)", why, key, pl);
        return;
    }
    g_sc_ls.sm = sm, sc_key_set(g_sc_ls.key, key), g_sc_ls.key[0] = key[0], g_sc_ls.t = sc_now(), g_sc_ls.probes = 1, g_sc_ls.player = (unsigned)pl, g_sc_ls_starts++;
    int sched = sc_local_start_runs_scheduler();
    r2_log(W_LOG "scene: %s: this PC's own scene set model %08x of %s starts its directors as its Move action does (%sFUN_00773EA0, player %d = the partner's puppet %08x)", why,
           sm, key, sched ? "FUN_007781D0, then " : "", pl, po);
    if (sched) {
        unsigned a[1] = {sm};
        do_call(2, 0x7781D0, a, 1), g_sc_rx_sched++;   /* fastcall set model: its scheduler +0xC |= 0x800 | 0x400 (the Move action's state 2 does it before FUN_00773EA0) */
    }
    o_smstart((void *)sm, (unsigned)pl);
    sc_watch_begin(sm, key, "started here by the mod");
    sc_ls_log("started");
}

/* the partner's TRIG */
static void scene_trig_rx(const char *key, int slot)
{
    g_sc_trig_rx++;
    if (!g_sc_trig_on || !o_trig) return;
    uint32_t now = sc_now();
    unsigned sm = sc_find_sm(key);
    int triggered = sm && ((rd32(sm + 0x43EC) & 0xFF) || (rd32(sm + 0x37B8) & 4));
    int win = g_sc_gate.armed && now - g_sc_gate_arm_t < SC_GATE_WINDOW_MS;
    unsigned po = slot >= 0 && slot < 8 ? player_obj(slot) : 0;
    int seq = sm ? (int)rd32(sm + 0x43E8) : -1;
    int r = sc_trig_decide(sc_coop(), sm != 0, triggered, win, po != 0, seq);
    if (g_sc_lab_trigdelay > 0 && (r == SC_TRIG_APPLY || r == SC_TRIG_WAIT)) {   /* LAB: the pending path applies it after the delay (once the sequence waits for a trigger) */
        memset(&g_sc_tp, 0, sizeof g_sc_tp);
        sc_key_set(g_sc_tp.key, key), g_sc_tp.key[0] = key[0], g_sc_tp.slot = slot, g_sc_tp.t = now + (uint32_t)g_sc_lab_trigdelay, g_sc_tp.sm = sm;
        r2_log(W_LOG "scene: LAB the partner's trigger of %s (player %d) waits %d ms here before it is applied (`net scene trigdelay`)", key, slot, g_sc_lab_trigdelay);
        return;
    }
    if (r == SC_TRIG_APPLY) {
        sc_trig_apply(sm, key, slot, sc_trig_names[SC_TRIG_APPLY]);
        return;
    }
    char st[300] = "";
    if (sm) sc_sm_state(sm, st, sizeof st);
    g_sc_trig_res[r]++;
    if (r == SC_TRIG_WAIT) {
        memset(&g_sc_tp, 0, sizeof g_sc_tp);
        sc_key_set(g_sc_tp.key, key), g_sc_tp.key[0] = key[0], g_sc_tp.slot = slot, g_sc_tp.t = now, g_sc_tp.sm = sm;
        r2_log(W_LOG "scene: the partner's trigger of %s (player %d, puppet %08x): this PC's own sequence still prepares: the TRIG waits for its state 3 (at most %u ms), the "
                     "partner's SCENE stays out meanwhile: %s", key, slot, po, SC_TRIG_WAIT_MS, st);
        return;
    }
    r2_log(W_LOG "scene: the partner's trigger of %s (player %d, puppet %08x): %s%s%s", key, slot, po, sc_trig_names[r], st[0] ? ": " : "", st);
}
/* a waiting TRIG, every frame (scene_logic.h sc_trig_pending_step) */
static void sc_tp_frame(uint32_t now)
{
    if (!g_sc_tp.key[0]) return;
    if ((int32_t)(now - g_sc_tp.t) < 0) return;   /* LAB `net scene trigdelay`: not yet */
    unsigned sm = g_sc_tp.sm;
    char key[SC_KEY_MAX];
    sc_key_set(key, g_sc_tp.key), key[0] = g_sc_tp.key[0];
    int slot = g_sc_tp.slot, seen = g_sc_tp.scene_seen;
    uint32_t el = now - g_sc_tp.t;
    if (!sm || !sc_sm_listed(sm)) {
        r2_log(W_LOG "scene: the waiting TRIG of %s ends after %u ms: its set model is gone", key, el);
        memset(&g_sc_tp, 0, sizeof g_sc_tp);
        return;
    }
    int triggered = (rd32(sm + 0x43EC) & 0xFF) || (rd32(sm + 0x37B8) & 4);
    int seq = (int)rd32(sm + 0x43E8);
    int r = sc_trig_pending_step(triggered, seq, el);
    if (r == SC_TP_WAIT) return;
    g_sc_tp_res[r]++;
    memset(&g_sc_tp, 0, sizeof g_sc_tp);
    if (r == SC_TP_APPLY) {
        char how[120];
        snprintf(how, sizeof how, "triggered here through the stock trigger %u ms after the TRIG (its sequence reached state 3)", el);
        if (!sc_trig_apply(sm, key, slot, how) && seen) sc_local_start(sm, key, slot, "the stock trigger refused the waiting TRIG and the partner's SCENE came");
        return;
    }
    if (r == SC_TP_DROP) {
        r2_log(W_LOG "scene: the waiting TRIG of %s ends after %u ms: this PC's own story triggered it meanwhile", key, el);
        return;
    }
    r2_log(W_LOG "scene: the waiting TRIG of %s expired after %u ms (its sequence stayed in state %d)%s", key, el, seq, seen ? ": the partner's SCENE came, the scene starts here as before" : "");
    if (seen) sc_local_start(sm, key, slot, "the waiting TRIG expired");
}

/* the scene actors this PC created (scene_actor_allowed): the host's record / SPAWN that first finds one by its layout id is logged (live evidence: no second enemy) */
static struct { unsigned obj; int a; unsigned bc, adopted; } g_sc_act[8];
static unsigned g_sc_act_i, g_sc_act_adopted;
static void scene_actor_created(unsigned obj, int a, unsigned bc)
{
    unsigned i = g_sc_act_i++ & 7;
    g_sc_act[i].obj = obj, g_sc_act[i].a = a, g_sc_act[i].bc = bc, g_sc_act[i].adopted = 0;
    char cn[100];
    r2_log(W_LOG "scene: the scene actor (%d,%u,%u) is %08x %s on this PC", a, bc & 0xFFFF, bc >> 16, obj, w_class(obj, cn, sizeof cn));
}
static void scene_actor_adopt(unsigned en, const char *how)   /* world.inc: a host record / SPAWN found this enemy by its layout id */
{
    for (int i = 0; i < 8; i++)
        if (en && g_sc_act[i].obj == en && !g_sc_act[i].adopted) {
            g_sc_act[i].adopted = 1, g_sc_act_adopted++;
            r2_log(W_LOG "scene: the host's %s now drives the scene actor (%d,%u,%u) %08x that this PC created (found by its layout id, no second enemy): hp %d act %d at (%.0f,%.0f,%.0f)",
                   how, g_sc_act[i].a, g_sc_act[i].bc & 0xFFFF, g_sc_act[i].bc >> 16, en, (int)rd32(en + 0x1A08), (int)rd32(en + 0x1A54), rdf(en + 0x40), rdf(en + 0x44), rdf(en + 0x48));
        }
}

/* coop3.c view_guard / coop_game.inc setup_roles_step: an in-engine scene plays here, the view belongs to it (scene_logic.h sc_view_owned) */
static int g_sc_view_on = 1;
static unsigned g_sc_view_yields;
static int scene_plays_here(void)   /* coop_game.inc setup_roles_step: a type-3 scene plays on this PC (live sc032n: the host's own role set-up in the middle of its
                                       WakeUp put its viewport on its human's gameplay camera 2.3 s into the scene) */
{
    if (!g_sc_on || !g_sc_view_on || !rd32(0x1581788)) return 0;
    return sc_view_owned(1, sc_playing(0xFFFFFFFFu));
}
static int scene_owns_view(void)
{
    unsigned gm = rd32(0x157AE00);
    if (!g_sc_on || !rd32(0x1581788) || !gm || g_my_slot < 0 || (int)rd32(gm + 0x8F8) == g_my_slot) return 0;   /* the view shows this PC's own human: nothing to leave alone */
    int r = sc_view_owned(g_sc_view_on, sc_playing(0xFFFFFFFFu));
    if (r) g_sc_view_yields++;
    return r;
}

/* the target layouts the enemy units of a scene set model name (+0x436C, two levels, as sc_units_log walks them): a unit whose class is uDemoControllerEm* and whose control
 * (+0x42B0) is 3 (Control::Enemy, asserted by its target getter vf90 0x76A460) names the LayoutID +0x42BC / +0x42C0 / +0x42C4; returns how many (at most `cap`) */
static int sc_enemy_targets(unsigned sm, sc_lay *t, int cap)
{
    int n = 0;
    unsigned l1 = rd32(sm + 0x436C), n1 = l1 ? rd32(l1 + 4) : 0, v1 = l1 ? rd32(l1 + 0x10) : 0;
    for (unsigned j = 0; j < n1 && j < 16 && v1; j++) {
        unsigned l2 = rd32(v1 + 4 * j), n2 = l2 ? rd32(l2 + 4) : 0, v2 = l2 ? rd32(l2 + 0x10) : 0;
        for (unsigned q = 0; q < n2 && q < 64 && v2 && n < cap; q++) {
            unsigned u = rd32(v2 + 4 * q);
            if (!u) continue;
            char cn[100];
            w_class(u, cn, sizeof cn);
            if (strncmp(cn, "uDemoControllerEm", 17) || rd32(u + 0x42B0) != 3) continue;
            t[n].a = (int)rd32(u + 0x42BC), t[n].b = (int)rd32(u + 0x42C0), t[n].c = (int)rd32(u + 0x42C4), n++;
        }
    }
    return n;
}
static unsigned g_sc_actor_refused;
/* world.inc h_create, on a client whose guard would refuse the creation: the actor of this PC's own in-engine scene (scene_logic.h sc_actor_allow): a layout one of the
 * watched scene set model's enemy units names, while its sequence waits for its units (state 7) */
static int scene_actor_allowed(int a, unsigned bc)
{
    if (!g_sc_on || !g_sc_actor_on) return 0;
    unsigned sm = g_sc_wt.sm;
    int listed = sm && sc_sm_listed(sm);
    int seq = listed ? (int)rd32(sm + 0x43E8) : -1;
    sc_lay tg[8];
    int nt = listed ? sc_enemy_targets(sm, tg, 8) : 0;
    int named = sc_lay_named(tg, nt, a, bc);
    int playing = sc_playing(0xFFFFFFFFu);
    if (!sc_actor_allow(sc_coop(), seq, playing, named)) {
        if ((seq >= 4 && seq <= 8) || playing) {   /* the first form of the rule let this one through: say why not (live r032fa: the snake (600,30,0) in state 8) */
            g_sc_actor_refused++;
            if (g_sc_actor_refused <= 40) {
                char nm[120] = "none";
                int o = 0;
                for (int i = 0; i < nt && o < (int)sizeof nm - 24; i++) o += snprintf(nm + o, sizeof nm - (size_t)o, "%s(%d,%d,%d)", i ? " " : "", tg[i].a, tg[i].b, tg[i].c);
                r2_log(W_LOG "scene: CreateEnemy(layout (%d,%u,%u)) while %s%s (sequence state %d): not this scene's actor (its enemy units name %s): left to the client guard "
                             "(the host's SPAWN creates it)", a, bc & 0xFFFF, bc >> 16, listed ? "this PC's scene " : "a scene plays here", listed ? g_sc_wt.key : "", seq, nm);
            }
        }
        return 0;
    }
    g_sc_actor_allowed++;
    if (g_sc_actor_allowed <= 40)
        r2_log(W_LOG "scene: CreateEnemy(layout (%d,%u,%u)) asked while this PC's scene %s waits for its units (sequence state %d): an enemy unit of the scene names it: the scene's "
                     "actor is created on this PC (the host's record / SPAWN finds it by its layout id)", a, bc & 0xFFFF, bc >> 16, g_sc_wt.key, seq);
    return 1;
}

/* a received SCENE: this PC's own scene set model of that key starts its directors (scene_logic.h sc_rx_decide); 1 = the received bytes are not submitted */
static int scene_rx_takes(unsigned id)
{
    if (!g_sc_on || !o_smstart) return 0;
    const char *key = g_sc_skey[id & 0xFF];
    uint32_t now = sc_now();
    unsigned sm = sc_find_sm(key);
    int started = sm && (rd32(sm + 0x43EC) & 0xFF);
    int win = g_sc_gate.armed && now - g_sc_gate_arm_t < SC_GATE_WINDOW_MS;
    int pend = g_sc_tp.key[0] && sc_key_eq(g_sc_tp.key, key) && sm == g_sc_tp.sm;
    int r = sc_rx_decide(sc_coop(), sm != 0, started, win, pend);
    if (r == SC_RX_APPLY) return 0;
    g_sc_dd_skip++;
    if (r == SC_RX_KEEP_OUT) {
        if (pend) g_sc_tp.scene_seen = 1;
        r2_log(W_LOG "scene: the partner's SCENE %u (%s) not submitted: %s", id & 0xFF, key,
               started ? "this PC's own scene set model runs it already" : pend ? "the partner's TRIG waits here for this PC's own sequence" : "the first scene after the load starts from this PC's own story");
        return 1;
    }
    char why[80];
    snprintf(why, sizeof why, "the partner's SCENE %u not submitted", id & 0xFF);
    sc_local_start(sm, key, g_rem_slot, why);
    return 1;
}
static void scene_partner_scene_applied(unsigned id, unsigned r)
{
    if (!g_sc_on || r == 0xFFFFFFFFu) return;
    const char *key = g_sc_skey[id & 0xFF];
    uint32_t now = sc_now();
    sc_dd_note(&g_sc_dd, key, SC_FROM_PARTNER, r, now);
    g_sc_ap.serial = r, sc_key_set(g_sc_ap.key, key), g_sc_ap.key[0] = key[0], g_sc_ap.t = now, g_sc_ap.checked = 0;
    sc_gate_disarm(&g_sc_gate);   /* the first scene after the load came from the partner */
}

/* what the scene did meanwhile (live check of the first-start hold): the camera director's own motion frame (director +0x41E0 -> +0x34, as cDemoCameraControllerActionBase_vf43
 * reads it), the slot player's action id and position, the owner set model's action */
static void sc_scene_probe(unsigned slot, unsigned player, char *out, int size)
{
    unsigned d = rd32(slot + 0x70), cm = 0, p = player_obj((int)player);
    if (d > 0x10000u + 0x4214u) d -= 0x4214u, cm = rd32(d + 0x41E0);
    unsigned sm = d ? sc_owner_sm(d) : 0;
    snprintf(out, (size_t)size, "camera frame %.1f, player %u action %d at (%.0f,%.0f,%.0f), set model action %d", cm ? rdf(cm + 0x34) : -1.0f, player, p ? (int)rd32(p + 0x1A54) : -1,
             p ? rdf(p + 0x40) : 0.0f, p ? rdf(p + 0x44) : 0.0f, p ? rdf(p + 0x48) : 0.0f, sm ? (int)rd32(sm + 0x1A54) : -9);
}

/* the start of a type-3 scene slot, sDemo FUN_00A9D530 (thiscall sDemo, slot, player; 0 = not ready yet, retried every frame by 0xAA01E0): the PC whose own submit started
 * the scene waits (at most SC_SCENE_HOLD_MS) until the partner's copy started; every start is stamped here (the gap lines measure the real starts) */
typedef unsigned(__attribute__((thiscall)) *slotstart_t)(void *self, unsigned slot, unsigned player);
static slotstart_t o_slotstart;
static __attribute__((thiscall)) unsigned h_slotstart(void *self, unsigned slot, unsigned player)
{
    if (!g_sc_on || !slot) return o_slotstart(self, slot, player);
    unsigned serial = rd32(slot + 4);
    uint32_t now = sc_now();
    int ap = g_sc_ap.key[0] && serial == g_sc_ap.serial && now - g_sc_ap.t < 60000;   /* the partner's copy, submitted here from its SCENE */
    char key[SC_KEY_MAX];
    if (slot == g_sc_kc_slot && serial == g_sc_kc_serial) sc_key_set(key, g_sc_kc_key), key[0] = g_sc_kc_key[0];
    else {
        if (ap) sc_key_set(key, g_sc_ap.key), key[0] = g_sc_ap.key[0];   /* (live sc032c: its bytes hold the sender's director pointer, so +0x70 names nothing here) */
        else {
            g_sc_key_quiet = 1;
            sc_req_key(2, (void *)slot, key);   /* the slot holds the request's copy, +0x70 included (FUN_00A9CEB0) */
            g_sc_key_quiet = 0;
        }
        g_sc_kc_slot = slot, g_sc_kc_serial = serial, sc_key_set(g_sc_kc_key, key), g_sc_kc_key[0] = key[0];
    }
    if (key[0]) {
        int i = sc_dd_find(&g_sc_dd, key, SC_FROM_OWN, now);
        int own_recent = i >= 0 && g_sc_dd.e[i].serial == serial && now - g_sc_dd.e[i].t < 3000;
        int pstarted = i >= 0 && sc_key_eq(g_sc_pstart.key, key) && (int32_t)(g_sc_pstart.t - g_sc_dd.e[i].t) >= -1000;
        if (serial != g_sc_hold_serial) g_sc_hold_serial = serial, g_sc_hold_t0 = now, g_sc_hold_n = 0;
        if (g_sc_hold_on && sc_scene_hold(sc_coop(), 1, own_recent, pstarted, now - g_sc_hold_t0)) {
            if (!g_sc_hold_n++) {
                char pr[160];
                sc_scene_probe(slot, player, pr, sizeof pr);
                g_sc_scene_holds++;
                r2_log(W_LOG "scene: start of %s held here until the partner's copy starts (at most %u ms): %s", key, SC_SCENE_HOLD_MS, pr);
            }
            return 0;
        }
        if (g_sc_hold_n) {
            char pr[160];
            sc_scene_probe(slot, player, pr, sizeof pr);
            r2_log(W_LOG "scene: start of %s released after %u ms: %s: %s", key, now - g_sc_hold_t0, pstarted ? "the partner's copy started" : "the 1 s limit", pr);
            g_sc_hold_n = 0;
        }
    }
    unsigned r = o_slotstart(self, slot, player);
    if (r & 0xFF) {   /* every player's slot of a scene (a type-3 request fills the fix slot of each human of its mask): which player's slot drives this PC's viewport */
        static unsigned ps_serial[8], ps_player[8], ps_i, ps_logs;
        int seen = 0;
        for (int k = 0; k < 8; k++) seen |= ps_serial[k] == serial && ps_player[k] == player;
        if (!seen && ps_logs++ < 60) {
            unsigned bc = rd32(0x15DE88C);
            ps_serial[ps_i & 7] = serial, ps_player[ps_i & 7] = player, ps_i++;
            r2_log(W_LOG "scene: sDemo slot %08x of player %u starts (serial %u, %s): viewports follow players %d / %d, request mask %08x +0x90 %02x camera index %d", slot, player, serial,
                   key[0] ? key : "no key", bc ? (int)rd32(bc + 0xCF0) : -9, bc ? (int)rd32(bc + 0xCF4) : -9, rd32(slot + 0x8C), rd32(slot + 0x90) & 0xFF, (int)(signed char)(rd32(slot + 0xBD) & 0xFF));
        }
    }
    if ((r & 0xFF) && key[0]) {
        int done = 0;
        for (int k = 0; k < 8; k++) done |= g_sc_stamped[k] == serial;
        if (!done) {
            g_sc_stamped[g_sc_stamped_i++ & 7] = serial;
            char pr[160];
            sc_scene_probe(slot, player, pr, sizeof pr);
            r2_log(W_LOG "scene: in-engine scene %s starts here (sDemo serial %u%s): %s", key, serial, ap ? ", the partner's copy" : "", pr);
            sc_key_set(g_sc_tl.key, key), g_sc_tl.key[0] = key[0], g_sc_tl.t0 = now, g_sc_tl.n = 0;   /* the camera frame 1, 3, 5 s later */
            if (sc_active()) sc_stamp(SCT_SCENE, key, now, 0);
            if (ap) sc_local_dir_log("starts"), g_sc_ap.probes = 1;
        }
    }
    if (r & 0xFF) sc_gate_disarm(&g_sc_gate);   /* a scene plays here: the first one after the load is past */
    return r;
}

/* what the waiting PC does meanwhile (live evidence for the gate): the director's camera frame, both players' actions and positions, the owner set model's action */
static void sc_dir_probe(unsigned d, char *out, int size)
{
    unsigned cm = rd32(d + 0x41E0), sm = sc_owner_sm(d), p0 = player_obj(0), p1 = player_obj(1);
    snprintf(out, (size_t)size, "camera frame %.1f; player 0 action %d at (%.0f,%.0f,%.0f), player 1 action %d at (%.0f,%.0f,%.0f); set model action %d", cm ? rdf(cm + 0x34) : -1.0f,
             p0 ? (int)rd32(p0 + 0x1A54) : -1, p0 ? rdf(p0 + 0x40) : 0.0f, p0 ? rdf(p0 + 0x44) : 0.0f, p0 ? rdf(p0 + 0x48) : 0.0f,
             p1 ? (int)rd32(p1 + 0x1A54) : -1, p1 ? rdf(p1 + 0x40) : 0.0f, p1 ? rdf(p1 + 0x44) : 0.0f, p1 ? rdf(p1 + 0x48) : 0.0f, sm ? (int)rd32(sm + 0x1A54) : -9);
}

/* the camera director's submit FUN_0075D3A0 (thiscall director; bool): its action FUN_0075CFB0 calls it in state 2 (director +0x4204 == 0) every frame until it returns
 * true (0x75CFF0 / 0x75CFF5; the stock itself returns false while another demo is pending, FUN_00A9E0C0(-1)), and runs the director's per-frame update 0x75D8D0 only
 * after it. The first scene after a load waits here until the partner's story reached it (scene_logic.h sc_gate_decide); the game loop runs on, the strip shows */
typedef unsigned char(__attribute__((thiscall)) *dsub_t)(void *self);
static dsub_t o_dsub;
static const char *const sc_gate_names[SC_GATE_N] = {"wait", "pass", "the partner's story reached it", "the partner's copy started", "no partner", "the partner has no gate",
                                                      "the 15 s limit"};
/* every director's submit attempts, logged when the director or the result changes (at most 60 lines a load): does a director run here at all, and does the stock let it
 * submit (live sc032c / sc032e: the guest showed no submit of the host's knife table scene) */
static unsigned char sc_dsub_logged(void *self)
{
    unsigned d = (unsigned)self;
    unsigned char r = o_dsub(self);
    g_sc_ds_calls++;
    if ((d != g_sc_ds_last_d || (int)r != g_sc_ds_last_r) && g_sc_ds_logs < 60) {
        char key[SC_KEY_MAX], pr[200];
        g_sc_key_quiet = 1;
        sc_dir_key(d, key);
        g_sc_key_quiet = 0;
        sc_dir_probe(d, pr, sizeof pr);
        unsigned sd = rd32(0x1581788);
        g_sc_ds_logs++;
        r2_log(W_LOG "scene: director %08x (%s, +0x4204 %u) asked to submit: %s (sDemo +0x38 %d, +0xCC8 %d): %s", d, key[0] ? key : "no key", rd32(d + 0x4204),
               r ? "submitted" : "the stock said not now", sd ? (int)(rd32(sd + 0x38) & 0xFF) : -1, sd ? (int)(rd32(sd + 0xCC8) & 0xFF) : -1, pr);
    }
    g_sc_ds_last_d = d, g_sc_ds_last_r = r;
    return r;
}
static __attribute__((thiscall)) unsigned char h_dsub(void *self)
{
    unsigned d = (unsigned)self;
    if (!g_sc_on || !g_sc_gate_on || !g_sc_gate.armed || !d || !sc_active() || !sc_coop()) return sc_dsub_logged(self);
    uint32_t now = sc_now();
    if (now - g_sc_gate_arm_t >= SC_GATE_WINDOW_MS && !g_sc_gate.waiting) {   /* not the scene of the load (a story trigger later in the level) */
        sc_gate_disarm(&g_sc_gate);
        return sc_dsub_logged(self);
    }
    char key[SC_KEY_MAX], name[40];
    g_sc_key_quiet = 1;
    sc_dir_key(d, key);
    g_sc_key_quiet = 0;
    int partner = sc_partner(name, sizeof name);
    int started = key[0] && sc_key_eq(g_sc_pstart.key, key) && (int32_t)(g_sc_pstart.t - g_sc_gate_arm_t) >= 0;
    int was = g_sc_gate.waiting;
    int r = sc_gate_decide(&g_sc_gate, key, now, rd32(d + 0x4204) == 0, partner, g_sc_bar.caps > 0 && g_sc_pcaps_ver >= SC_CAPS_VER, started);
    if (r == SC_GATE_PASS) return sc_dsub_logged(self);
    if (r == SC_GATE_WAIT) {
        if (!was) {
            char pr[200];
            sc_dir_probe(d, pr, sizeof pr);
            g_sc_gate_waits++, g_sc_gate_tx_t = 0, g_sc_gate_banner_t = 0, g_sc_gate_shot_done = 0;
            r2_log(W_LOG "scene: the first scene after the load (%s) waits here until %s's story reaches it (at most %u s): %s", key, partner ? name : "the partner", SC_GATE_LIMIT_MS / 1000, pr);
        }
        if (!g_sc_gate_tx_t || now - g_sc_gate_tx_t >= 1000) g_sc_gate_tx_t = now ? now : 1, sc_send_key(SCO_SREADY, 0, key);
        if (!g_sc_gate_banner_t || now - g_sc_gate_banner_t >= 1000) g_sc_gate_banner_t = now ? now : 1, menu_banner(MB_WAIT, name, 2500);
        if (g_sc_gate_shot && !g_sc_gate_shot_done && now - g_sc_gate.t0 >= 1500) {
            char pr[200];
            sc_dir_probe(d, pr, sizeof pr);
            g_sc_gate_shot_done = 1, d3d_request_shot();
            r2_log(W_LOG "scene: LAB gate shot requested %u ms into the wait: %s", now - g_sc_gate.t0, pr);
        }
        return 0;   /* the stock "not now": state 2 again next frame */
    }
    /* released: tell the partner (it may still wait for this PC), then the stock submit */
    sc_send_key(SCO_SREADY, 0, key);
    char pr[200];
    sc_dir_probe(d, pr, sizeof pr);
    g_sc_gate_rel[r]++;
    r2_log(W_LOG "scene: the first scene after the load (%s) %s after %u ms: %s: %s", key, was ? "released" : "passes", was ? now - g_sc_gate.t0 : 0, sc_gate_names[r], pr);
    if (g_sc_gate_banner_t) menu_banner(MB_NONE, NULL, 0), g_sc_gate_banner_t = 0;
    sc_gate_disarm(&g_sc_gate);
    return sc_dsub_logged(self);
}

/* door.inc: the partner's action for a set model; 1 = not applied (a scene set model only moves forward) */
static int scene_door_veto(unsigned o, int local, int asked, int a, unsigned b, unsigned c)
{
    if (!g_sc_on || asked == local) return 0;   /* the same action: door.inc's own "same" rule */
    static char big[4096];
    classes_name(o, big, sizeof big - 1);
    big[sizeof big - 1] = 0;
    const char *p = strstr(big, ": ");
    p = p ? p + 2 : big;
    int pd = os_has_class(p, "uSmPlayerDemo"), is = pd || os_has_class(p, "uSmOtherDemo");
    int trig = !pd || (rd32(o + 0x43EC) & 0xFF) || (rd32(o + 0x37B8) & 4);   /* (a uSmOtherDemo has no stock trigger: as before) */
    if (sc_demo_sm_apply(is, local, asked, trig)) return 0;
    if (g_sc_door_veto++ < 40)
        r2_log(W_LOG "scene: DOOR (%d,%u,%u) scene set model action %d -> %d not applied: %s", a, b, c, local, asked,
               asked > local ? "this PC's own set model is not triggered yet (its own sequence makes the Wait -> Move step with the trigger's player)"
                             : "it only moves forward (a step back restarted a playing scene)");
    return 1;
}

/* ---------------------------------------------------------------- events, per frame, level reset */
static void scene_on_event(const cp_event *e)
{
    if (!g_sc_on || e->len < 1) return;
    const unsigned char *b = e->body;
    uint32_t now = sc_now();
    char key[SC_KEY_MAX];
    switch (b[0]) {
    case SCO_CAPS:
        if (e->len >= 2) {
            int had = g_sc_bar.caps;
            sc_bar_on_caps(&g_sc_bar, b[1] != 0);
            g_sc_pcaps_ver = e->len >= 3 ? b[2] : 1;
            if (!had) sc_send_caps();
            if (!g_sc_caps_seen || had != g_sc_bar.caps) r2_log(W_LOG "scene: the partner's scene sync is %s", b[1] ? "on" : "OFF"), g_sc_caps_seen = 1;
        }
        break;
    case SCO_READY:
        if (e->len >= 3) {
            sc_get_key(e, 2, key);
            sc_bar_on_ready(&g_sc_bar, b[1], key, now);
            unsigned ag = game_area();
            int in_play = game_in_episode() && ag && (rd32(ag + 4) & 0xFF) == 3;   /* the game's own state: a PC that did not load waits in G_WAITGO while its game plays */
            if (sc_should_answer_inplay(in_play, in_play && g_sc_inplay_since ? now - g_sc_inplay_since : 0, g_sc_bar.holding)) {
                char mine[SC_KEY_MAX];
                snprintf(mine, sizeof mine, "a%u", story_area_id());
                sc_send_key(SCO_INPLAY, b[1], mine);
            }
            static unsigned ready_logs;
            if (ready_logs++ < 200) r2_log(W_LOG "scene: the partner reached the %s point (%s)%s", b[1] == SC_KIND_LOAD ? "loader end" : "movie start", key, g_sc_bar.holding ? " (holding here too)" : "");
        }
        break;
    case SCO_INPLAY:
        if (e->len >= 3) sc_get_key(e, 2, key), sc_bar_on_inplay(&g_sc_bar, key, now);
        break;
    case SCO_LOADING:
        sc_bar_on_partner_loading(&g_sc_bar, now);
        g_sc_gate.phave = 0;   /* the partner's SREADY belonged to its old level */
        break;
    case SCO_SREADY:   /* the partner's story reached the first scene after its load (re-sent every second while it waits there) */
        if (e->len >= 3) {
            sc_get_key(e, 2, key);
            int fresh = !g_sc_gate.phave || !sc_key_eq(g_sc_gate.pkey, key);
            sc_gate_on_partner(&g_sc_gate, key, now);
            if (fresh && g_sc_gate_logged++ < 40) r2_log(W_LOG "scene: the partner's story reached the first scene after its load (%s)%s", key, g_sc_gate.waiting ? " (waiting here too)" : "");
        }
        break;
    case SCO_VOTE:
        if (e->len >= 3 && b[1] >= 1 && b[1] <= 3) {
            sc_get_key(e, 2, key);
            sc_vote_theirs(&g_sc_vote[b[1]], key, now);
            g_sc_votes_rx++;
            r2_log(W_LOG "scene: the partner pressed skip (%s %s)%s", scv_names[b[1]], key, g_sc_vote[b[1]].active && sc_key_eq(g_sc_vote[b[1]].key, key) ? "" : " (for a scene that has not begun here: kept 15 s)");
        }
        break;
    case SCO_SKEY:
        if (e->len >= 3) sc_get_key(e, 2, key), sc_key_set(g_sc_skey[b[1]], key);
        break;
    case SCO_TRIG:   /* the partner's scene set model was triggered (player slot, key) */
        if (e->len >= 3) sc_get_key(e, 2, key), scene_trig_rx(key, b[1]);
        break;
    case SCO_PING:
        if (e->len >= 5) {
            unsigned char r[9];
            r[0] = SCO_PONG, memcpy(r + 1, b + 1, 4), memcpy(r + 5, &now, 4);
            sc_send(r, 9);
        }
        break;
    case SCO_PONG:
        if (e->len >= 9) {
            uint32_t t1, t2, old = g_sc_clk.rtt;
            int had = g_sc_clk.have;
            memcpy(&t1, b + 1, 4), memcpy(&t2, b + 5, 4);
            sc_clock_sample(&g_sc_clk, t1, t2, now);
            if (!had || (g_sc_clk.rtt < old && g_sc_clk.samples < 300)) r2_log(W_LOG "scene: clock of the partner: offset %d ms, round trip %u ms (%u samples)", (int)g_sc_clk.offset, g_sc_clk.rtt, g_sc_clk.samples);
        }
        break;
    case SCO_STAMP:
        if (e->len >= 11 && b[1] < SCT_N) {
            int w = b[1];
            memcpy(&g_sc_st_theirs[w].t, b + 2, 4), memcpy(&g_sc_st_theirs[w].extra, b + 6, 4);
            sc_get_key(e, 10, g_sc_st_theirs[w].key);
            g_sc_st_theirs[w].have = 1;
            if (w == SCT_SCENE) sc_key_set(g_sc_pstart.key, g_sc_st_theirs[w].key), g_sc_pstart.t = now;
            sc_gap_try(w);
        }
        break;
    default:
        break;
    }
}

static void scene_frame(void)
{
    if (!g_sc_on || !g_cs || !cs_active(g_cs)) return;
    uint32_t now = sc_now();
    if (g_state >= G_SETUP) {
        if (!g_sc_caps_t || now - g_sc_caps_t >= 10000) g_sc_caps_t = now ? now : 1, sc_send_caps();
        if (!g_sc_ping_t || now - g_sc_ping_t >= 2000) {
            unsigned char b[5];
            g_sc_ping_t = now ? now : 1;
            b[0] = SCO_PING, memcpy(b + 1, &now, 4);
            sc_send(b, 5);
        }
    }
    unsigned ag = game_area();
    int in_play = g_state >= G_SETUP && game_in_episode() && ag && (rd32(ag + 4) & 0xFF) == 3;   /* (not G_RUN only: see the READY answer) */
    if (!in_play) g_sc_inplay_since = 0;
    else if (!g_sc_inplay_since) g_sc_inplay_since = now ? now : 1;
    /* the movie: its end (stamp with the frame), the vote's end */
    unsigned cs = rd32(0x1579134), st = cs ? rd32(cs + 0xDC) : 0, mv = cs ? rd32(cs + 0x48) : 0;
    if (st >= 3 && st <= 6 && mv) g_sc_movie_frame = rdf(mv + 0x34);
    if (g_sc_movie_state >= 3 && g_sc_movie_state <= 6 && (st == 7 || st == 8 || st == 0)) {
        if (g_sc_cur_movie[0] && sc_active()) {
            sc_stamp(SCT_MOVIE_END, g_sc_cur_movie, now, (int)g_sc_movie_frame);
            r2_log(W_LOG "scene: movie %s ended here at frame %d%s", g_sc_cur_movie, (int)g_sc_movie_frame, g_sc_vote[SCV_MOVIE].mine ? " (skip voted here)" : "");
        }
        sc_vote_end(&g_sc_vote[SCV_MOVIE]), g_sc_cur_movie[0] = 0;
    }
    g_sc_movie_state = (int)st;
    /* the mini demo (sDemo +0x38) and the player demos (a type-3 slot in state 1 / 2): the votes end with them */
    unsigned sd = rd32(0x1581788), ms = sd ? rd32(sd + 0x38) & 0xFF : 0;
    if (g_sc_mini_state == 2 && ms != 2) sc_vote_end(&g_sc_vote[SCV_MINI]), g_sc_mini_applied = 0;
    g_sc_mini_state = (int)ms;
    if (g_sc_vote[SCV_MINI].active && g_sc_vote[SCV_MINI].mine && g_sc_vote[SCV_MINI].theirs && !g_sc_mini_applied && sc_coop()) {
        unsigned net = rd32(0x15DE668);
        if (net) g_sc_mini_applied = 1, g_sc_skipped++, r2_log(W_LOG "scene: both players skipped the mini demo: the stock vote bit is set"), o_netvote(net);
    }
    if (g_sc_vote[SCV_PDEMO].active && sd && !(rd32(sd + 0x195F) & 0xFF) && !sc_playing(0xFFFFFFFFu)) sc_vote_end(&g_sc_vote[SCV_PDEMO]);
    sc_watch_frame(now);
    sc_tl_frame(now);
    sc_tp_frame(now);
    if (g_sc_el.t && now - g_sc_el.t >= 30000u) sc_elist_auto(g_sc_el.key, "+30s"), g_sc_el.t = 0;
    /* the scene this PC started from a received SCENE: its own director 1.5 s and 4 s later (camera frame > 0, a submit through 0x75D3A0) */
    if (g_sc_ls.sm && g_sc_ls.probes == 1 && now - g_sc_ls.t >= 1500) sc_ls_log("1.5 s later"), g_sc_ls.probes = 2;
    if (g_sc_ls.sm && g_sc_ls.probes == 2 && now - g_sc_ls.t >= 4000) sc_ls_log("4 s later"), g_sc_ls.probes = 3;
    /* the partner's copy: this PC's own director of that scene 1.5 s and 4 s after it was submitted here (does the scene run here) */
    if (g_sc_ap.key[0] && g_sc_ap.probes == 1 && now - g_sc_ap.t >= 1500) sc_local_dir_log("1.5 s later"), g_sc_ap.probes = 2;
    if (g_sc_ap.key[0] && g_sc_ap.probes == 2 && now - g_sc_ap.t >= 4000) sc_local_dir_log("4 s later"), g_sc_ap.probes = 3;
    /* the partner's copy submitted here: say once when its slot did not start within 5 s (live sc032c: the knife table scene applied on the guest logged no start) */
    if (g_sc_ap.key[0] && !g_sc_ap.checked && now - g_sc_ap.t >= 5000) {
        int done = 0;
        for (int k = 0; k < 8; k++) done |= g_sc_stamped[k] == g_sc_ap.serial;
        if (!done) r2_log(W_LOG "scene: the partner's copy of %s (sDemo serial %u) did not start here within 5 s (its slot: %s)", g_sc_ap.key, g_sc_ap.serial,
                          sc_playing(g_sc_ap.serial) ? "state 1 / 2" : "not in state 1 / 2");
        g_sc_ap.checked = 1;
    }
    /* the strips of the vote */
    if (!g_sc_bar.holding && now - g_sc_strip_t >= 400) {
        g_sc_strip_t = now;
        char name[40];
        int p = sc_partner(name, sizeof name) && g_sc_bar.caps > 0, s = SC_STRIP_NONE;
        for (int c = 1; c <= 3 && s == SC_STRIP_NONE; c++) s = sc_vote_strip(&g_sc_vote[c], p);
        if (s != SC_STRIP_NONE) menu_banner(s == SC_STRIP_WAIT ? MB_SKIP_WAIT : MB_SKIP_PARTNER, name, 1200), g_sc_strip = s;
        else if (g_sc_strip) menu_banner(MB_NONE, NULL, 0), g_sc_strip = 0;
    }
}

/* a new level instance on this PC (the glue's G_LOAD / G_SETUP): its own load began (a hold keeps its time: the glue may set itself up again during it) */
static void scene_level_reset(void)
{
    uint32_t now = sc_now();
    if (!g_sc_bar.holding) sc_bar_load_begin(&g_sc_bar, now);
    for (int c = 1; c <= 3; c++) sc_vote_end(&g_sc_vote[c]);
    g_sc_mini_applied = 0, g_sc_inplay_since = 0;
    /* sDemo numbers its scenes from 1 again after a load (live sc032b: the retry's opening was serial 1 again): no serial of the old level names a slot of the new one */
    memset(g_sc_stamped, 0, sizeof g_sc_stamped), g_sc_kc_slot = 0, g_sc_kc_serial = 0, g_sc_hold_serial = 0, g_sc_hold_n = 0;
    memset(&g_sc_ap, 0, sizeof g_sc_ap);
    g_sc_ds_logs = 0, g_sc_ds_last_d = 0, g_sc_ds_last_r = -1;
    memset(&g_sc_ls, 0, sizeof g_sc_ls);
    memset(&g_sc_wt, 0, sizeof g_sc_wt), memset(&g_sc_tl, 0, sizeof g_sc_tl), memset(&g_sc_tp, 0, sizeof g_sc_tp);   /* (the set models of the old level are gone) */
    if (g_sc_gate_banner_t) menu_banner(MB_NONE, NULL, 0), g_sc_gate_banner_t = 0;
    sc_gate_arm(&g_sc_gate), g_sc_gate_arm_t = now;   /* the level's first scene is gated (at the glue's load and set-up: the window counts from the set-up) */
    if (g_sc_on && g_cs && cs_active(g_cs)) {
        unsigned char b[1] = {SCO_LOADING};
        sc_send(b, 1);
    }
}

/* the session started (CS_N_START): nothing of an earlier session counts */
static void scene_session_start(void)
{
    sc_bar_reset(&g_sc_bar, sc_now());
    memset(g_sc_vote, 0, sizeof g_sc_vote);
    memset(&g_sc_dd, 0, sizeof g_sc_dd);
    memset(g_sc_st_mine, 0, sizeof g_sc_st_mine), memset(g_sc_st_theirs, 0, sizeof g_sc_st_theirs);
    memset(g_sc_skey, 0, sizeof g_sc_skey);
    memset(g_sc_stamped, 0, sizeof g_sc_stamped), g_sc_kc_slot = 0, g_sc_kc_serial = 0, g_sc_hold_serial = 0, g_sc_hold_n = 0;
    memset(&g_sc_ap, 0, sizeof g_sc_ap);
    memset(&g_sc_wt, 0, sizeof g_sc_wt), memset(&g_sc_tl, 0, sizeof g_sc_tl), memset(&g_sc_tp, 0, sizeof g_sc_tp);
    sc_gate_arm(&g_sc_gate), g_sc_gate_arm_t = sc_now(), g_sc_pcaps_ver = 0;
    g_sc_caps_t = 0, g_sc_caps_seen = 0, g_sc_set3a8 = 0;
    if (g_sc_on) sc_send_caps();
}

static void scene_install(int *ok)
{
    char v[8];
    if (r2_env("REV2_SCENE", v, sizeof v) && v[0] == '0') g_sc_on = 0;
    void *t;
    static const unsigned char b_skipq[] = {0x56, 0x8B, 0xF1, 0x8B, 0x0D, 0x34, 0x91, 0x57, 0x01};   /* push esi; mov esi, ecx; mov ecx, [0x1579134] */
    if ((t = w_tramp(0x426AD0, b_skipq, 9))) o_skipq = (skipq_t)t, *ok += w_jmp(0x426AD0, 9, (void *)h_skipq);
    static const unsigned char b_pdpop[] = {0x56, 0x8B, 0xF1, 0x8B, 0x0D, 0xF8, 0xE6, 0x5D, 0x01};   /* push esi; mov esi, ecx; mov ecx, [0x15DE6F8] */
    if ((t = w_tramp(0xA9E5C0, b_pdpop, 9))) o_pdpop = (pdpop_t)t, *ok += w_jmp(0xA9E5C0, 9, (void *)h_pdpop);
    static const unsigned char b_slot[] = {0x55, 0x56, 0x8B, 0x74, 0x24, 0x10};   /* push ebp; push esi; mov esi, [esp+0x10] */
    if ((t = w_tramp(0xA9D530, b_slot, 6))) o_slotstart = (slotstart_t)t, *ok += w_jmp(0xA9D530, 6, (void *)h_slotstart);
    static const unsigned char b_vote[] = {0xA1, 0x00, 0xAE, 0x57, 0x01};   /* mov eax, [0x157ae00] */
    if ((t = w_tramp(0x9F4DF0, b_vote, 5))) o_netvote = (netvote_t)t, *ok += w_jmp(0x9F4DF0, 5, (void *)h_netvote);
    static const unsigned char b_dsub[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};   /* push ebp; mov ebp, esp; and esp, -16 */
    if ((t = w_tramp(0x75D3A0, b_dsub, 6))) o_dsub = (dsub_t)t, *ok += w_jmp(0x75D3A0, 6, (void *)h_dsub);
    static const unsigned char b_smstart[] = {0x83, 0xEC, 0x0C, 0x8B, 0x81, 0x6C, 0x43, 0x00, 0x00};   /* sub esp, 0xc; mov eax, [ecx+0x436c] */
    if ((t = w_tramp(0x773EA0, b_smstart, 9))) o_smstart = (smstart_t)t, *ok += w_jmp(0x773EA0, 9, (void *)h_smstart);
    static const unsigned char b_trig[] = {0x57, 0x8B, 0x7C, 0x24, 0x08, 0x8B, 0xCF, 0x8B, 0x07};   /* push edi; mov edi, [esp+8]; mov ecx, edi; mov eax, [edi] */
    if ((t = w_tramp(0x773F40, b_trig, 9))) o_trig = (trig_t)t, *ok += w_jmp(0x773F40, 9, (void *)h_trig);
    r2_log(W_LOG "scene sync %s (REV2_SCENE / `net scene 0|1`): movie skip query %s, player demo popup %s, network vote %s, scene slot start %s, director submit (first-scene gate) %s, "
           "scene set model start %s, scene set model trigger %s", g_sc_on ? "on" : "OFF", o_skipq ? "yes" : "NO", o_pdpop ? "yes" : "NO", o_netvote ? "yes" : "NO",
           o_slotstart ? "yes" : "NO", o_dsub ? "yes" : "NO", o_smstart ? "yes" : "NO", o_trig ? "yes" : "NO");
    if (!o_dsub) g_sc_gate_on = 0;   /* no hook: the partner must not wait for an SREADY that never comes */
}

/* LAB `net scene elist [tag]`: this PC's live enemies (sEnemy, both lists: object, class, layout, position, health, action) and both humans' positions, into the log with
 * its time stamp (both PCs asked together = the lists of one moment) and back to the console */
static void sc_elist(const char *tag, char *out, int size)
{
    unsigned m = rd32(SENEMY_CELL), p[2] = {player_obj(0), player_obj(1)};
    char t[24];
    snprintf(t, sizeof t, "%.20s", tag && *tag ? tag : "-");
    int o = snprintf(out, (size_t)size, "elist %s: area %u; player 0 action %d at (%.0f,%.0f,%.0f), player 1 action %d at (%.0f,%.0f,%.0f)", t, story_area_id(),
                     p[0] ? (int)rd32(p[0] + 0x1A54) : -1, p[0] ? rdf(p[0] + 0x40) : 0.0f, p[0] ? rdf(p[0] + 0x44) : 0.0f, p[0] ? rdf(p[0] + 0x48) : 0.0f,
                     p[1] ? (int)rd32(p[1] + 0x1A54) : -1, p[1] ? rdf(p[1] + 0x40) : 0.0f, p[1] ? rdf(p[1] + 0x44) : 0.0f, p[1] ? rdf(p[1] + 0x48) : 0.0f);
    r2_log(W_LOG "scene: %s", out);
    int n = 0;
    for (int k = 0; m && k < 2; k++) {
        unsigned c = rd32(m + (k ? 0x44 : 0x30)), a = rd32(m + (k ? 0x50 : 0x3C));
        for (unsigned i = 0; i < c && i < 64 && a; i++) {
            unsigned e = rd32(a + 4 * i);
            if (!e) continue;
            layout_t l;
            char cn[100], line[200];
            if (!layout_of(e, &l)) l.a = -1, l.b = 0, l.c = 0;
            snprintf(line, sizeof line, "list%d %08x %s (%d,%u,%u) at (%.0f,%.0f,%.0f) hp %d/%d act %d", k + 1, e, w_class(e, cn, sizeof cn), l.a, l.b, l.c, rdf(e + 0x40), rdf(e + 0x44),
                     rdf(e + 0x48), (int)rd32(e + 0x1A08), (int)rd32(e + 0x1A0C), (int)rd32(e + 0x1A54));
            n++;
            if (n <= 48) r2_log(W_LOG "scene: elist %s enemy %d: %s", t, n, line);
            if (o < size - 220) o += snprintf(out + o, (size_t)(size - o), "\n  %s", line);
        }
    }
    r2_log(W_LOG "scene: elist %s: %d enemies", t, n);
    if (o < size - 40) snprintf(out + o, (size_t)(size - o), "\n%d enemies", n);
}

static void scene_command(const char *arg, char *out, int size)
{
    while (*arg == ' ') arg++;
    if (!strncmp(arg, "elist", 5)) {   /* LAB: the enemy list of this moment */
        const char *v = arg + 5;
        while (*v == ' ') v++;
        sc_elist(v, out, size);
        return;
    }
    if (!strncmp(arg, "shots", 5)) {   /* LAB: own back-buffer shots 1, 3, 5 s after every in-engine scene start here */
        const char *v = arg + 5;
        while (*v == ' ') v++;
        if (*v == '0' || *v == '1') g_sc_shots = *v == '1';
    }
    if (!strncmp(arg, "view", 4)) {   /* LAB: the view guard and the role set-up leave the view to a playing scene (default on) */
        const char *v = arg + 4;
        while (*v == ' ') v++;
        if (*v == '0' || *v == '1') g_sc_view_on = *v == '1';
    }
    if (!strncmp(arg, "actor", 5)) {   /* LAB: a client creates the actor its own scene asks for (default on) */
        const char *v = arg + 5;
        while (*v == ' ') v++;
        if (*v == '0' || *v == '1') g_sc_actor_on = *v == '1';
    }
    if (!strncmp(arg, "trigdelay", 9)) {   /* LAB `net scene trigdelay <ms>`: the partner's TRIG is applied here that much later (0 = at once, the default) */
        int v = atoi(arg + 9);
        if (v >= 0 && v <= 5000) g_sc_lab_trigdelay = v;
        r2_log(W_LOG "scene: LAB net scene trigdelay %d ms", g_sc_lab_trigdelay);
    } else if (!strncmp(arg, "trig", 4)) {   /* LAB: the scene set model trigger to the partner (TRIG), default on */
        const char *v = arg + 4;
        while (*v == ' ') v++;
        if (*v == '0' || *v == '1') g_sc_trig_on = *v == '1';
    }
    if (!strncmp(arg, "hold", 4)) {   /* net scene hold 0|1: the first-start hold of in-engine scenes */
        const char *v = arg + 4;
        while (*v == ' ') v++;
        if (*v == '0' || *v == '1') g_sc_hold_on = *v == '1';
    }
    if (!strncmp(arg, "gateshot", 8)) {   /* LAB: one back-buffer shot 1.5 s into a gate wait */
        const char *v = arg + 8;
        while (*v == ' ') v++;
        if (*v == '0' || *v == '1') g_sc_gate_shot = *v == '1';
    } else if (!strncmp(arg, "gate", 4)) {   /* net scene gate 0|1: the gate of the first in-engine scene after a load */
        const char *v = arg + 4;
        while (*v == ' ') v++;
        if ((*v == '0' || *v == '1') && o_dsub) {
            g_sc_gate_on = *v == '1';
            if (!g_sc_gate_on) sc_gate_disarm(&g_sc_gate);
            sc_send_caps();
        }
    }
    if (!strncmp(arg, "vote", 4)) {   /* LAB: the next skip query of a skippable scene counts as this player's press (the vote path without the stock inputs) */
        g_sc_lab_press = 1;
        r2_log(W_LOG "scene: LAB net scene vote: the next skip query counts as a press");
    }
    if (*arg == '0' || *arg == '1') {
        g_sc_on = *arg == '1';
        if (!g_sc_on && g_sc_bar.holding) sc_bar_release(&g_sc_bar, sc_now());   /* a hold ends at the next check through the stock path */
        sc_send_caps();
    }
    char name[40];
    int p = sc_partner(name, sizeof name);
    snprintf(out, (size_t)size,
             "scene sync %s; partner %s (scene sync %s); hold %s%s%s (%u holds; released: match %u, in play %u, elsewhere %u, limit %u, alone %u, no sync %u, off %u); votes tx %u rx %u, skips %u; "
             "dedupe: own submits suppressed %u, partner scenes skipped %u, first-start holds %u (hold %s); first-scene gate %s%s (waits %u; released: match %u, started %u, alone %u, no gate %u, limit %u; partner caps %d); "
             "door steps back refused %u; clock %s offset %d ms rtt %u ms (%u samples); start gaps logged %u; triggers %s: sent %u, received %u (triggered %u, refused %u, "
             "had %u, first scene %u, no player %u, not waiting %u, no set model %u, waited %u: then triggered %u / theirs %u / expired %u); local starts %u (scheduler first %u); "
             "scene actors created here %u (other creations during a scene left to the guard %u), adopted by the host's records %u (%s); LAB shots %s, TRIG delay %d ms; hooks %s %s %s %s",
             g_sc_on ? "on" : "OFF", p ? name : "none", g_sc_bar.caps > 0 ? "on" : g_sc_bar.caps < 0 ? "off" : "?", g_sc_bar.holding ? (g_sc_bar.kind == SC_KIND_LOAD ? "LOADER " : "MOVIE ") : "none",
             g_sc_bar.holding ? g_sc_bar.key : "", "", g_sc_holds, g_sc_rel[SC_REL_MATCH], g_sc_rel[SC_REL_INPLAY], g_sc_rel[SC_REL_ELSEWHERE], g_sc_rel[SC_REL_LIMIT], g_sc_rel[SC_REL_ALONE],
             g_sc_rel[SC_REL_NOCAPS], g_sc_rel[SC_REL_OFF], g_sc_votes_tx, g_sc_votes_rx, g_sc_skipped, g_sc_dd_sup, g_sc_dd_skip, g_sc_scene_holds, g_sc_hold_on ? "on" : "off",
             g_sc_gate_on ? "on" : "off", g_sc_gate.waiting ? " WAITING" : g_sc_gate.armed ? " armed" : "", g_sc_gate_waits, g_sc_gate_rel[SC_GATE_MATCH], g_sc_gate_rel[SC_GATE_STARTED],
             g_sc_gate_rel[SC_GATE_ALONE], g_sc_gate_rel[SC_GATE_NOGATE], g_sc_gate_rel[SC_GATE_LIMIT], g_sc_pcaps_ver, g_sc_door_veto, g_sc_clk.have ? "yes" : "no", (int)g_sc_clk.offset,
             g_sc_clk.rtt, g_sc_clk.samples, g_sc_gaps, g_sc_trig_on ? "on" : "off", g_sc_trig_tx, g_sc_trig_rx, g_sc_trig_res[SC_TRIG_APPLY], g_sc_trig_refused,
             g_sc_trig_res[SC_TRIG_HAVE], g_sc_trig_res[SC_TRIG_WINDOW], g_sc_trig_res[SC_TRIG_NOPLAYER], g_sc_trig_res[SC_TRIG_STATE], g_sc_trig_res[SC_TRIG_NONE], g_sc_trig_res[SC_TRIG_WAIT],
             g_sc_tp_res[SC_TP_APPLY], g_sc_tp_res[SC_TP_DROP], g_sc_tp_res[SC_TP_EXPIRE], g_sc_ls_starts, g_sc_rx_sched, g_sc_actor_allowed, g_sc_actor_refused, g_sc_act_adopted, g_sc_actor_on ? "on" : "off", g_sc_shots ? "on" : "off", g_sc_lab_trigdelay, o_skipq ? "skipq" : "-", o_pdpop ? "pdpop" : "-", o_netvote ? "vote" : "-", o_trig ? "trig" : "-");
}
