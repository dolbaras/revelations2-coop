/*
 * Session-layer tests on the in-process "sim" transport with a virtual clock: handshake and version gate, refusals, lobby rules,
 * start, load barrier, stale-epoch drop, state streaming under loss / duplication / reordering with interpolation error,
 * three-player relay, leave / reconnect / host loss, malformed input. Deterministic (seeded), runs in a second.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/coop_session.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t vclock = 1000;

typedef struct {
    cs_t *s;
    cn_t *raw;           /* a hand-driven transport endpoint instead of a session (protocol-level tests) */
    uint32_t offset;     /* this machine's clock = vclock + offset (different uptimes) */
    int count[16];       /* notifications by kind */
    cs_note last[16];
    char text[16][120];
    cp_event ev;         /* last event received */
    int ev_n;
} Node;

static uint32_t clk(void *u) { return vclock + ((Node *)u)->offset; }
static void on_note(void *u, const cs_note *n)
{
    Node *nd = (Node *)u;
    if (n->what >= 0 && n->what < 16) {
        nd->count[n->what]++;
        nd->last[n->what] = *n;
        snprintf(nd->text[n->what], sizeof nd->text[n->what], "%s", n->text ? n->text : "");
        nd->last[n->what].text = nd->text[n->what];
    }
    if (n->what == CS_N_EVENT && n->ev) nd->ev = *n->ev, nd->ev_n++;
}

static void node_init(Node *nd, const char *name, int want, uint32_t offset)
{
    memset(nd, 0, sizeof *nd);
    nd->offset = offset;
    cs_config c;
    memset(&c, 0, sizeof c);
    c.name = name, c.want_char = want, c.mod_version = "test", c.is_bot = 1, c.clock = clk, c.clock_user = nd, c.on_note = on_note, c.user = nd;
    nd->s = cs_new(&c);
}

static Node *all_nodes[8];
static int n_all;
static void reg(Node *nd) { all_nodes[n_all++] = nd; }
static void unreg_all(void) { n_all = 0; }

static void step(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        vclock += 5;
        cn_sim_set_time(vclock);
        for (int i = 0; i < n_all; i++) {
            if (all_nodes[i]->s) cs_poll(all_nodes[i]->s);
            if (all_nodes[i]->raw) cn_pump(all_nodes[i]->raw);
        }
    }
}

static void faults(int loss, int dup, int reorder, int delay, int jitter, uint32_t seed)
{
    cn_sim_params p;
    memset(&p, 0, sizeof p);
    p.loss_pct = loss, p.dup_pct = dup, p.reorder_pct = reorder, p.delay_ms = delay, p.jitter_ms = jitter, p.reorder_extra_ms = 60, p.seed = seed;
    cn_sim_set(&p);
}

static cp_state path_state(int pid, uint32_t t_ms)
{
    /* a circle of radius 300 around (1000*pid, 0, 0), 200 units/s, a walking action */
    cp_state s;
    memset(&s, 0, sizeof s);
    double a = (double)t_ms / 1000.0 * (200.0 / 300.0);
    s.pos[0] = (float)(1000.0 * pid + 300.0 * cos(a)), s.pos[1] = 0, s.pos[2] = (float)(300.0 * sin(a));
    s.yaw = (float)fmod(a + 1.5707963, 6.2831853) - ((fmod(a + 1.5707963, 6.2831853) > 3.1415927) ? 6.2831853f : 0.0f);
    s.action = 3, s.hp = 800, s.hpmax = 800, s.weapon = 2;
    return s;
}

static int connect_pair(Node *h, Node *c, int port)
{
    char err[200], target[32];
    snprintf(target, sizeof target, "sim:%d", port);
    if (!cs_host(h->s, "sim", port, 0, err, sizeof err)) { printf("host: %s\n", err); return 0; }
    if (!cs_join(c->s, "sim", target, 0, err, sizeof err)) { printf("join: %s\n", err); return 0; }
    step(100);
    return c->count[CS_N_CONNECTED] == 1;
}

static void start_game(Node *h, Node *c)
{
    char err[100];
    cs_ready(c->s, 1);
    step(100);
    CHECK(cs_start(h->s, 0, err, sizeof err));
    step(100);
    cs_load_done(h->s);
    cs_load_done(c->s);
    step(100);
}

/* ------------------------------------------------------------------------------------------------------------------ */
static void test_handshake_lobby_start(void)
{
    printf("-- handshake, lobby, start, barrier\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Client", CP_CHAR_NONE, 0x7FFFF000u);   /* a client whose clock is 24 days ahead (wraps through 2^31 and 2^32) */
    reg(&h), reg(&c);
    faults(0, 0, 0, 0, 0, 1);
    CHECK(connect_pair(&h, &c, 1));
    CHECK(cs_pid(c.s) == 1 && cs_pid(h.s) == 0);
    CHECK(cs_phase(h.s) == CP_PHASE_LOBBY && cs_phase(c.s) == CP_PHASE_LOBBY);
    CHECK(cs_local_character(h.s) == CP_CHAR_CLAIRE && cs_local_character(c.s) == CP_CHAR_MOIRA);   /* default: host Claire, client Moira */
    CHECK(h.count[CS_N_PEER_JOIN] == 1 && cs_player_count(h.s) == 2 && cs_player_count(c.s) == 2);

    CHECK(cs_pick(c.s, CP_CHAR_CLAIRE));   /* asked; the host refuses with a notice */
    step(60);
    CHECK(c.count[CS_N_NOTICE] == 1 && c.last[CS_N_NOTICE].a == CP_NOTE_SLOT_TAKEN);
    CHECK(cs_local_character(c.s) == CP_CHAR_MOIRA);
    CHECK(cs_pick(c.s, CP_CHAR_BARRY));    /* Barry is the same slot as Claire */
    step(60);
    CHECK(c.count[CS_N_NOTICE] == 2 && cs_local_character(c.s) == CP_CHAR_MOIRA);
    CHECK(cs_pick(c.s, CP_CHAR_NATALIA));  /* the Barry/Natalia pair: Natalia is slot 1, free for the client */
    step(60);
    CHECK(cs_local_character(c.s) == CP_CHAR_NATALIA && cs_local_character(h.s) == CP_CHAR_CLAIRE);
    CHECK(!cs_pick(h.s, CP_CHAR_MOIRA));   /* the host cannot take the client's slot */
    CHECK(cs_pick(c.s, CP_CHAR_MOIRA));
    step(60);
    CHECK(cs_local_character(c.s) == CP_CHAR_MOIRA);

    char err[100];
    CHECK(!cs_start(h.s, 0, err, sizeof err));   /* the client is not ready */
    CHECK(strstr(err, "ready") != NULL);
    cp_session ss;
    cs_session_get(h.s, &ss);
    ss.episode = 3, ss.part = 1, ss.difficulty = 2;
    CHECK(cs_set_session(h.s, &ss));
    step(60);
    cs_session_get(c.s, &ss);
    CHECK(ss.episode == 3 && ss.part == 1 && ss.difficulty == 2 && ss.id == (cs_session_get(h.s, &ss), ss.id));
    CHECK(cs_ready(c.s, 1));
    step(60);
    cs_player_info pi;
    CHECK(cs_player(h.s, 1, &pi) && pi.ready && pi.connected);
    CHECK(cs_start(h.s, 0, err, sizeof err));
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING && cs_epoch(h.s) == 1 && cs_epoch(c.s) == 1);
    CHECK(h.count[CS_N_START] == 1 && c.count[CS_N_START] == 1);
    CHECK(!cs_pick(c.s, CP_CHAR_CLAIRE));   /* locked after the start */

    /* barrier: GO only when both loaded */
    cp_state st = path_state(0, 0);
    CHECK(!cs_submit_state(h.s, &st));       /* no state while loading */
    cs_load_done(h.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING);
    cs_load_done(c.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    CHECK(h.count[CS_N_GO] == 1 && c.count[CS_N_GO] == 1);

    /* heartbeats give an RTT (sim delay 0 -> 0..10 ms) */
    step(2500);
    CHECK(cs_player(h.s, 1, &pi) && pi.rtt_ms >= 0 && pi.rtt_ms <= 20);
    CHECK(cs_player(c.s, 0, &pi) && pi.rtt_ms >= 0 && pi.rtt_ms <= 20);
    char text[1200];
    cs_status(h.s, text, sizeof text);
    CHECK(strstr(text, "HOST") && strstr(text, "moira") && strstr(text, "playing"));
    cs_free(h.s), cs_free(c.s);
}

static void test_refusals(void)
{
    printf("-- refusals: version gate, build gate, full, garbage, hello first\n");
    Node h;
    unreg_all();
    cs_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.name = "Host", cfg.want_char = CP_CHAR_CLAIRE, cfg.mod_version = "t", cfg.clock = clk, cfg.clock_user = &h, cfg.on_note = on_note, cfg.user = &h;
    for (int i = 0; i < 8; i++) cfg.exe_tag[i] = (uint8_t)(0xA0 + i);
    memset(&h, 0, sizeof h);
    h.s = cs_new(&cfg);
    reg(&h);
    faults(0, 0, 0, 0, 0, 1);
    char err[200];
    CHECK(cs_host(h.s, "sim", 2, 0, err, sizeof err));

    struct { const char *what; uint16_t ver, minv; int tag; int expect; } cases[] = {
        {"too new", 5, 3, 1, CP_REF_VERSION},
        {"too old", 0, 0, 1, CP_REF_VERSION},
        {"version 1 (40 byte states)", 1, 1, 1, CP_REF_VERSION},
        {"wrong build", 2, 2, 2, CP_REF_BUILD},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Node r;
        memset(&r, 0, sizeof r);
        r.raw = cn_new();
        reg(&r);
        CHECK(cn_join(r.raw, "sim", "sim:2", 0, err, sizeof err));
        step(30);
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_HELLO;
        m.u.hello.version = cases[i].ver, m.u.hello.min_version = cases[i].minv, m.u.hello.want_char = CP_CHAR_NONE;
        snprintf(m.u.hello.name, sizeof m.u.hello.name, "Raw%u", i);
        for (int k = 0; k < 8; k++) m.u.hello.exe_tag[k] = cases[i].tag == 1 ? (uint8_t)(0xA0 + k) : cases[i].tag == 2 ? 0x11 : 0;
        uint8_t buf[CP_MAX_PACKET];
        size_t n = cp_encode(&m, buf, sizeof buf);
        cn_send(r.raw, 0, CP_CH_CONTROL, 1, buf, n);
        step(60);
        cn_event ev;
        int got = 0;
        while (cn_recv(r.raw, &ev)) {
            if (ev.type == CN_EV_RECEIVE && cp_decode(ev.data, ev.len, &m) && m.type == CP_REFUSE) {
                CHECK(m.u.refuse.code == cases[i].expect);
                got = 1;
            }
            cn_event_free(&ev);
        }
        if (!got) printf("  no REFUSE for case '%s'\n", cases[i].what);
        CHECK(got);
        CHECK(cs_player_count(h.s) == 1);   /* nobody was added */
        cn_free(r.raw);
        r.raw = NULL;
        unreg_all();
        reg(&h);
        step(40);
    }

    /* a matching hello is accepted (zero tag = bot skips the build check) */
    {
        Node r;
        memset(&r, 0, sizeof r);
        r.raw = cn_new();
        reg(&r);
        cn_join(r.raw, "sim", "sim:2", 0, err, sizeof err);
        step(30);
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_HELLO, m.u.hello.version = 2, m.u.hello.min_version = 2, m.u.hello.want_char = CP_CHAR_NONE;
        snprintf(m.u.hello.name, sizeof m.u.hello.name, "NoTag");
        uint8_t buf[CP_MAX_PACKET];
        cn_send(r.raw, 0, CP_CH_CONTROL, 1, buf, cp_encode(&m, buf, sizeof buf));
        step(60);
        CHECK(cs_player_count(h.s) == 2);
        /* garbage after the handshake: counted, no crash */
        uint8_t junk[] = {0x77, 1, 2, 3, 4};
        cn_send(r.raw, 0, CP_CH_CONTROL, 1, junk, sizeof junk);
        cn_send(r.raw, 0, CP_CH_STATE, 0, junk, 3);
        step(40);
        cs_stats st;
        cs_get_stats(h.s, &st);
        CHECK(st.rx_bad == 2);
        cn_free(r.raw);
        unreg_all();
        reg(&h);
        step(40);
        CHECK(cs_player_count(h.s) == 1);   /* the raw peer left: lobby seat freed */
    }

    /* data before the hello is dropped; a peer that never says hello is cut after the deadline */
    {
        Node r;
        memset(&r, 0, sizeof r);
        r.raw = cn_new();
        reg(&r);
        cn_join(r.raw, "sim", "sim:2", 0, err, sizeof err);
        step(30);
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_LOBBY_PICK, m.u.pick.character = CP_CHAR_MOIRA;
        uint8_t buf[CP_MAX_PACKET];
        cn_send(r.raw, 0, CP_CH_CONTROL, 1, buf, cp_encode(&m, buf, sizeof buf));
        step(100);
        CHECK(cs_player_count(h.s) == 1);
        step(5200);
        CHECK(!cn_connected(r.raw, 0));
        cn_free(r.raw);
        unreg_all();
        reg(&h);
    }
    cs_free(h.s);

    /* full: a third player in a two-player session */
    {
        Node a, b, c;
        unreg_all();
        node_init(&a, "A", CP_CHAR_CLAIRE, 0), node_init(&b, "B", CP_CHAR_NONE, 0), node_init(&c, "C", CP_CHAR_NONE, 0);
        reg(&a), reg(&b), reg(&c);
        CHECK(connect_pair(&a, &b, 3));
        CHECK(cs_join(c.s, "sim", "sim:3", 0, err, sizeof err));
        step(100);
        CHECK(c.count[CS_N_REFUSED] == 1 && c.last[CS_N_REFUSED].a == CP_REF_FULL);
        CHECK(!cs_active(c.s) && cs_player_count(a.s) == 2);
        cs_free(a.s), cs_free(b.s), cs_free(c.s);
    }
}

static void test_barrier_epochs(void)
{
    printf("-- second load: client-initiated epoch, host joins it, stale states dropped\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Client", CP_CHAR_NONE, 12345);
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 1);
    CHECK(connect_pair(&h, &c, 4));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    /* a few states flow */
    for (int i = 0; i < 40; i++) {
        cp_state s = path_state(0, cs_now(h.s));
        cs_submit_state(h.s, &s);
        s = path_state(1, cs_now(c.s));
        cs_submit_state(c.s, &s);
        step(50);
    }
    cs_stats st;
    cs_get_stats(h.s, &st);
    CHECK(st.rx_state >= 35);
    /* the client starts loading level instance 2 */
    cs_load_begin(c.s, 700);
    step(80);
    CHECK(cs_epoch(c.s) == 2 && cs_epoch(h.s) == 2);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING && cs_phase(h.s) == CP_PHASE_LOADING);
    CHECK(h.count[CS_N_LOAD_BEGIN] >= 1 && h.last[CS_N_LOAD_BEGIN].pid == 1 && h.last[CS_N_LOAD_BEGIN].b == 700);
    /* the host's game loads too (its glue answers the notification with its own begin/done) */
    cs_load_begin(h.s, 700);
    CHECK(cs_epoch(h.s) == 2);   /* joins the announced epoch instead of advancing */
    cs_load_done(c.s);
    step(60);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);
    cs_load_done(h.s);
    step(80);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING && c.count[CS_N_GO] == 2 && h.count[CS_N_GO] == 2);
    /* a state of the old epoch that arrives late is dropped as stale */
    cp_msg m;
    memset(&m, 0, sizeof m);
    m.type = CP_STATE;
    m.u.state = path_state(1, cs_now(c.s));
    m.u.state.pid = 1, m.u.state.seq = 60000, m.u.state.t_ms = cs_now(c.s), m.u.state.epoch = 1;
    uint8_t raw[64];
    size_t n = cp_encode(&m, raw, sizeof raw);
    cn_send(cs_transport(c.s), 0, CP_CH_STATE, 0, raw, n);
    step(60);
    cs_get_stats(h.s, &st);
    CHECK(st.rx_stale_epoch >= 1);
    /* both begin a load at the same moment (host-first): the epochs agree, no double increment */
    cs_load_begin(h.s, 800);
    cs_load_begin(c.s, 800);
    step(100);
    CHECK(cs_epoch(h.s) == 3 && cs_epoch(c.s) == 3);
    cs_load_done(h.s), cs_load_done(c.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    cs_free(h.s), cs_free(c.s);
}

static void test_stream_under_faults(const char *label, int loss, int dup, int reorder, int delay, int jitter, double max_err, double max_mean)
{
    printf("-- 20 Hz stream, %s: loss %d%% dup %d%% reorder %d%% delay %d+%d ms\n", label, loss, dup, reorder, delay, jitter);
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Client", CP_CHAR_NONE, 0x80000100u);   /* clocks 24 days apart */
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 7);
    CHECK(connect_pair(&h, &c, 5));
    start_game(&h, &c);
    faults(loss, dup, reorder, delay, jitter, 99);
    double err_sum = 0, err_max = 0;
    int err_n = 0, none = 0, stale = 0;
    int sent = 0;
    for (int t = 0; t < 30000; t += 5) {
        if (t % 50 == 0) {   /* 20 Hz */
            cp_state s = path_state(0, cs_now(h.s));
            sent += cs_submit_state(h.s, &s);
            cp_state s2 = path_state(1, cs_now(c.s));
            cs_submit_state(c.s, &s2);
        }
        step(5);
        if (t > 3000 && t % 15 == 0) {   /* the game samples about 60 times a second */
            cp_state out;
            int r = cs_remote_sample(c.s, 0, cs_now(c.s), &out);
            if (r == CS_SAMPLE_NONE) { none++; continue; }
            if (r == CS_SAMPLE_STALE) stale++;
            cp_state truth = path_state(0, out.t_ms);   /* out.t_ms = the sender clock time being shown */
            double dx = out.pos[0] - truth.pos[0], dz = out.pos[2] - truth.pos[2], e = sqrt(dx * dx + dz * dz);
            err_sum += e, err_n++;
            if (e > err_max) err_max = e;
        }
    }
    cs_stats st;
    cs_get_stats(c.s, &st);
    cs_player_info pi;
    cs_player(c.s, 0, &pi);
    printf("   client received %u of %d sent (gap %u, dup dropped %u, old dropped %u); position error mean %.2f max %.2f units over %d samples; stale %d none %d\n", st.rx_state, sent,
           st.rx_gap, st.rx_dup, st.rx_old, err_n ? err_sum / err_n : -1, err_max, err_n, stale, none);
    CHECK(err_n > 1000 && none == 0);
    CHECK(err_max <= max_err);
    CHECK(err_n && err_sum / err_n <= max_mean);
    CHECK(st.rx_state + st.rx_gap >= (uint32_t)sent - 3);   /* every sent sample is either accepted or counted as a gap */
    if (dup) CHECK(st.rx_dup > 0);
    if (reorder) CHECK(st.rx_old > 0 || st.rx_gap > 0);
    if (loss) CHECK(st.rx_gap > 0);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    cs_free(h.s), cs_free(c.s);
}

static void test_interpolation_cases(void)
{
    printf("-- interpolation: teleport, extrapolation cap, stale, yaw wrap\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Client", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 0, 0, 1);
    CHECK(connect_pair(&h, &c, 6));
    start_game(&h, &c);
    cp_state s;
    memset(&s, 0, sizeof s);
    s.hp = 100, s.hpmax = 100;
    for (int i = 0; i < 10; i++) {
        s.pos[0] = 100.0f + 10.0f * (float)i, s.yaw = 3.0f + 0.05f * (float)i;   /* yaw crosses +pi */
        if (s.yaw > 3.1415927f) s.yaw -= 6.2831853f;
        cs_submit_state(h.s, &s);
        step(50);
    }
    cp_state out;
    int r = cs_remote_sample(c.s, 0, cs_now(c.s), &out);
    CHECK(r == CS_SAMPLE_INTERP || r == CS_SAMPLE_EXTRAP);
    CHECK(out.pos[0] > 100 && out.pos[0] <= 190);
    CHECK(fabsf(out.yaw) > 2.5f || fabsf(out.yaw) < 0.05f * 10);   /* never the average across the wrap (~0) of two values near +-pi */
    /* teleport: the sample after the jump is not blended */
    s.pos[0] = 5000.0f, s.flags = CP_SF_TELEPORT;
    cs_submit_state(h.s, &s);
    s.flags = 0;
    step(50);
    s.pos[0] = 5010.0f;
    cs_submit_state(h.s, &s);
    for (int i = 0; i < 5; i++) {
        step(30);
        cs_remote_sample(c.s, 0, cs_now(c.s), &out);
        CHECK(out.pos[0] < 195 || out.pos[0] > 4990);   /* never somewhere between */
    }
    /* silence: extrapolation is capped, then STALE after 1 s */
    step(300);
    r = cs_remote_sample(c.s, 0, cs_now(c.s), &out);
    CHECK(r == CS_SAMPLE_EXTRAP || r == CS_SAMPLE_INTERP);
    CHECK(out.pos[0] < 5010 + 100);
    step(1500);
    r = cs_remote_sample(c.s, 0, cs_now(c.s), &out);
    CHECK(r == CS_SAMPLE_STALE);
    CHECK(cs_remote_sample(c.s, 1, cs_now(c.s), &out) == CS_SAMPLE_NONE);   /* own pid / unknown */
    CHECK(cs_remote_sample(c.s, 3, cs_now(c.s), &out) == CS_SAMPLE_NONE);
    cs_free(h.s), cs_free(c.s);
}

static void test_three_players_relay_events(void)
{
    printf("-- three players: relay of state and events through the host\n");
    Node h, a, b;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&a, "A", CP_CHAR_NONE, 0);
    node_init(&b, "B", CP_CHAR_NONE, 777);
    reg(&h), reg(&a), reg(&b);
    faults(0, 0, 0, 5, 0, 1);
    char err[100];
    CHECK(cs_host(h.s, "sim", 7, 0, err, sizeof err));
    cp_session ss;
    cs_session_get(h.s, &ss);
    ss.max_players = 3;
    CHECK(cs_set_session(h.s, &ss));
    CHECK(cs_join(a.s, "sim", "sim:7", 0, err, sizeof err));
    CHECK(cs_join(b.s, "sim", "sim:7", 0, err, sizeof err));
    step(200);
    CHECK(cs_player_count(h.s) == 3 && cs_player_count(a.s) == 3 && cs_player_count(b.s) == 3);
    CHECK(cs_pid(a.s) != cs_pid(b.s));
    cs_ready(a.s, 1), cs_ready(b.s, 1);
    step(100);
    CHECK(cs_start(h.s, 0, err, sizeof err));
    step(100);
    cs_load_done(h.s), cs_load_done(a.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);   /* B is still loading */
    cs_load_done(b.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(a.s) == CP_PHASE_PLAYING && cs_phase(b.s) == CP_PHASE_PLAYING);
    for (int i = 0; i < 30; i++) {
        cp_state s = path_state(cs_pid(a.s), cs_now(a.s));
        cs_submit_state(a.s, &s);
        step(50);
    }
    cp_state out;
    CHECK(cs_remote_sample(b.s, cs_pid(a.s), cs_now(b.s), &out) != CS_SAMPLE_NONE);   /* B sees A's state, relayed */
    CHECK(out.pid == cs_pid(a.s) && fabsf(out.pos[0] - (1000.0f * (float)cs_pid(a.s) + 300.0f)) < 400.0f);
    CHECK(cs_remote_sample(h.s, cs_pid(a.s), cs_now(h.s), &out) != CS_SAMPLE_NONE);
    CHECK(cs_remote_sample(a.s, cs_pid(a.s), cs_now(a.s), &out) == CS_SAMPLE_NONE);   /* no echo of oneself */
    const char body[] = {1, 2, 3, 4};
    CHECK(cs_send_event(a.s, CP_EV_PICKUP, body, sizeof body));
    step(100);
    CHECK(h.ev_n == 1 && h.ev.kind == CP_EV_PICKUP && h.ev.pid == cs_pid(a.s) && h.ev.len == 4 && !memcmp(h.ev.body, body, 4));
    CHECK(b.ev_n == 1 && b.ev.kind == CP_EV_PICKUP && b.ev.pid == cs_pid(a.s));   /* relayed */
    CHECK(a.ev_n == 0);
    /* WORLD batches (stage 6): unreliable and sequenced, relayed by the host, never echoed, newest 64 kept */
    {
        uint8_t wb[200];
        cp_world w;
        for (int i = 0; i < 200; i++) wb[i] = (uint8_t)(i * 7);
        CHECK(cs_send_world(a.s, 1, wb, sizeof wb));
        step(100);
        CHECK(cs_recv_world(h.s, &w) && w.pid == cs_pid(a.s) && w.kind == 1 && w.len == 200 && !memcmp(w.body, wb, 200));
        CHECK(cs_recv_world(b.s, &w) && w.pid == cs_pid(a.s) && w.len == 200);   /* relayed */
        CHECK(!cs_recv_world(a.s, &w) && !cs_recv_world(h.s, &w));
        CHECK(!cs_send_world(a.s, 1, wb, CP_WORLD_MAX + 1));
        for (int i = 0; i < 70; i++) cs_send_world(h.s, 2, wb, 10), step(5);
        int n = 0;
        while (cs_recv_world(a.s, &w)) n++;
        CHECK(n > 0 && n <= 64);
        uint32_t tx, rx, dr;
        cs_world_stats(h.s, &tx, &rx, &dr);
        CHECK(tx == 70 && rx == 1);
    }
    /* a client cannot speak for someone else: a forged state / event with another pid is ignored by the host */
    cp_msg m;
    memset(&m, 0, sizeof m);
    m.type = CP_EVENT, m.u.event.pid = 0, m.u.event.kind = CP_EV_DEATH;
    uint8_t raw[64];
    cn_send(cs_transport(a.s), 0, CP_CH_CONTROL, 1, raw, cp_encode(&m, raw, sizeof raw));
    step(60);
    CHECK(h.ev_n == 1 && b.ev_n == 1);
    /* a client leaves: the others are told */
    cs_leave(a.s, 1);
    step(100);
    CHECK(h.count[CS_N_PEER_LEAVE] >= 1 && b.count[CS_N_LOBBY] >= 2);
    CHECK(cs_player_count(h.s) == 2);
    cs_free(h.s), cs_free(a.s), cs_free(b.s);
}

/* the partner leaves while a level load is pending (the pause menu's Leave goes through the game's quit flow, which announces a load first) */
static void test_leave_during_load(void)
{
    printf("-- partner leaves while the barrier waits\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 5, 0, 1);
    CHECK(connect_pair(&h, &c, 21));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING);
    cs_load_begin(c.s, 100);   /* the client announces a load ... */
    step(60);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);
    cs_load_begin(h.s, 100), cs_load_done(h.s);   /* ... the host is loaded at once and waits for it */
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);
    cs_free(c.s);   /* ... and the client quits instead of finishing it */
    unreg_all();
    reg(&h);
    step(200);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING);   /* the barrier opened without the missing player */
    cs_free(h.s);
    unreg_all();
}

/* stage 6f (retry hang, quit during a barrier): every way a barrier can lose a player must leave both PCs in a defined state */
static void test_quit_during_barrier(void)
{
    printf("-- 6f: host quits / client quits / client never reports during a retry barrier\n");
    char err[100];
    Node h, c;

    /* (1) the host starts a retry load, loads, waits; the client quits without ever finishing its load: the host runs on */
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 5, 0, 1);
    CHECK(connect_pair(&h, &c, 31));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    cs_load_begin(h.s, 100);   /* the host's retry */
    step(60);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING);
    cs_load_done(h.s);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);   /* waits for the client */
    cs_free(c.s);
    unreg_all();
    reg(&h);
    step(200);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1 && cs_phase(h.s) == CP_PHASE_PLAYING);
    cs_free(h.s);
    unreg_all();

    /* (2) the client has loaded and waits at the barrier, the host quits: the client is told (LOST) and its session is gone, not stuck in LOADING */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(connect_pair(&h, &c, 32));
    start_game(&h, &c);
    cs_load_begin(c.s, 100);
    step(60);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING);
    cs_load_done(c.s);
    step(60);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING);   /* the host has not loaded yet */
    cs_leave(h.s, 0);                           /* the host quits (pause menu Leave / window closed) */
    step(100);
    CHECK(c.count[CS_N_LOST] == 1 && !cs_active(c.s) && cs_phase(c.s) == CP_PHASE_NONE);
    cs_free(h.s), cs_free(c.s);
    unreg_all();

    /* (3) the host disappears without a word (crash): same result for the waiting client */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(connect_pair(&h, &c, 33));
    start_game(&h, &c);
    cs_load_begin(h.s, 100);
    step(60);
    cs_load_done(c.s);
    cn_disconnect_peer(cs_transport(h.s), 1, 0);
    step(100);
    CHECK(c.count[CS_N_LOST] == 1 && !cs_active(c.s));
    cs_free(h.s), cs_free(c.s);
    unreg_all();

    /* (4) the client's game hangs and never reports "loaded" (the s6e2 retry hang): the host's barrier timeout opens the barrier, both PCs end in PLAYING */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(connect_pair(&h, &c, 34));
    start_game(&h, &c);
    cs_load_begin(h.s, 100);
    cs_load_begin(c.s, 100);
    step(60);
    cs_load_done(h.s);
    step(60000);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING);   /* 60 s: still waiting */
    step(70000);   /* 130 s: past the OLD 120 s limit (6i raised it to 300 s: the N100 needs 150 s for a story load) */
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING);
    CHECK(h.count[CS_N_WAIT] >= 4 && h.last[CS_N_WAIT].pid == 1 && !strcmp(h.last[CS_N_WAIT].text, "Moira Player") && h.last[CS_N_WAIT].b == 300);   /* notes at 20, 50, 80, 110 s name the awaited player and the limit */
    step(180000);   /* 310 s: past the barrier timeout */
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    CHECK(h.count[CS_N_GO] >= 2 && c.count[CS_N_GO] >= 2);
    CHECK(c.count[CS_N_WAIT] == 0);   /* the client never finished loading: it has nothing to wait for */
    (void)err;
    cs_free(h.s), cs_free(c.s);
    unreg_all();
}

/* stage 6i: a slow story load (the N100 needs ~150 s) must not make the host open the barrier early, and the machine that finished first tells its player who it waits for */
static void test_slow_load_barrier(void)
{
    printf("-- 6i: slow load of 150 s, waiting notes on the machine that is ready first\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 5, 0, 1);
    CHECK(connect_pair(&h, &c, 35));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    int go_h = h.count[CS_N_GO];

    /* (a) the CLIENT is the slow one: the host is ready after 5 s and waits 145 s more, no timeout */
    cs_load_begin(h.s, 153);
    cs_load_begin(c.s, 153);
    step(5000);
    cs_load_done(h.s);
    step(10000);
    CHECK(h.count[CS_N_WAIT] == 0);   /* before the first note (20 s) */
    step(10000);
    CHECK(h.count[CS_N_WAIT] == 1 && h.last[CS_N_WAIT].a >= 20 && h.last[CS_N_WAIT].a <= 21);
    step(125000);   /* 150 s */
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING);
    CHECK(h.count[CS_N_WAIT] == 5 && h.last[CS_N_WAIT].a >= 140);   /* 20 50 80 110 140 s */
    cs_load_done(c.s);
    step(200);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING && h.count[CS_N_GO] == go_h + 1);   /* "all loaded", not the timeout */
    int w = h.count[CS_N_WAIT];
    step(70000);
    CHECK(h.count[CS_N_WAIT] == w);   /* playing: no more notes */

    /* (b) the HOST is the slow one: the client is ready after 5 s, waits for the host and says so (no limit known on a client: b = 0) */
    int cw = c.count[CS_N_WAIT];
    cs_load_begin(h.s, 154);
    step(100);
    cs_load_begin(c.s, 154);   /* the client's own game starts its load (it joined the epoch the host announced) */
    step(5000);
    cs_load_done(c.s);
    step(60000);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING && c.count[CS_N_WAIT] == cw + 2 && c.last[CS_N_WAIT].pid == 0 && !strcmp(c.last[CS_N_WAIT].text, "Host") && c.last[CS_N_WAIT].b == 0);
    cs_load_done(h.s);
    step(200);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    cs_free(h.s), cs_free(c.s);
    unreg_all();
}

static void test_leave_reconnect_hostloss(void)
{
    printf("-- client leaves and comes back mid-game, host leaves\n");
    Node h, c, c2;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 5, 0, 1);
    CHECK(connect_pair(&h, &c, 8));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING);
    cs_free(c.s);   /* quits abruptly: LEAVE + disconnect */
    unreg_all();
    reg(&h);
    step(200);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1 && cs_player_count(h.s) == 1);
    cs_player_info pi;
    CHECK(cs_player(h.s, 1, &pi) && !pi.connected && pi.character == CP_CHAR_MOIRA);   /* the seat is kept */
    /* a stranger cannot take the seat */
    node_init(&c2, "Somebody Else", CP_CHAR_NONE, 0);
    reg(&c2);
    char err[100];
    CHECK(cs_join(c2.s, "sim", "sim:8", 0, err, sizeof err));
    step(100);
    CHECK(c2.count[CS_N_REFUSED] == 1 && c2.last[CS_N_REFUSED].a == CP_REF_STARTED);
    cs_free(c2.s);
    unreg_all();
    reg(&h);
    /* the same name returns: same pid, same character, a new barrier */
    Node r;
    node_init(&r, "Moira Player", CP_CHAR_NONE, 0);
    reg(&r);
    CHECK(cs_join(r.s, "sim", "sim:8", 0, err, sizeof err));
    step(150);
    CHECK(r.count[CS_N_CONNECTED] == 1 && cs_pid(r.s) == 1 && cs_local_character(r.s) == CP_CHAR_MOIRA);
    CHECK(cs_phase(r.s) == CP_PHASE_LOADING && cs_phase(h.s) == CP_PHASE_LOADING && cs_epoch(h.s) == cs_epoch(r.s));
    cs_load_done(h.s), cs_load_done(r.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(r.s) == CP_PHASE_PLAYING);
    /* host goes away */
    cs_leave(h.s, 0);
    step(100);
    CHECK(r.count[CS_N_LOST] == 1 && !cs_active(r.s));
    cs_free(h.s), cs_free(r.s);

    /* the host disappears without a word: client sees the transport drop */
    Node h2, c3;
    unreg_all();
    node_init(&h2, "H", CP_CHAR_CLAIRE, 0), node_init(&c3, "C", CP_CHAR_NONE, 0);
    reg(&h2), reg(&c3);
    CHECK(connect_pair(&h2, &c3, 9));
    cn_disconnect_peer(cs_transport(h2.s), 1, 0);   /* the transport kills the link, no LEAVE message */
    step(100);
    CHECK(c3.count[CS_N_LOST] == 1);
    cs_free(h2.s), cs_free(c3.s);

    /* the join that nobody answers */
    Node c4;
    unreg_all();
    node_init(&c4, "C", CP_CHAR_NONE, 0);
    reg(&c4);
    CHECK(!cs_join(c4.s, "sim", "sim:77", 0, err, sizeof err));
    CHECK(strstr(err, "no host") != NULL);
    cs_free(c4.s);
}

static void test_transport_selection(void)
{
    printf("-- transport selection: unknown name, Steam without a platform, real ENet host and join on loopback\n");
    Node h;
    unreg_all();
    node_init(&h, "H", CP_CHAR_CLAIRE, 0);
    char err[200];
    CHECK(!cs_host(h.s, "nonsense", 1, 0, err, sizeof err) && strstr(err, "unknown transport"));
    CHECK(!cs_host(h.s, "steam", 1, 0, err, sizeof err) && strstr(err, "game process"));   /* the real Steam backend; no Steam in a test process (test_steam.c has the mock) */
    CHECK(!cs_join(h.s, "steam", "lobby:109775241006780450", 0, err, sizeof err) && strstr(err, "game process"));
    CHECK(!cs_join(h.s, "enet", "", 0, err, sizeof err) && strstr(err, "bad address"));
    CHECK(!cs_join(h.s, "enet", "no.such.host.invalid:28410", 0, err, sizeof err));
    CHECK(!cs_active(h.s));
    /* the command parser (what `net ...` runs) */
    char out[400];
    CHECK(cs_command(h.s, "status", out, sizeof out) && strstr(out, "no session"));
    CHECK(cs_command(h.s, "char barry", out, sizeof out) && strstr(out, "barry"));
    CHECK(cs_command(h.s, "host 28431 enet", out, sizeof out) && strstr(out, "hosting") && strstr(out, "barry"));
    CHECK(cs_is_host(h.s) && cs_local_character(h.s) == CP_CHAR_BARRY);
    CHECK(cs_command(h.s, "session 2 1 3", out, sizeof out) && strstr(out, "episode 2"));
    CHECK(!cs_command(h.s, "frobnicate", out, sizeof out));
    CHECK(cs_command(h.s, "stop", out, sizeof out) && !cs_active(h.s));
    cs_free(h.s);
}

int main(void)
{
    cn_sim_set_time(vclock);
    test_handshake_lobby_start();
    test_refusals();
    test_barrier_epochs();
    test_stream_under_faults("clean", 0, 0, 0, 20, 0, 1.0, 0.5);
    test_stream_under_faults("lan jitter", 0, 0, 0, 5, 10, 2.0, 0.7);
    test_stream_under_faults("loss 15%", 15, 0, 0, 40, 10, 6.0, 1.5);
    test_stream_under_faults("loss + dup + reorder", 10, 10, 10, 50, 20, 8.0, 2.0);
    test_stream_under_faults("bad wifi", 25, 5, 15, 80, 40, 20.0, 4.0);
    test_interpolation_cases();
    test_three_players_relay_events();
    test_leave_during_load();
    test_quit_during_barrier();
    test_slow_load_barrier();
    test_leave_reconnect_hostloss();
    test_transport_selection();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
