/*
 * Headless co-op bot: speaks the real protocol over real UDP (ENet) with the same session code as the game, no game needed.
 *
 *   coop_bot host [--port 28411] [--char claire] [--name Host] [--players 2] ...
 *   coop_bot join <ip:port> [--char moira] [--name Guest] ...
 *
 * Script of a run: connect -> lobby (pick / ready, the host starts when everybody is ready) -> simulated level load (--load-ms)
 * -> PLAYING: walks a circle (radius 300, 200 units/s, centre 1000*pid) and sends its state at 20 Hz, samples the other
 * player at 60 Hz and measures the position error against the other bot's known path -> optional second level load
 * (--load-at), an event (--event-at), an early leave (--leave-at) -> prints one RESULT json line.
 * Options: --seconds N (playing time, default 12) --load-ms N (default 800) --load-at S --event-at S --leave-at S
 *          --autostart-delay MS --no-ready --verbose --exe-tag HEX16 --die-at S (exit without goodbye)
 * netflake (net suite): --linger S (stay after the playing time until the partner left, at most S s), --play-clock (--seconds counts only playing
 *          time with the partner's states fresh: not loads, barrier waits, a dead link), --event-every MS (reliable event stream, delivery times); "diag"
 * Partner of a REAL game (downed / revive live run B, notes\downed-live-checks.md): instead of the circle far away
 *          --follow D    stand idle D units (x) beside the other player's sampled position (nothing is sent before the first sample)
 *          --hp N        the hp / max hp it reports (default 800)
 *          --down-at S   at S seconds of play it "goes down": the DOWN event (kind 0x160, body my slot, 255) once, then STATE with CP_SF_DOWN, hp 1, action 0x7A
 *          --bleed-s S   S seconds after going down without a DONE for it: the DEAD event and STATE with CP_SF_DEAD, hp 0 (default: never)
 *          --revive-after S   when the other player lies (its DOWN event or CP_SF_DOWN in its STATE), S seconds later the bot "holds the button" on it: BEGIN, KEEPALIVE
 *                             every 500 ms, DONE after --revive-ms (default 3000); --revive-cancel 1 = the first hold is broken off half-way (CANCEL), later ones complete
 *          --ctl FILE    read FILE twice a second; when its text changed, run its lines once: `revive on|off`, `cancel on|off`, `down`, `up` (stand up, DONE "I am up"),
 *                        `dead`, `quit` (a lab script switches the bot during one session; the --revive-after delay applies, default 3 s once switched on)
 *          the downed events (0x160..0x165) it receives are printed on stderr ("downed rx ..."); a DONE whose target is its slot stands it up again
 * Lobby checks (0.2.0, host only): --stall-ms N   no session poll for the first N ms (a joining game shows "Awaiting reply from ...")
 *                                   --kick-at S    S seconds after the other player is in the lobby: Remove partner (LEAVE REMOVED; its name is refused then);
 *                                                  with --autostart-delay large the bot host never starts
 * Release gate checks (0.2.0, gate020): --mod-version S   the co-op package version it announces (default "bot"; "none" = an older host that sends none)
 *                                       --as-player       not marked as a bot, so the mod-version gate applies to it as to a real game
 *                                       --freeze-load-ms N  at the start of its level load: no session poll for N ms (the transport thread keeps the link: a stalled game)
 *                                       --leave-load-ms N   N ms into its level load: leave the session with a goodbye (a player who quits while loading)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/coop_session.h"

static int verbose;
static DWORD t_start;

static void logline(const char *line)
{
    if (verbose) fprintf(stderr, "[%6lu] %s\n", (unsigned long)(GetTickCount() - t_start), line);
}

typedef struct {
    int connected, refused, lost, started, go_count, load_begin_count, events, notices, lobby_changes;
    char refuse_text[120];
    int refuse_code;
    int lost_a, lost_b;      /* netflake: the CS_N_LOST note's a (1 = the host said LEAVE) and b (its reason) */
    char lost_text[120];
    /* netflake: session clock (QPC ms, the same in every process of this PC) of the first LOAD_BEGIN / GO note per epoch 1..3 (this bot's own load or the
     * partner's arriving), and the delivery time of reliable events (now - the sender's stamp): how long the reliable control messages took */
    uint32_t q_lb[4], q_go[4];
    uint32_t ev_lat_max, ev_lat_n, ev_lat_over1s;
    double ev_lat_sum;
    uint32_t stream_rx;      /* --event-every: events of the latency stream received (not counted in `events`) */
} Notes;
static Notes notes;
#define BOT_EV_STREAM (CP_EV_USER + 0x70)   /* netflake --event-every: a reliable event stream that only measures delivery times */

/* downed / revive events (docs/protocol.md: CP_EV_USER + 0x60..0x65, body u8 sender slot, u8 target slot or 255) */
enum { BOT_EV_D_DOWN = CP_EV_USER + 0x60, BOT_EV_D_DEAD, BOT_EV_D_BEGIN, BOT_EV_D_KEEPALIVE, BOT_EV_D_CANCEL, BOT_EV_D_DONE };
static const char *const bot_d_names[6] = {"DOWN", "DEAD", "BEGIN", "KEEPALIVE", "CANCEL", "DONE"};
static unsigned d_rx[6];
static int d_my_slot = -1, d_raised;   /* d_raised: a DONE for my slot arrived (the glue of the bot's loop stands it up) */
static int d_partner_ev = -1;          /* the other player's last DOWN / DEAD / up news by event: 1 down, 2 dead, 0 up, -1 none */

static void on_downed_event(const cp_event *e)
{
    unsigned k = e->kind - BOT_EV_D_DOWN;
    int from = e->len >= 1 ? e->body[0] : -1, to = e->len >= 2 ? (e->body[1] == 255 ? -1 : e->body[1]) : -1;
    d_rx[k]++;
    if (k != 3 || d_rx[k] <= 3 || d_rx[k] % 20 == 0)   /* KEEPALIVE every 500 ms: the first ones and every 20th */
        fprintf(stderr, "[%6lu] downed rx %s from slot %d target %d (epoch %u, #%u)\n", (unsigned long)(GetTickCount() - t_start), bot_d_names[k], from, to, e->epoch, d_rx[k]);
    if (k == 5 && to == d_my_slot) d_raised = 1;
    if (from != d_my_slot) {
        if (k == 0) d_partner_ev = 1;
        else if (k == 1) d_partner_ev = 2;
        else if (k == 5 && to == from) d_partner_ev = 0;   /* "I am up" */
    }
}

static void on_note(void *u, const cs_note *n)
{
    (void)u;
    switch (n->what) {
    case CS_N_CONNECTED: notes.connected++; break;
    case CS_N_REFUSED: notes.refused++, notes.refuse_code = n->a, snprintf(notes.refuse_text, sizeof notes.refuse_text, "%s", n->text ? n->text : ""); break;
    case CS_N_LOST: notes.lost++, notes.lost_a = n->a, notes.lost_b = n->b, snprintf(notes.lost_text, sizeof notes.lost_text, "%s", n->text ? n->text : ""); break;
    case CS_N_START: notes.started++; break;
    case CS_N_GO:
        notes.go_count++;
        if (n->a >= 1 && n->a <= 3 && !notes.q_go[n->a]) notes.q_go[n->a] = cn_now_ms() | 1u;
        break;
    case CS_N_LOAD_BEGIN:
        notes.load_begin_count++;
        if (n->a >= 1 && n->a <= 3 && !notes.q_lb[n->a]) notes.q_lb[n->a] = cn_now_ms() | 1u;
        break;
    case CS_N_EVENT:
        if (n->ev) {
            uint32_t lat = cn_now_ms() - n->ev->t_ms;
            if ((int32_t)lat >= 0 && lat < 600000u) {
                notes.ev_lat_n++, notes.ev_lat_sum += lat;
                if (lat > notes.ev_lat_max) notes.ev_lat_max = lat;
                if (lat > 1000) notes.ev_lat_over1s++;
            }
        }
        if (n->ev && n->ev->kind == BOT_EV_STREAM) { notes.stream_rx++; break; }
        notes.events++;
        if (n->ev && n->ev->kind >= BOT_EV_D_DOWN && n->ev->kind <= BOT_EV_D_DONE) on_downed_event(n->ev);
        break;
    case CS_N_NOTICE: notes.notices++; break;
    case CS_N_LOBBY: notes.lobby_changes++; break;
    default: break;
    }
    if (verbose) {
        char b[200];
        snprintf(b, sizeof b, "note %d pid %d a %d b %d %s", n->what, n->pid, n->a, n->b, n->text ? n->text : "");
        logline(b);
    }
}

static cp_state path_state(int pid, uint32_t t_ms)
{
    cp_state s;
    memset(&s, 0, sizeof s);
    double a = (double)t_ms / 1000.0 * (200.0 / 300.0);
    s.pos[0] = (float)(1000.0 * pid + 300.0 * cos(a)), s.pos[1] = 0, s.pos[2] = (float)(300.0 * sin(a));
    double y = fmod(a + 1.5707963, 6.2831853);
    s.yaw = (float)(y > 3.1415927 ? y - 6.2831853 : y);
    s.action = 3, s.hp = 800, s.hpmax = 800, s.weapon = 2;
    s.flags = ((t_ms / 1000) & 1) ? CP_SF_AIMING : 0;
    s.aim = (float)(30.0 * sin(a)), s.aimv[0] = 0.0f, s.aimv[1] = 0.0f, s.aimv[2] = 1.0f;
    return s;
}

static const char *arg_str(int argc, char **argv, const char *name, const char *def)
{
    const char *r = def;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) r = argv[i + 1];   /* the last one wins */
    return r;
}
static double arg_num(int argc, char **argv, const char *name, double def)
{
    const char *v = arg_str(argc, argv, name, NULL);
    return v ? atof(v) : def;
}
static int arg_flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "host") && strcmp(argv[1], "join"))) {
        fprintf(stderr, "usage: coop_bot host [--port N] ... | coop_bot join <ip:port> ... (see the comment at the top of coop_bot.c)\n");
        return 2;
    }
    int host = !strcmp(argv[1], "host");
    verbose = arg_flag(argc, argv, "--verbose");
    t_start = GetTickCount();
    cn_set_log(logline);
    timeBeginPeriod(1);
    cs_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.name = arg_str(argc, argv, "--name", host ? "BotHost" : "BotGuest");
    cfg.want_char = cp_char_parse(arg_str(argc, argv, "--char", host ? "claire" : "none"));
    cfg.mod_version = arg_str(argc, argv, "--mod-version", "bot");
    if (!strcmp(cfg.mod_version, "none")) cfg.mod_version = "";   /* an older host: no version at all (what a 0.1.x host's WELCOME carries) */
    cfg.is_bot = !arg_flag(argc, argv, "--as-player");
    cfg.on_note = on_note;
    const char *tag = arg_str(argc, argv, "--exe-tag", "");
    for (int i = 0; i < 8 && strlen(tag) >= (size_t)(2 * i + 2); i++) {
        char b[3] = {tag[2 * i], tag[2 * i + 1], 0};
        cfg.exe_tag[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    cs_t *s = cs_new(&cfg);
    char err[200];
    int players = (int)arg_num(argc, argv, "--players", 2);
    double play_secs = arg_num(argc, argv, "--seconds", 12);
    int load_ms = (int)arg_num(argc, argv, "--load-ms", 800);
    double load_at = arg_num(argc, argv, "--load-at", -1), event_at = arg_num(argc, argv, "--event-at", -1), leave_at = arg_num(argc, argv, "--leave-at", -1), die_at = arg_num(argc, argv, "--die-at", -1);
    double linger_s = arg_num(argc, argv, "--linger", 0);   /* netflake: after its playing time, stay (no measuring) until the partner has left, at most this long */
    int play_clock = arg_flag(argc, argv, "--play-clock");   /* netflake: --seconds counts only time spent playing while the partner's states are fresh */
    int event_every = (int)arg_num(argc, argv, "--event-every", 0);   /* netflake: a reliable event every N ms while playing (delivery-time statistics only) */
    DWORD stream_t = 0;
    uint32_t stream_tx = 0;
    int start_delay = (int)arg_num(argc, argv, "--autostart-delay", 300);
    int want_pick = !host && cfg.want_char != CP_CHAR_NONE;
    int follow = arg_str(argc, argv, "--follow", NULL) != NULL;
    float follow_d = (float)arg_num(argc, argv, "--follow", 80);
    int bot_hp = (int)arg_num(argc, argv, "--hp", 800);
    double down_at = arg_num(argc, argv, "--down-at", -1), bleed_s = arg_num(argc, argv, "--bleed-s", -1);
    int d_down = 0, d_dead = 0, d_sent_down = 0, d_ups = 0;
    double d_down_t = 0;
    cp_state last_remote;
    int have_remote = 0;
    memset(&last_remote, 0, sizeof last_remote);
    float fpos[3] = {0, 0, 0}, fyaw = 0;   /* --follow: the position it reports */
    double revive_after = arg_num(argc, argv, "--revive-after", -1);
    int revive_ms = (int)arg_num(argc, argv, "--revive-ms", 3000), revive_cancel = (int)arg_num(argc, argv, "--revive-cancel", 0);
    int rv_phase = 0, rv_holds = 0, rv_done = 0, rv_cancels = 0;   /* rv_phase: 0 idle, 1 partner seen lying (waiting revive_after), 2 holding, 3 done (waiting for him to stand) */
    DWORD rv_t = 0, rv_ka = 0;
    const char *ctl = arg_str(argc, argv, "--ctl", NULL);
    char ctl_last[512] = "";
    DWORD ctl_t = 0;
    int ctl_runs = 0, ctl_quit = 0;
    if (host) {
        int port = (int)arg_num(argc, argv, "--port", 28411);
        if (!cs_host(s, "enet", port, 1, err, sizeof err)) {
            fprintf(stderr, "host failed: %s\n", err);
            return 1;
        }
        cp_session ss;
        cs_session_get(s, &ss);
        ss.max_players = (uint8_t)players;
        ss.episode = (uint8_t)arg_num(argc, argv, "--episode", 1);
        cs_set_session(s, &ss);
    } else {
        if (argc < 3 || !cs_join(s, "enet", argv[2], 1, err, sizeof err)) {
            fprintf(stderr, "join failed: %s\n", argc < 3 ? "no address" : err);
            return 1;
        }
    }

    /* lobby-must (0.2.0) lab options of a bot host: --stall-ms N: no session poll for the first N ms (the ENet thread still accepts the connect, so a joining game
     * shows "Awaiting reply from ..."); --kick-at S: S seconds after the other player is in the lobby, remove it (LEAVE REMOVED, its name then refused) */
    int stall_ms = (int)arg_num(argc, argv, "--stall-ms", 0);
    double kick_at = arg_num(argc, argv, "--kick-at", -1);
    DWORD stall_until = GetTickCount() + (DWORD)(stall_ms > 0 ? stall_ms : 0), lobby_t0 = 0;
    int kicked = 0;
    int freeze_load_ms = (int)arg_num(argc, argv, "--freeze-load-ms", 0), leave_load_ms = (int)arg_num(argc, argv, "--leave-load-ms", -1), froze = 0;
    while (host && stall_ms > 0 && (int)(stall_until - GetTickCount()) > 0) Sleep(20);

    enum { W_CONNECT, W_LOBBY, W_LOADING, W_PLAYING, W_DONE } stage = W_CONNECT;
    DWORD stage_t = GetTickCount(), play_t0 = 0, last_state = 0, last_sample = 0, last_status = 0, ready_since = 0;
    int load_done_sent = 0, event_sent = 0, left_early = 0, mid_load_started = 0, in_mid_load = 0;
    DWORD mid_load_t = 0;
    double measure_from = 2.0;
    double err_sum = 0, err_max = 0;
    long err_n = 0, none_n = 0, stale_n = 0, extrap_n = 0;
    int ready_sent = 0, exit_code = 0;
    unsigned max_epoch = 0;
    /* netflake diagnostics (2026-11-08, notes\netflake.md): what an error peak or a lost session coincides with. Times are the session clock (QPC: the same on
     * every process of this PC, so the host's and the client's numbers compare). They describe the run only; no verdict of the bot uses them. */
    uint32_t q_prev = 0, q_tx = 0, q_rx = 0, q_play0 = 0;
    uint32_t loop_max = 0, tx_gap_max = 0, rx_gap_max = 0, rx_gap_jump = 0;
    double loop_max_at = 0, tx_gap_max_at = 0, rx_gap_max_at = 0, err_max_at = 0;
    int err_max_kind = 0;
    uint32_t d_rx_state = 0, d_rx_gap = 0;
    /* netflake (2026-11-08, notes\netflake.md): a sample counts only once the partner's stream is known to have gone on past the sample's time. An extrapolated
     * sample (the shown time is past the newest state) waits here until an interpolated one shows that a newer state arrived; when the stretch of play ends
     * first (the partner's scripted level load or end reached this bot late, its LOAD_BEGIN / LEAVE delayed by the loss), its samples are dropped: they measured
     * the puppet of a partner that had stopped walking against the circle it would have walked. Mid-stream outages stay measured in full (up to STALE).
     * raw_*: every sample, the measure before netflake (printed, no verdict); tail_*: the samples dropped that way. */
    enum { PEND_MAX = 256 };
    struct { uint32_t rt; double e, t; int kind; } pend[PEND_MAX];
    int pend_n = 0, lingering = 0, seen_rp = -1;
    uint32_t q_linger = 0;   /* when it began to stay after its playing time */
    uint32_t played_ms = 0;  /* --play-clock: time in W_PLAYING with the partner's states fresh (its last sample interpolated or extrapolated) */
    int partner_fresh = 0;
    uint32_t paused_ms = 0;  /* --play-clock: time in W_PLAYING not counted; after 20 s of it the clock runs regardless (the run stays bounded) */
    cs_player_info seen_pi;
    memset(&seen_pi, 0, sizeof seen_pi);
    long raw_n = 0, tail_n = 0;
    double raw_sum = 0, raw_max = 0, tail_max = 0;
#define COUNT_SAMPLE(E, T, K) do { err_sum += (E), err_n++; if ((E) > err_max) err_max = (E), err_max_at = (T), err_max_kind = (K); } while (0)

    while (stage != W_DONE) {
        if (stage != W_PLAYING && pend_n) {   /* the stretch of play ended: the partner's stream was not seen to go on past these */
            for (int i = 0; i < pend_n; i++) { tail_n++; if (pend[i].e > tail_max) tail_max = pend[i].e; }
            pend_n = 0;
        }
        cs_poll(s);
        DWORD now = GetTickCount();
        uint32_t qn = cs_now(s);
        if (stage == W_PLAYING) {   /* netflake: main-loop stalls and arrival gaps of the partner's states inside one stretch of play */
            double tq = (now - play_t0) / 1000.0;
            cs_stats dst;
            if (q_prev) {
                if (partner_fresh || paused_ms >= 20000) played_ms += qn - q_prev;
                else paused_ms += qn - q_prev;
            }
            if (q_prev && qn - q_prev > loop_max) loop_max = qn - q_prev, loop_max_at = tq;
            q_prev = qn;
            cs_get_stats(s, &dst);
            if (dst.rx_state != d_rx_state) {
                if (q_rx && qn - q_rx > rx_gap_max) rx_gap_max = qn - q_rx, rx_gap_jump = dst.rx_gap - d_rx_gap, rx_gap_max_at = tq;
                q_rx = qn;
            }
            d_rx_state = dst.rx_state, d_rx_gap = dst.rx_gap;
        } else
            q_prev = 0, q_rx = 0, q_tx = 0;
        if (notes.refused) {
            printf("RESULT {\"refused\":%d,\"code\":%d,\"text\":\"%s\"}\n", notes.refused, notes.refuse_code, notes.refuse_text);
            exit_code = 3;
            break;
        }
        if (notes.lost && stage != W_CONNECT) {
            printf("RESULT {\"lost\":1,\"stage\":%d,\"t\":%.3f,\"left\":%d,\"reason\":%d,\"why\":\"%s\",\"q_play0\":%u,\"q_end\":%u}\n", (int)stage,
                   play_t0 ? (now - play_t0) / 1000.0 : -1.0, notes.lost_a, notes.lost_b, notes.lost_text, q_play0, qn);
            exit_code = 4;
            break;
        }
        if (verbose && now - last_status > 2000) {
            last_status = now;
            char st[1500];
            cs_status(s, st, sizeof st);
            logline(st);
        }
        if (cs_epoch(s) > max_epoch) max_epoch = cs_epoch(s);
        switch (stage) {
        case W_CONNECT:
            if (host ? cs_player_count(s) >= players : notes.connected) stage = W_LOBBY, stage_t = now;
            else if (now - stage_t > 60000) { printf("RESULT {\"error\":\"nobody connected\"}\n"); exit_code = 5; stage = W_DONE; }
            break;
        case W_LOBBY:
            if (host && kick_at >= 0 && !kicked) {
                if (!lobby_t0) lobby_t0 = now;
                if ((double)(now - lobby_t0) >= kick_at * 1000.0 && cs_remote_pid(s) >= 0) {
                    kicked = cs_kick(s, cs_remote_pid(s), CP_LEAVE_REMOVED);
                    fprintf(stderr, "[%6lu] kick: the other player removed (%d)\n", (unsigned long)(now - t_start), kicked);
                }
            }
            if (!host && want_pick && cs_local_character(s) != cfg.want_char) {
                if (now - stage_t > 200) { cs_pick(s, cfg.want_char); stage_t = now; }
            } else if (!ready_sent && !arg_flag(argc, argv, "--no-ready")) {
                cs_ready(s, 1);
                ready_sent = 1;
                ready_since = now;
            }
            if (host && ready_sent && now - ready_since > (DWORD)start_delay) {
                if (cs_start(s, 0, err, sizeof err)) logline("start sent");
            }
            if (notes.started) stage = W_LOADING, stage_t = now;
            break;
        case W_LOADING:
            if (freeze_load_ms > 0 && !froze) {   /* gate020: a game that stalls in its load (the ENet thread still answers) */
                froze = 1;
                fprintf(stderr, "[%6lu] freeze: no session poll for %d ms (loading)\n", (unsigned long)(now - t_start), freeze_load_ms);
                Sleep((DWORD)freeze_load_ms);
                fprintf(stderr, "[%6lu] freeze: over\n", (unsigned long)(GetTickCount() - t_start));
                break;
            }
            if (leave_load_ms >= 0 && (int)(now - stage_t) >= leave_load_ms) {   /* gate020: quits while loading (cs_free sends the LEAVE) */
                fprintf(stderr, "[%6lu] leave: leaving the session %d ms into the load\n", (unsigned long)(now - t_start), (int)(now - stage_t));
                left_early = 1;
                stage = W_DONE;
                break;
            }
            if (!load_done_sent && (int)(now - stage_t) >= load_ms) {
                cs_load_done(s);
                load_done_sent = 1;
            }
            if (in_mid_load && (int)(now - mid_load_t) >= load_ms) {
                cs_load_done(s);
                in_mid_load = 0;
            }
            if (cs_phase(s) == CP_PHASE_PLAYING) {
                stage = W_PLAYING;
                if (!play_t0) play_t0 = now, q_play0 = qn;
                measure_from = (now - play_t0) / 1000.0 + 2.0;
            }
            break;
        case W_PLAYING: {
            double t = (now - play_t0) / 1000.0;
            if (d_my_slot < 0 && cs_local_character(s) >= 0 && cs_local_character(s) <= 3) d_my_slot = CP_CHAR_SLOT(cs_local_character(s));
            if (ctl && now - ctl_t >= 500) {   /* the lab script's switches (--ctl) */
                ctl_t = now;
                char buf[512] = "";
                FILE *f = fopen(ctl, "rb");
                if (f) {
                    size_t got = fread(buf, 1, sizeof buf - 1, f);
                    buf[got] = 0;
                    fclose(f);
                }
                if (buf[0] && strcmp(buf, ctl_last)) {
                    snprintf(ctl_last, sizeof ctl_last, "%s", buf);
                    ctl_runs++;
                    const unsigned char me_body[2] = {(unsigned char)(d_my_slot < 0 ? 1 : d_my_slot), (unsigned char)(d_my_slot < 0 ? 1 : d_my_slot)};
                    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
                        if (line[0] == '#' || !line[0]) continue;
                        fprintf(stderr, "[%6lu] ctl: %s\n", (unsigned long)(GetTickCount() - t_start), line);
                        if (!strcmp(line, "revive on")) { if (revive_after < 0) revive_after = 3; rv_phase = 0; }
                        else if (!strcmp(line, "revive off")) revive_after = -1, rv_phase = 0;
                        else if (!strcmp(line, "cancel on")) revive_cancel = 1, rv_cancels = 0;
                        else if (!strcmp(line, "cancel off")) revive_cancel = 0;
                        else if (!strcmp(line, "down")) { if (!d_down) down_at = t, d_sent_down = 0, d_dead = 0; }   /* the go-down block below fires this pass */
                        else if (!strcmp(line, "up")) { if (d_down || d_dead) { d_down = d_dead = 0, d_ups++; cs_send_event(s, BOT_EV_D_DONE, me_body, 2); } }
                        else if (!strcmp(line, "dead")) { if (!d_dead) { const unsigned char b2[2] = {me_body[0], 255}; cs_send_event(s, BOT_EV_D_DEAD, b2, 2); d_down = 0, d_dead = 1; } }
                        else if (!strcmp(line, "quit")) ctl_quit = 1;
                    }
                }
            }
            if (ctl_quit) {
                stage = W_DONE;
                break;
            }
            if (!d_down) d_raised = 0;   /* a DONE only counts while lying */
            if (down_at >= 0 && !d_down && !d_dead && !d_sent_down && t >= down_at) {   /* the partner "goes down" (run B: the game's human is the one who can raise it) */
                const unsigned char body[2] = {(unsigned char)(d_my_slot < 0 ? 1 : d_my_slot), 255};
                cs_send_event(s, BOT_EV_D_DOWN, body, 2);
                d_down = d_sent_down = 1, d_down_t = t;
                fprintf(stderr, "[%6lu] downed: the bot (slot %d) goes DOWN at %.1f s\n", (unsigned long)(GetTickCount() - t_start), d_my_slot, t);
            }
            if (d_down && d_raised) {
                d_down = 0, d_raised = 0, d_ups++;
                fprintf(stderr, "[%6lu] downed: raised by the partner (DONE for my slot), standing up\n", (unsigned long)(GetTickCount() - t_start));
            }
            if (d_down && bleed_s >= 0 && t - d_down_t >= bleed_s) {
                const unsigned char body[2] = {(unsigned char)(d_my_slot < 0 ? 1 : d_my_slot), 255};
                cs_send_event(s, BOT_EV_D_DEAD, body, 2);
                d_down = 0, d_dead = 1;
                fprintf(stderr, "[%6lu] downed: the bot bled out (DEAD) after %.1f s\n", (unsigned long)(GetTickCount() - t_start), t - d_down_t);
            }
            if (revive_after >= 0 && !d_down && !d_dead) {   /* the bot as the rescuer of the other (real) player */
                int pdown = d_partner_ev == 1 || (d_partner_ev < 0 && have_remote && (last_remote.flags & CP_SF_DOWN));
                if (d_partner_ev == 1 && have_remote && !(last_remote.flags & CP_SF_DOWN) && rv_phase == 3) pdown = 0;   /* after my DONE his STATE says up */
                unsigned char body[2] = {(unsigned char)(d_my_slot < 0 ? 1 : d_my_slot), (unsigned char)(d_my_slot == 0 ? 1 : 0)};
                if (!pdown) {
                    if (rv_phase == 2) {
                        cs_send_event(s, BOT_EV_D_CANCEL, body, 2);
                        fprintf(stderr, "[%6lu] downed: the partner is not lying any more: hold CANCELLED\n", (unsigned long)(GetTickCount() - t_start));
                    }
                    if (rv_phase == 3) d_partner_ev = -1;   /* the next DOWN starts afresh */
                    rv_phase = 0;
                } else if (rv_phase == 0) {
                    rv_phase = 1, rv_t = now;
                    fprintf(stderr, "[%6lu] downed: the partner lies, the bot holds the button in %.1f s\n", (unsigned long)(GetTickCount() - t_start), revive_after);
                } else if (rv_phase == 1 && (double)(now - rv_t) >= revive_after * 1000.0) {
                    cs_send_event(s, BOT_EV_D_BEGIN, body, 2);
                    rv_phase = 2, rv_t = rv_ka = now, rv_holds++;
                    fprintf(stderr, "[%6lu] downed: BEGIN (hold %d on slot %d, %d ms)\n", (unsigned long)(GetTickCount() - t_start), rv_holds, body[1], revive_ms);
                } else if (rv_phase == 2) {
                    if (revive_cancel && rv_cancels == 0 && (int)(now - rv_t) >= revive_ms / 2) {
                        cs_send_event(s, BOT_EV_D_CANCEL, body, 2);
                        rv_cancels++, rv_phase = 1, rv_t = now;   /* a new hold after revive_after again */
                        fprintf(stderr, "[%6lu] downed: hold broken off half-way (CANCEL)\n", (unsigned long)(GetTickCount() - t_start));
                    } else if ((int)(now - rv_t) >= revive_ms) {
                        cs_send_event(s, BOT_EV_D_DONE, body, 2);
                        rv_phase = 3, rv_done++;
                        fprintf(stderr, "[%6lu] downed: DONE (raised slot %d)\n", (unsigned long)(GetTickCount() - t_start), body[1]);
                    } else if (now - rv_ka >= 500) {
                        cs_send_event(s, BOT_EV_D_KEEPALIVE, body, 2);
                        rv_ka = now;
                    }
                }
            }
            if (now - last_state >= 50) {
                last_state = now;
                cp_state st = path_state(cs_pid(s), cs_now(s));
                int send = 1;
                if (follow) {   /* idle beside the other player; while lying (or dead) it stays where it fell */
                    send = have_remote;
                    if (have_remote && !d_down && !d_dead) fpos[0] = last_remote.pos[0] + follow_d, fpos[1] = last_remote.pos[1], fpos[2] = last_remote.pos[2], fyaw = last_remote.yaw;
                    memset(&st, 0, sizeof st);
                    st.pos[0] = fpos[0], st.pos[1] = fpos[1], st.pos[2] = fpos[2], st.yaw = fyaw;
                    st.action = 2, st.hp = (int16_t)bot_hp, st.hpmax = (int16_t)bot_hp;
                    st.aimv[2] = 1.0f;
                }
                if (d_down) st.flags |= CP_SF_DOWN, st.hp = 1, st.action = 0x7A;
                if (d_dead) st.flags |= CP_SF_DEAD, st.hp = 0;
                if (send && cs_submit_state(s, &st)) {
                    if (q_tx && qn - q_tx > tx_gap_max) tx_gap_max = qn - q_tx, tx_gap_max_at = t;
                    q_tx = qn;
                }
            }
            if (now - last_sample >= 16) {
                last_sample = now;
                int rp = cs_remote_pid(s);
                cp_state out;
                if (rp >= 0 && cs_player(s, rp, &seen_pi)) seen_rp = rp;
                if (rp < 0) partner_fresh = 1;   /* the partner has gone: nothing to wait for, the clock runs */
                if (rp >= 0) {
                    int r = cs_remote_sample(s, rp, cs_now(s), &out);
                    partner_fresh = r == CS_SAMPLE_INTERP || r == CS_SAMPLE_EXTRAP;
                    if (r == CS_SAMPLE_NONE) none_n++;
                    else {
                        last_remote = out, have_remote = 1;
                        if (r == CS_SAMPLE_STALE) stale_n++;
                        if (r == CS_SAMPLE_EXTRAP) extrap_n++;
                        if (r == CS_SAMPLE_INTERP && pend_n) {   /* a state newer than out.t_ms is in the ring: the stream went on past every earlier pending sample */
                            int k = 0;
                            for (int i = 0; i < pend_n; i++) {
                                if ((int32_t)(pend[i].rt - out.t_ms) <= 0) COUNT_SAMPLE(pend[i].e, pend[i].t, pend[i].kind);
                                else pend[k++] = pend[i];
                            }
                            pend_n = k;
                        }
                        if (t > measure_from && r != CS_SAMPLE_STALE && !follow && !lingering) {   /* the path error only means something against another bot */
                            cp_state truth = path_state(rp, out.t_ms);
                            double dx = out.pos[0] - truth.pos[0], dz = out.pos[2] - truth.pos[2], e = sqrt(dx * dx + dz * dz);
                            raw_sum += e, raw_n++;
                            if (e > raw_max) raw_max = e;
                            if (r == CS_SAMPLE_EXTRAP && pend_n < PEND_MAX) pend[pend_n].rt = out.t_ms, pend[pend_n].e = e, pend[pend_n].t = t, pend[pend_n].kind = r, pend_n++;
                            else COUNT_SAMPLE(e, t, r);
                        }
                    }
                }
            }
            if (load_at >= 0 && !mid_load_started && t >= load_at) {   /* this bot's "game" starts loading the next level */
                mid_load_started = in_mid_load = 1;
                mid_load_t = now;
                cs_load_begin(s, 777);
                logline("mid-game load begins");
                stage = W_LOADING, stage_t = now;
                break;
            }
            if (cs_phase(s) == CP_PHASE_LOADING && !in_mid_load) {   /* somebody else started a load: this bot's game loads too */
                in_mid_load = 1;
                mid_load_t = now;
                cs_load_begin(s, 777);
                stage = W_LOADING, stage_t = now;
                break;
            }
            if (event_every > 0 && !lingering && now - stream_t >= (DWORD)event_every) {
                const char body[] = {1};
                stream_t = now;
                if (cs_send_event(s, BOT_EV_STREAM, body, sizeof body)) stream_tx++;
            }
            if (event_at >= 0 && !event_sent && t >= event_at) {
                const char body[] = {9, 8, 7, 6};
                cs_send_event(s, CP_EV_PICKUP, body, sizeof body);
                event_sent = 1;
            }
            if (leave_at >= 0 && t >= leave_at) {
                left_early = 1;
                stage = W_DONE;
            }
            if (die_at >= 0 && t >= die_at) ExitProcess(0);   /* no goodbye at all: the peers must notice by themselves */
            if ((play_clock ? played_ms / 1000.0 : t) >= play_secs) {
                /* netflake --linger: the host of the suite stays until its guest has left. Both play clocks start at their own GO, and a GO that the loss delays
                 * starts the guest late (harsh: two retransmissions, > 1 s); a host that left on its own clock then ended the guest's run as "lost".
                 * Every bot also stays up to 1.5 s while samples of its last moments still wait for the partner's next state (an outage at the end of its own
                 * playing time is measured, not dropped); no new samples are taken meanwhile. With --play-clock the playing time counts only while the
                 * partner's states are fresh: a level load, a barrier wait of either side (a LOADED / GO / LOAD_BEGIN delayed by ENet's doubling retries
                 * under harsh loss took 5-9 s) or a dead link makes the run longer instead of cutting the observed play short; at most 20 s are left
                 * out, and none once the partner has gone. */
                int partner = cs_remote_pid(s) >= 0;
                if (!lingering) q_linger = qn;
                double stayed = (qn - q_linger) / 1000.0;
                if (partner && ((linger_s > 0 && stayed < linger_s) || (pend_n && stayed < 1.5))) lingering = 1;
                else stage = W_DONE;
            }
            break;
        }
        default:
            break;
        }
        Sleep(3);
    }
    for (int i = 0; i < pend_n; i++) { tail_n++; if (pend[i].e > tail_max) tail_max = pend[i].e; }   /* the end of the run is the end of a stretch too */
    pend_n = 0;
#undef COUNT_SAMPLE

    if (exit_code == 0) {
        cs_stats st;
        cs_get_stats(s, &st);
        cs_player_info pi = {0};
        int rp = cs_remote_pid(s);
        if (rp >= 0) cs_player(s, rp, &pi);
        else if (seen_rp >= 0) rp = seen_rp, pi = seen_pi;   /* --linger: the partner has left by now; its numbers as last seen */
        printf("RESULT {\"role\":\"%s\",\"pid\":%d,\"character\":\"%s\",\"remote_character\":\"%s\",\"phase\":%d,\"epoch\":%u,\"max_epoch\":%u,\"go\":%d,\"started\":%d,"
               "\"tx_state\":%u,\"rx_state\":%u,\"dup\":%u,\"old\":%u,\"gap\":%u,\"stale_epoch\":%u,\"err_mean\":%.3f,\"err_max\":%.3f,\"err_n\":%ld,\"none\":%ld,\"stale\":%ld,\"extrap\":%ld,"
               "\"events\":%d,\"rtt\":%d,\"left_early\":%d,\"bad\":%u,\"hb\":%u,\"downed\":{\"went_down\":%d,\"dead\":%d,\"raised\":%d,\"rx_down\":%u,\"rx_dead\":%u,\"rx_begin\":%u,\"rx_keepalive\":%u,\"rx_cancel\":%u,\"rx_done\":%u,\"holds\":%d,\"cancels\":%d,\"revives\":%d,\"ctl_runs\":%d},"
               "\"diag\":{\"q_play0\":%u,\"q_end\":%u,\"loop_max\":%u,\"loop_max_at\":%.2f,\"tx_gap_max\":%u,\"tx_gap_max_at\":%.2f,\"rx_gap_max\":%u,\"rx_gap_jump\":%u,"
               "\"rx_gap_max_at\":%.2f,\"err_max_at\":%.2f,\"err_max_kind\":%d,\"raw_n\":%ld,\"raw_mean\":%.3f,\"raw_max\":%.3f,\"tail_n\":%ld,\"tail_max\":%.3f,\"lingered_ms\":%u,\"paused_ms\":%u,"
               "\"q_lb\":[%u,%u,%u],\"q_go\":[%u,%u,%u],\"ev_lat_n\":%u,\"ev_lat_mean\":%.1f,\"ev_lat_max\":%u,\"ev_lat_over1s\":%u,\"stream_tx\":%u,\"stream_rx\":%u}}\n",
               host ? "host" : "client", cs_pid(s), cp_char_name((unsigned)cs_local_character(s)), rp >= 0 ? cp_char_name(pi.character) : "none", cs_phase(s), cs_epoch(s), max_epoch,
               notes.go_count, notes.started, st.tx_state, st.rx_state, st.rx_dup, st.rx_old, st.rx_gap, st.rx_stale_epoch, err_n ? err_sum / (double)err_n : -1.0, err_max, err_n, none_n, stale_n,
               extrap_n, notes.events, pi.rtt_ms, left_early, st.rx_bad, st.hb_rx, d_sent_down, d_dead, d_ups, d_rx[0], d_rx[1], d_rx[2], d_rx[3], d_rx[4], d_rx[5], rv_holds, rv_cancels, rv_done, ctl_runs,
               q_play0, cs_now(s), loop_max, loop_max_at, tx_gap_max, tx_gap_max_at, rx_gap_max, rx_gap_jump, rx_gap_max_at, err_max_at, err_max_kind,
               raw_n, raw_n ? raw_sum / (double)raw_n : -1.0, raw_max, tail_n, tail_max, lingering ? cs_now(s) - q_linger : 0u, paused_ms,
               notes.q_lb[1], notes.q_lb[2], notes.q_lb[3], notes.q_go[1], notes.q_go[2], notes.q_go[3], notes.ev_lat_n, notes.ev_lat_n ? notes.ev_lat_sum / notes.ev_lat_n : -1.0,
               notes.ev_lat_max, notes.ev_lat_over1s, stream_tx, notes.stream_rx);
    }
    fflush(stdout);
    cs_free(s);
    return exit_code;
}
