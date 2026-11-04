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
} Notes;
static Notes notes;

static void on_note(void *u, const cs_note *n)
{
    (void)u;
    switch (n->what) {
    case CS_N_CONNECTED: notes.connected++; break;
    case CS_N_REFUSED: notes.refused++, notes.refuse_code = n->a, snprintf(notes.refuse_text, sizeof notes.refuse_text, "%s", n->text ? n->text : ""); break;
    case CS_N_LOST: notes.lost++; break;
    case CS_N_START: notes.started++; break;
    case CS_N_GO: notes.go_count++; break;
    case CS_N_LOAD_BEGIN: notes.load_begin_count++; break;
    case CS_N_EVENT: notes.events++; break;
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
    cfg.mod_version = "bot";
    cfg.is_bot = 1;
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
    int start_delay = (int)arg_num(argc, argv, "--autostart-delay", 300);
    int want_pick = !host && cfg.want_char != CP_CHAR_NONE;
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

    enum { W_CONNECT, W_LOBBY, W_LOADING, W_PLAYING, W_DONE } stage = W_CONNECT;
    DWORD stage_t = GetTickCount(), play_t0 = 0, last_state = 0, last_sample = 0, last_status = 0, ready_since = 0;
    int load_done_sent = 0, event_sent = 0, left_early = 0, mid_load_started = 0, in_mid_load = 0;
    DWORD mid_load_t = 0;
    double measure_from = 2.0;
    double err_sum = 0, err_max = 0;
    long err_n = 0, none_n = 0, stale_n = 0, extrap_n = 0;
    int ready_sent = 0, exit_code = 0;
    unsigned max_epoch = 0;

    while (stage != W_DONE) {
        cs_poll(s);
        DWORD now = GetTickCount();
        if (notes.refused) {
            printf("RESULT {\"refused\":%d,\"code\":%d,\"text\":\"%s\"}\n", notes.refused, notes.refuse_code, notes.refuse_text);
            exit_code = 3;
            break;
        }
        if (notes.lost && stage != W_CONNECT) {
            printf("RESULT {\"lost\":1,\"stage\":%d}\n", (int)stage);
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
                if (!play_t0) play_t0 = now;
                measure_from = (now - play_t0) / 1000.0 + 2.0;
            }
            break;
        case W_PLAYING: {
            double t = (now - play_t0) / 1000.0;
            if (now - last_state >= 50) {
                last_state = now;
                cp_state st = path_state(cs_pid(s), cs_now(s));
                cs_submit_state(s, &st);
            }
            if (now - last_sample >= 16) {
                last_sample = now;
                int rp = cs_remote_pid(s);
                cp_state out;
                if (rp >= 0) {
                    int r = cs_remote_sample(s, rp, cs_now(s), &out);
                    if (r == CS_SAMPLE_NONE) none_n++;
                    else {
                        if (r == CS_SAMPLE_STALE) stale_n++;
                        if (r == CS_SAMPLE_EXTRAP) extrap_n++;
                        if (t > measure_from && r != CS_SAMPLE_STALE) {
                            cp_state truth = path_state(rp, out.t_ms);
                            double dx = out.pos[0] - truth.pos[0], dz = out.pos[2] - truth.pos[2], e = sqrt(dx * dx + dz * dz);
                            err_sum += e, err_n++;
                            if (e > err_max) err_max = e;
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
            if (t >= play_secs) stage = W_DONE;
            break;
        }
        default:
            break;
        }
        Sleep(3);
    }

    if (exit_code == 0) {
        cs_stats st;
        cs_get_stats(s, &st);
        cs_player_info pi = {0};
        int rp = cs_remote_pid(s);
        if (rp >= 0) cs_player(s, rp, &pi);
        printf("RESULT {\"role\":\"%s\",\"pid\":%d,\"character\":\"%s\",\"remote_character\":\"%s\",\"phase\":%d,\"epoch\":%u,\"max_epoch\":%u,\"go\":%d,\"started\":%d,"
               "\"tx_state\":%u,\"rx_state\":%u,\"dup\":%u,\"old\":%u,\"gap\":%u,\"stale_epoch\":%u,\"err_mean\":%.3f,\"err_max\":%.3f,\"err_n\":%ld,\"none\":%ld,\"stale\":%ld,\"extrap\":%ld,"
               "\"events\":%d,\"rtt\":%d,\"left_early\":%d,\"bad\":%u,\"hb\":%u}\n",
               host ? "host" : "client", cs_pid(s), cp_char_name((unsigned)cs_local_character(s)), rp >= 0 ? cp_char_name(pi.character) : "none", cs_phase(s), cs_epoch(s), max_epoch,
               notes.go_count, notes.started, st.tx_state, st.rx_state, st.rx_dup, st.rx_old, st.rx_gap, st.rx_stale_epoch, err_n ? err_sum / (double)err_n : -1.0, err_max, err_n, none_n, stale_n,
               extrap_n, notes.events, pi.rtt_ms, left_early, st.rx_bad, st.hb_rx);
    }
    fflush(stdout);
    cs_free(s);
    return exit_code;
}
