/*
 * Co-op lobby model (see lobby.h). One instance lives while the menu entry "Co-Op" is open. All state is polled from the session object
 * (coop_session.h), so a session that was started elsewhere (a Steam "Join game" click, the test console) shows up in the room at once.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lobby.h"

enum {
    A_NONE = 0, A_HOST, A_JOIN, A_BACK, A_FRIEND, A_LINK, A_REFRESH, A_EPISODE, A_DIFF, A_DOWNED, A_READY, A_START, A_LEAVE, A_YES, A_NO, A_INVITE, A_KICK,
    A_CONT,             /* 0.2.1: the host's Continue (the checkpoint of its co-op save) */
    A_CHAR = 20,        /* + CP_CHAR_* (20..23) */
    A_FRIEND_ENTRY = 40 /* + index */
};
enum { JOB_NONE = 0, JOB_HOST, JOB_JOIN, JOB_FRIENDS, JOB_NAME };

#define MAX_FR 10

struct lobby {
    lobby_env env;
    lobby_settings set;
    int open, screen, sel, request, way;     /* way: 0 Steam friends, 1 direct IP (the way of the current host / join attempt) */
    char status[240];
    int status_kind;
    uint32_t status_until;                    /* 0 = stays */
    char input[96];
    int job, hosting;                         /* hosting: the job is a host job */
    int cancelled;                            /* the player left the connecting screen while the job still ran: close the session when it ends */
    uint32_t t_job, t_enter;
    int started;
    int partner_seen, partner_joined_until;
    uint32_t last_auto_pick;
    char applied_name[CP_NAME_MAX + 1];
    int name_probed;                          /* the Steam persona name was asked for once (only when the settings hold no name) */
    struct { char label[96]; char cmd[48]; } fr[MAX_FR];
    int nfr, fr_done;
    char note_text[160];
    int note_what, note_new, note_a, note_b, note_pid;
    char target[64];                          /* what the guest connects to (the address, or the friend's name): "Connecting to <target>..." */
    char addr[64];
    int prev_ready_partner;
    char ask_from;                            /* the room the leave question came from */
    int cont;                                 /* 0.2.1: the host chose Continue (not kept in the settings file: a new room starts with a new game) */
};

static const char *tx(lobby_t *l, int id) { return l->env.text ? l->env.text(l->env.user, id) : ""; }
static cs_t *sess(lobby_t *l) { return l->env.session ? l->env.session(l->env.user) : NULL; }
static uint32_t now(lobby_t *l) { return l->env.now ? l->env.now(l->env.user) : 0; }

static void set_status(lobby_t *l, int kind, uint32_t ms, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->status, sizeof l->status, fmt, ap);
    va_end(ap);
    l->status_kind = kind;
    l->status_until = ms ? now(l) + ms : 0;
    if (ms && !l->status_until) l->status_until = 1;
}

static void clear_status(lobby_t *l) { l->status[0] = 0, l->status_kind = LSK_INFO, l->status_until = 0; }

static int start_job(lobby_t *l, int kind, const char *line);

lobby_t *lobby_new(const lobby_env *env)
{
    lobby_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->env = *env;
    l->set.character = CP_CHAR_CLAIRE, l->set.port = 28410, l->set.episode = 1, l->set.difficulty = 1;
    return l;
}

void lobby_free(lobby_t *l) { free(l); }

void lobby_set_settings(lobby_t *l, const lobby_settings *s)
{
    l->set = *s;
    if (l->set.character < 0 || l->set.character > 3) l->set.character = CP_CHAR_CLAIRE;
    if (l->set.port <= 0 || l->set.port > 65535) l->set.port = 28410;
    if (l->set.episode < 1 || l->set.episode > 4 || !l->set.episodes_all) l->set.episode = 1;   /* 0.2.0: Episode 1 only (the lab's REV2_EPISODES=all keeps 1-4) */
    if (l->set.difficulty < 0 || l->set.difficulty > 3) l->set.difficulty = 1;
    l->set.sflags &= 7;
}

void lobby_get_settings(lobby_t *l, lobby_settings *s) { *s = l->set; }

static void save(lobby_t *l)
{
    if (l->env.save) l->env.save(l->env.user, &l->set);
}

static int run(lobby_t *l, char *out, int size, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    out[0] = 0;
    return l->env.command ? l->env.command(l->env.user, line, out, size) : 0;
}

static const char *char_name(lobby_t *l, int c)
{
    switch (c) {
    case CP_CHAR_CLAIRE: return tx(l, LT_CLAIRE);
    case CP_CHAR_MOIRA: return tx(l, LT_MOIRA);
    case CP_CHAR_BARRY: return tx(l, LT_BARRY);
    case CP_CHAR_NATALIA: return tx(l, LT_NATALIA);
    default: return "-";
    }
}
static const char *const char_words[4] = {"claire", "moira", "barry", "natalia"};

static const char *diff_name(lobby_t *l, int d)
{
    switch (d) {
    case 0: return tx(l, LT_CASUAL);
    case 1: return tx(l, LT_NORMAL);
    case 2: return tx(l, LT_HARD);
    default: return tx(l, LT_VERY_HARD);
    }
}

/* ------------------------------------------------------------------ session facts */
typedef struct {
    int active, host, phase, pid, count, connected, steam;   /* steam: the session runs on the Steam backend */
    cp_session ss;
    cs_player_info pl[CP_MAX_PLAYERS];
    int has[CP_MAX_PLAYERS];
    int other;            /* pid of the first other connected player or -1 */
} facts;

static void get_facts(lobby_t *l, facts *f)
{
    memset(f, 0, sizeof *f);
    f->other = -1, f->pid = -1;
    cs_t *s = sess(l);
    if (!s) return;
    f->active = cs_active(s);
    if (!f->active) return;
    f->host = cs_is_host(s), f->phase = cs_phase(s), f->pid = cs_pid(s);
    f->steam = cs_transport(s) && !strcmp(cn_backend_name(cs_transport(s)), "steam");
    cs_session_get(s, &f->ss);
    for (int i = 0; i < CP_MAX_PLAYERS; i++) {
        f->has[i] = cs_player(s, i, &f->pl[i]);
        if (f->has[i] && f->pl[i].connected) {
            f->count++;
            if (i != f->pid && f->other < 0) f->other = i;
        }
    }
    f->connected = f->pid >= 0 && f->has[f->pid];
}

/* a player's name as shown: the default names "Player 1" / "Player 2" in the player's language */
static const char *shown_name(lobby_t *l, const char *wire, char *out, size_t size) { return lobby_display_name(wire, tx(l, LT_PLAYER), out, size); }

/* 0.2.0: a session of another episode than 1 is not played (the lab's REV2_EPISODES=all is) */
static int episode_ok(lobby_t *l, const facts *f) { return l->set.episodes_all || !f->active || f->ss.episode == 0 || f->ss.episode == 1; }

static int slot_taken_by_partner(const facts *f, int character)
{
    if (f->other < 0) return 0;
    int c = f->pl[f->other].character;
    return c <= 3 && CP_CHAR_SLOT(c) == CP_CHAR_SLOT(character);
}

/* ------------------------------------------------------------------ screens */
static void goto_screen(lobby_t *l, int screen)
{
    l->screen = screen;
    l->sel = 0;
    l->t_enter = now(l);
}

static void enter_room(lobby_t *l)
{
    goto_screen(l, LS_ROOM);
    l->started = 0;
    l->cont = 0;
    l->partner_seen = 0;
    l->job = JOB_NONE;
    clear_status(l);
    l->sel = -1;   /* the first enabled row is picked by the view */
}

void lobby_open(lobby_t *l)
{
    l->open = 1;
    l->request = LR_NONE;
    l->started = 0;
    clear_status(l);
    if (l->env.local_address) l->env.local_address(l->env.user, l->addr, sizeof l->addr);
    facts f;
    get_facts(l, &f);
    if (f.active && f.connected) {
        enter_room(l);
        if (f.phase != CP_PHASE_LOBBY) l->started = 0;   /* update() will ask to start at once */
    } else {
        goto_screen(l, LS_TOP);
        if (f.active) {   /* a join attempt that is still running (invite clicked a moment ago) */
            goto_screen(l, LS_CONNECTING);
            l->t_job = now(l);
            set_status(l, LSK_INFO, 0, "%s", tx(l, LT_CONNECTING));
        }
    }
    /* the player name: applied to the session object while no session exists; without one the Steam persona name is asked for once */
    if (!f.active && l->set.name[0] && strcmp(l->set.name, l->applied_name)) {
        char out[120];
        run(l, out, sizeof out, "name %s", l->set.name);
        snprintf(l->applied_name, sizeof l->applied_name, "%s", l->set.name);
    } else if (!f.active && !l->set.name[0] && !l->name_probed && l->screen == LS_TOP) {
        l->name_probed = 1;
        start_job(l, JOB_NAME, "steam");
    }
}

void lobby_close(lobby_t *l)
{
    l->open = 0;
    l->job = JOB_NONE;
}

int lobby_take_request(lobby_t *l)
{
    int r = l->request;
    l->request = LR_NONE;
    return r;
}

int lobby_leave_ask_active(lobby_t *l) { return l->open && l->screen == LS_LEAVE_ASK; }

void lobby_start_params(lobby_t *l, int *episode, int *part, int *difficulty, int *character, int *game_type)
{
    facts f;
    get_facts(l, &f);
    if (episode) *episode = f.ss.episode ? f.ss.episode : l->set.episode;
    if (part) *part = f.ss.part;
    if (difficulty) *difficulty = f.ss.difficulty;
    if (character) *character = f.active ? cs_local_character(sess(l)) : CP_CHAR_NONE;
    if (game_type) *game_type = f.ss.game_type;
}

int lobby_session_running(cs_t *s)
{
    if (!s || !cs_active(s)) return 0;
    int p = cs_phase(s);
    return p == CP_PHASE_LOADING || p == CP_PHASE_PLAYING;
}

void lobby_note(lobby_t *l, int what, int pid, int a, int b, const char *text)
{
    if (what == CS_N_REFUSED || what == CS_N_LOST || what == CS_N_NOTICE || what == CS_N_PEER_LEAVE) {
        snprintf(l->note_text, sizeof l->note_text, "%s", text ? text : "");
        l->note_what = what, l->note_new = 1, l->note_a = a, l->note_b = b, l->note_pid = pid;
    }
}

/* ------------------------------------------------------------------ actions */
static void drain_job(lobby_t *l);
static void leave_session(lobby_t *l)
{
    char out[120];
    run(l, out, sizeof out, "stop");
    l->job = JOB_NONE;
    drain_job(l);
    l->started = 0;
}

static void push_session(lobby_t *l, int episode, int part, int diff)
{
    char out[160];
    run(l, out, sizeof out, "session %d %d %d %d", episode, part, diff, (l->set.sflags & 7) | (l->cont ? (int)CP_SESS_CONT : 0));
}

/* 0.2.1: this PC's co-op save can continue (1 + its half and difficulty) */
static int own_save(lobby_t *l, int *part, int *diff)
{
    int p = -1, d = 1;
    if (!l->env.coop_save || !l->env.coop_save(l->env.user, &p, &d) || p < 0 || p > 1) return 0;
    if (part) *part = p;
    if (diff) *diff = d;
    return 1;
}
/* 0.2.1: a Continue session (CP_SESS_CONT) can start only when both PCs hold a co-op save of its half (the guest follows the host's checkpoint) */
static int cont_ok(lobby_t *l, const facts *f)
{
    if (!(f->ss.flags & CP_SESS_CONT)) return 1;
    int p = -1;
    if (f->host) {
        if (!own_save(l, &p, NULL) || p != f->ss.part) return 0;
        return f->other < 0 || (f->pl[f->other].flags & (f->ss.part ? CP_PF_SAVE1 : CP_PF_SAVE0)) != 0;
    }
    return own_save(l, &p, NULL) && p == f->ss.part;
}

/* forget a finished result nobody collected (a cancelled attempt), so the next job is accepted */
static void drain_job(lobby_t *l)
{
    char tmp[8];
    if (l->env.async_poll) l->env.async_poll(l->env.user, tmp, sizeof tmp);
}

static int start_job(lobby_t *l, int kind, const char *line)
{
    drain_job(l);
    if (!l->env.async_start || !l->env.async_start(l->env.user, line)) return 0;
    l->job = kind;
    l->t_job = now(l);
    return 1;
}

static void begin_host(lobby_t *l, int way)
{
    char out[160], cmd[96];
    l->way = way;
    l->set.transport = way;
    save(l);
    run(l, out, sizeof out, "char %s", char_words[l->set.character & 3]);
    if (way == 0) snprintf(cmd, sizeof cmd, "steamhost friends 2");
    else snprintf(cmd, sizeof cmd, "host %d enet", l->set.port);
    l->hosting = 1;
    if (start_job(l, JOB_HOST, cmd)) {
        goto_screen(l, LS_CONNECTING);
        set_status(l, LSK_INFO, 0, "%s", tx(l, LT_CONNECTING));
    } else
        set_status(l, LSK_ERROR, 4000, "%s", tx(l, LT_FAIL_CREATE));
}

static void begin_join(lobby_t *l, int way, const char *cmd_arg, const char *target_label)
{
    char out[160], cmd[160];
    l->way = way;
    l->set.transport = way;
    save(l);
    run(l, out, sizeof out, "char %s", char_words[l->set.character & 3]);
    if (way == 0) snprintf(cmd, sizeof cmd, "steamjoin %s", cmd_arg);
    else snprintf(cmd, sizeof cmd, "join %s enet", cmd_arg);
    if (!target_label || !target_label[0]) target_label = cmd_arg;
    snprintf(l->target, sizeof l->target, "%s", target_label);
    l->hosting = 0;
    if (start_job(l, JOB_JOIN, cmd)) {
        goto_screen(l, LS_CONNECTING);
        set_status(l, LSK_INFO, 0, "%s", tx(l, LT_CONNECTING));
    } else
        set_status(l, LSK_ERROR, 4000, "%s", tx(l, LT_FAIL_JOIN));
}

static void start_friend_search(lobby_t *l)
{
    l->nfr = 0, l->fr_done = 0;
    if (!start_job(l, JOB_FRIENDS, "steamlobbies")) return;
    set_status(l, LSK_INFO, 0, "%s", tx(l, LT_SEARCHING));
}

/* "  friend 765611980... "name" is in the game, lobby 109775... (ours)"  and  "  lobby 1097... host 7656... "name"  1/2  lobby" */
static void parse_friends(lobby_t *l, const char *text)
{
    l->nfr = 0;
    const char *p = text;
    while (*p && l->nfr < MAX_FR) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char line[300];
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n), line[n] = 0;
        p += eol ? n + 1 : n;
        unsigned long long id = 0, lobby = 0;
        char name[96] = "";
        const char *q = line;
        while (*q == ' ') q++;
        if (!strncmp(q, "friend ", 7) && strstr(q, ", lobby ")) {
            const char *a = strchr(q, '"');
            const char *b = a ? strchr(a + 1, '"') : NULL;
            id = strtoull(q + 7, NULL, 10);
            if (a && b && b - a - 1 < (int)sizeof name) memcpy(name, a + 1, (size_t)(b - a - 1)), name[b - a - 1] = 0;
            lobby = strtoull(strstr(q, ", lobby ") + 8, NULL, 10);
            /* a friend in the game with a lobby: the lobby's data (our tag) is only known once it is joined, so "(ours)" is not required; a lobby of another mode fails to join cleanly */
            if (id && lobby) {
                snprintf(l->fr[l->nfr].label, sizeof l->fr[0].label, "%s", name[0] ? name : "?");
                snprintf(l->fr[l->nfr].cmd, sizeof l->fr[0].cmd, "%llu", id);
                l->nfr++;
            }
        } else if (!strncmp(q, "lobby ", 6) && strstr(q, " host ")) {
            const char *a = strchr(q, '"');
            const char *b = a ? strchr(a + 1, '"') : NULL;
            lobby = strtoull(q + 6, NULL, 10);
            if (a && b && b - a - 1 < (int)sizeof name) memcpy(name, a + 1, (size_t)(b - a - 1)), name[b - a - 1] = 0;
            if (lobby) {
                snprintf(l->fr[l->nfr].label, sizeof l->fr[0].label, "%s", name[0] ? name : "?");
                snprintf(l->fr[l->nfr].cmd, sizeof l->fr[0].cmd, "lobby:%llu", lobby);
                l->nfr++;
            }
        }
    }
    l->fr_done = 1;
}

static void job_done(lobby_t *l, const char *out)
{
    if (l->job == JOB_NAME) {   /* "steam: ready (...), you are <id> \"<persona name>\", ..." */
        l->job = JOB_NONE;
        const char *a = strstr(out, "you are ");
        const char *q = a ? strchr(a, '"') : NULL;
        const char *e = q ? strchr(q + 1, '"') : NULL;
        if (q && e && e > q + 1 && e - q - 1 <= CP_NAME_MAX && !l->set.name[0]) {
            char nm[CP_NAME_MAX + 1], res[120];
            memcpy(nm, q + 1, (size_t)(e - q - 1)), nm[e - q - 1] = 0;
            facts f;
            get_facts(l, &f);
            if (!f.active) {
                snprintf(l->set.name, sizeof l->set.name, "%s", nm);
                run(l, res, sizeof res, "name %s", nm);
                snprintf(l->applied_name, sizeof l->applied_name, "%s", nm);
                save(l);
            }
        }
        return;
    }
    int kind = l->job;
    l->job = JOB_NONE;
    if (l->cancelled) {   /* the attempt was given up while it ran: whatever it created is closed again */
        l->cancelled = 0;
        leave_session(l);
        return;
    }
    if (kind == JOB_FRIENDS) {
        if (!strncmp(out, "steam not ready", 15) || !strncmp(out, "steam: not available", 20)) {
            l->nfr = 0, l->fr_done = 1;
            set_status(l, LSK_ERROR, 0, "%s", tx(l, LT_STEAM_NA));
            return;
        }
        parse_friends(l, out);
        if (l->nfr) clear_status(l);
        else set_status(l, LSK_INFO, 0, "%s", tx(l, LT_NO_FRIENDS));
        return;
    }
    if (kind == JOB_HOST) {
        if (strstr(out, "failed") || !strstr(out, "hosting")) {
            goto_screen(l, LS_HOST_WAY);
            if (strstr(out, "not ready") || strstr(out, "not available")) set_status(l, LSK_ERROR, 6000, "%s", tx(l, LT_STEAM_NA));
            else set_status(l, LSK_ERROR, 6000, "%s", tx(l, LT_FAIL_CREATE));
            return;
        }
        push_session(l, l->set.episode, l->set.character >= 2 ? 1 : 0, l->set.difficulty);
        enter_room(l);
        return;
    }
    if (kind == JOB_JOIN) {
        if (strstr(out, "failed") || strstr(out, "usage") || strstr(out, "no friend") || strstr(out, "no pending")) {
            goto_screen(l, l->way == 0 ? LS_FRIENDS : LS_JOIN_IP);
            if (strstr(out, "not ready") || strstr(out, "not available")) set_status(l, LSK_ERROR, 6000, "%s", tx(l, LT_STEAM_NA));
            else set_status(l, LSK_ERROR, 6000, "%s", tx(l, LT_FAIL_JOIN));
            return;
        }
        /* joined asynchronously: update() moves on when the host answers */
        l->t_job = now(l);
    }
}

static void poll_job(lobby_t *l)
{
    if (!l->job || !l->env.async_poll) return;
    char out[6000];
    out[0] = 0;
    if (l->env.async_poll(l->env.user, out, sizeof out)) job_done(l, out);
}

/* ------------------------------------------------------------------ rows */
static void add_row(lobby_view *v, int action, int kind, const char *label, const char *value)
{
    if (v->nrows >= LOBBY_ROWS) return;
    lobby_row *r = &v->rows[v->nrows++];
    memset(r, 0, sizeof *r);
    r->action = action, r->kind = kind;
    snprintf(r->label, sizeof r->label, "%s", label ? label : "");
    snprintf(r->value, sizeof r->value, "%s", value ? value : "");
}

static int can_start(lobby_t *l, const facts *f, int *needs_partner)
{
    *needs_partner = 0;
    if (!f->active || !f->host || f->phase != CP_PHASE_LOBBY || !episode_ok(l, f) || !cont_ok(l, f)) return 0;
    if (f->count < 2) {
        if (l->set.solo) return 1;
        *needs_partner = 1;
        return 0;
    }
    for (int i = 0; i < CP_MAX_PLAYERS; i++)
        if (f->has[i] && f->pl[i].connected && i != f->pid && !f->pl[i].ready) {
            *needs_partner = 1;
            return 0;
        }
    if (f->other >= 0 && f->pl[f->other].character <= 3 && (f->pl[f->other].character >> 1) != f->ss.part) {
        *needs_partner = 1;
        return 0;
    }
    return 1;
}

static void build_rows(lobby_t *l, lobby_view *v, const facts *f)
{
    char buf[160];
    v->nrows = 0;
    switch (l->screen) {
    case LS_TOP:
        snprintf(v->title, sizeof v->title, "%s", tx(l, LT_COOP));
        add_row(v, A_HOST, LV_NORMAL, tx(l, LT_HOST), "");
        add_row(v, A_JOIN, LV_NORMAL, tx(l, LT_JOIN), "");
        add_row(v, A_BACK, LV_NORMAL, tx(l, LT_BACK), "");
        break;
    case LS_HOST_WAY:
        snprintf(v->title, sizeof v->title, "%s", tx(l, LT_HOST));
        add_row(v, A_FRIEND, LV_NORMAL, tx(l, LT_FRIEND), "Steam");
        add_row(v, A_LINK, LV_NORMAL, tx(l, LT_LINK), "");
        add_row(v, A_BACK, LV_NORMAL, tx(l, LT_BACK), "");
        break;
    case LS_JOIN_WAY:
        snprintf(v->title, sizeof v->title, "%s", tx(l, LT_JOIN));
        add_row(v, A_FRIEND, LV_NORMAL, tx(l, LT_FRIEND), "Steam");
        add_row(v, A_LINK, LV_NORMAL, tx(l, LT_LINK), "");
        add_row(v, A_BACK, LV_NORMAL, tx(l, LT_BACK), "");
        break;
    case LS_JOIN_IP:
        snprintf(v->title, sizeof v->title, "%s - %s", tx(l, LT_JOIN), tx(l, LT_LINK));
        snprintf(v->prompt, sizeof v->prompt, "%s", tx(l, LT_ENTER_ADDR));
        v->input_on = 1;
        snprintf(v->input, sizeof v->input, "%s", l->input);
        add_row(v, A_JOIN, l->input[0] ? LV_NORMAL : LV_GREY, tx(l, LT_JOIN), "");
        add_row(v, A_BACK, LV_NORMAL, tx(l, LT_BACK), "");
        break;
    case LS_FRIENDS:
        snprintf(v->title, sizeof v->title, "%s - %s", tx(l, LT_JOIN), tx(l, LT_FRIEND));
        for (int i = 0; i < l->nfr; i++) add_row(v, A_FRIEND_ENTRY + i, LV_NORMAL, l->fr[i].label, "");
        add_row(v, A_REFRESH, l->job == JOB_FRIENDS ? LV_GREY : LV_NORMAL, tx(l, LT_REFRESH), "");
        add_row(v, A_BACK, LV_NORMAL, tx(l, LT_BACK), "");
        v->busy = l->job == JOB_FRIENDS;
        break;
    case LS_CONNECTING:
        snprintf(v->title, sizeof v->title, "%s", tx(l, l->hosting ? LT_HOST : LT_JOIN));
        add_row(v, A_BACK, LV_NORMAL, tx(l, LT_BACK), "");
        v->busy = 1;
        break;
    case LS_LEAVE_ASK:
        snprintf(v->title, sizeof v->title, "%s", tx(l, LT_COOP));
        snprintf(v->prompt, sizeof v->prompt, "%s", tx(l, LT_QUIT_SESSION));
        add_row(v, A_YES, LV_NORMAL, tx(l, LT_YES), "");
        add_row(v, A_NO, LV_NORMAL, tx(l, LT_NO), "");
        break;
    case LS_KICK_ASK: {   /* the host's "Remove partner": the stock "Do you want to kick <name>?" */
        char nm[64];
        snprintf(v->title, sizeof v->title, "%s", tx(l, LT_COOP));
        lobby_format(tx(l, LT_KICK_ASK), f->other >= 0 ? shown_name(l, f->pl[f->other].name, nm, sizeof nm) : "?", NULL, v->prompt, sizeof v->prompt);
        add_row(v, A_YES, LV_NORMAL, tx(l, LT_YES), "");
        add_row(v, A_NO, LV_NORMAL, tx(l, LT_NO), "");
        break;
    }
    case LS_ROOM: {
        snprintf(v->title, sizeof v->title, "%s", tx(l, LT_COOP));
        int myc = f->connected ? f->pl[f->pid].character : CP_CHAR_NONE;
        char nm[64];
        for (int c = 0; c < 4; c++) {
            int taken = slot_taken_by_partner(f, c);
            int other_part = !f->host && (c >> 1) != f->ss.part;
            const char *val = "";
            if (c == myc) val = tx(l, LT_YOU);
            else if (taken && f->other >= 0 && f->pl[f->other].character == c) val = shown_name(l, f->pl[f->other].name, nm, sizeof nm);
            add_row(v, A_CHAR + c, taken || other_part ? LV_GREY : LV_NORMAL, char_name(l, c), val);
        }
        if (f->host) {
            if (l->set.episodes_all) {
                snprintf(buf, sizeof buf, "<  %d  >", f->ss.episode);
                add_row(v, A_EPISODE, LV_NORMAL, tx(l, LT_EPISODE), buf);
            } else {   /* 0.2.0: Episode 1 only (the info block says so) */
                snprintf(buf, sizeof buf, "%d", f->ss.episode ? f->ss.episode : 1);
                add_row(v, 0, LV_INFO, tx(l, LT_EPISODE), buf);
            }
            if (l->cont || own_save(l, NULL, NULL)) {   /* 0.2.1: the checkpoint of the co-op save (the half and the difficulty come from it) */
                snprintf(buf, sizeof buf, "<  %s  >", tx(l, (f->ss.flags & CP_SESS_CONT) ? LT_YES : LT_NO));
                add_row(v, A_CONT, LV_NORMAL, tx(l, LT_CONTINUE), buf);
            }
            if (f->ss.flags & CP_SESS_CONT) add_row(v, 0, LV_INFO, tx(l, LT_DIFFICULTY), diff_name(l, f->ss.difficulty));
            else {
                snprintf(buf, sizeof buf, "<  %s  >", diff_name(l, f->ss.difficulty));
                add_row(v, A_DIFF, LV_NORMAL, tx(l, LT_DIFFICULTY), buf);
            }
            snprintf(buf, sizeof buf, "<  %s  >", tx(l, (f->ss.flags & CP_SESS_DOWNED) ? LT_YES : LT_NO));
            add_row(v, A_DOWNED, LV_NORMAL, tx(l, LT_DOWNED), buf);
            int need = 0;
            int ok = can_start(l, f, &need);
            add_row(v, A_START, ok ? LV_NORMAL : LV_GREY, tx(l, LT_START), "");
            if (f->steam && f->count < 2) add_row(v, A_INVITE, LV_NORMAL, tx(l, LT_INVITE), "");
            if (f->other >= 0 && f->phase == CP_PHASE_LOBBY) add_row(v, A_KICK, LV_NORMAL, tx(l, LT_KICK), "");
        } else {
            snprintf(buf, sizeof buf, "%d", f->ss.episode);
            add_row(v, 0, LV_INFO, tx(l, LT_EPISODE), buf);
            if (f->ss.flags & CP_SESS_CONT) add_row(v, 0, LV_INFO, tx(l, LT_CONTINUE), tx(l, LT_YES));   /* 0.2.1: the host continues its co-op save */
            add_row(v, 0, LV_INFO, tx(l, LT_DIFFICULTY), diff_name(l, f->ss.difficulty));
            add_row(v, 0, LV_INFO, tx(l, LT_DOWNED), tx(l, (f->ss.flags & CP_SESS_DOWNED) ? LT_YES : LT_NO));
            int me_ready = f->connected && f->pl[f->pid].ready;
            add_row(v, A_READY, episode_ok(l, f) ? LV_NORMAL : LV_GREY, tx(l, LT_READY), me_ready ? "[X]" : "[  ]");
        }
        add_row(v, A_LEAVE, LV_NORMAL, tx(l, LT_LEAVE), "");
        break;
    }
    }
}

static int row_selectable(const lobby_row *r) { return r->kind == LV_NORMAL && r->action; }

static void fix_sel(lobby_view *v, int *sel)
{
    if (*sel >= 0 && *sel < v->nrows && row_selectable(&v->rows[*sel])) return;
    for (int i = 0; i < v->nrows; i++)
        if (row_selectable(&v->rows[i])) {
            *sel = i;
            return;
        }
    *sel = -1;
}

void lobby_view_get(lobby_t *l, lobby_view *v)
{
    facts f;
    memset(v, 0, sizeof *v);
    get_facts(l, &f);
    v->screen = l->screen;
    snprintf(v->col_name, sizeof v->col_name, "%s", tx(l, LT_NAME));
    snprintf(v->col_char, sizeof v->col_char, "%s", tx(l, LT_CHARACTER));
    snprintf(v->col_ping, sizeof v->col_ping, "%s", tx(l, LT_PING));
    snprintf(v->t_ready, sizeof v->t_ready, "%s", tx(l, LT_PARTNER_READY));
    snprintf(v->t_not_ready, sizeof v->t_not_ready, "%s", tx(l, LT_NOT_READY));
    snprintf(v->t_host, sizeof v->t_host, "%s", tx(l, LT_HOST_TAG));
    snprintf(v->t_ms, sizeof v->t_ms, "%s", tx(l, LT_MS));
    build_rows(l, v, &f);
    fix_sel(v, &l->sel);
    v->sel = l->sel;
    if ((l->screen == LS_ROOM || l->screen == LS_LEAVE_ASK || l->screen == LS_KICK_ASK)) {
        for (int i = 0; i < CP_MAX_PLAYERS; i++) {
            if (!f.has[i] || !f.pl[i].connected) continue;
            lobby_vplayer *p = &v->players[v->nplayers++];
            shown_name(l, f.pl[i].name, p->name, sizeof p->name);
            snprintf(p->chr, sizeof p->chr, "%s", char_name(l, f.pl[i].character));
            p->ready = f.pl[i].ready || i == 0, p->host = i == 0, p->me = i == f.pid, p->ping = f.pl[i].rtt_ms;
        }
        if (f.active && f.host) {
            if (!f.steam && l->addr[0]) snprintf(v->subtitle, sizeof v->subtitle, "%s: %s:%d", tx(l, LT_YOUR_ADDR), l->addr, l->set.port);
            else if (!f.steam) snprintf(v->subtitle, sizeof v->subtitle, "%s: :%d", tx(l, LT_YOUR_ADDR), l->set.port);
            else snprintf(v->subtitle, sizeof v->subtitle, "%s", tx(l, LT_STEAM_HINT));
        }
    }
    if (l->screen == LS_ROOM) snprintf(v->note, sizeof v->note, "%s", tx(l, LT_SAVE_WARN));
    if (l->screen == LS_LEAVE_ASK) snprintf(v->note, sizeof v->note, "%s", tx(l, LT_LEAVE_HINT));
    if (l->screen == LS_ROOM && f.active) {   /* the info block: Episode 1 only; how a friend reaches a System Link host over the internet */
        char port[16], hint[400];
        int o = snprintf(v->info, sizeof v->info, "%s", l->set.episodes_all ? "" : tx(l, LT_EP1_ONLY));
        if (f.host && !f.steam) {
            snprintf(port, sizeof port, "%d", l->set.port);
            lobby_format(tx(l, LT_HOST_NET_HINT), port, NULL, hint, sizeof hint);
            if (o < (int)sizeof v->info - 2) snprintf(v->info + o, sizeof v->info - (size_t)o, "%s%s", o ? "\n" : "", hint);
        }
    }
    snprintf(v->status, sizeof v->status, "%s", l->status);
    v->status_kind = l->status_kind;
    if (l->screen == LS_CONNECTING && !l->hosting && l->target[0] && !strcmp(l->status, tx(l, LT_CONNECTING))) {
        /* the guest's progress (RE5 / RE6 style): the transport connects, then the host's answer is awaited */
        int stage = sess(l) ? cs_join_stage(sess(l)) : 0;
        lobby_format(tx(l, stage == 2 ? LT_AWAITING : LT_CONNECTING_TO), l->target, NULL, v->status, sizeof v->status);
    }
    if (l->screen == LS_ROOM && !l->status[0] && f.active) {
        int need = 0;
        char nm[64];
        const char *other = f.other >= 0 ? shown_name(l, f.pl[f.other].name, nm, sizeof nm) : "";
        if (f.host && f.phase == CP_PHASE_LOBBY) {
            if (f.count < 2) snprintf(v->status, sizeof v->status, "%s", tx(l, LT_WAITING));
            else if (!cont_ok(l, &f)) snprintf(v->status, sizeof v->status, "%s", tx(l, LT_CONT_NEED)), v->status_kind = LSK_ERROR;   /* 0.2.1 */
            else if (!can_start(l, &f, &need) && need) {
                if (f.other >= 0 && !f.pl[f.other].ready) lobby_format(tx(l, LT_HOST_WAIT_READY), other, NULL, v->status, sizeof v->status);
                else snprintf(v->status, sizeof v->status, "%s", tx(l, LT_NEED_READY));
            } else if (f.other >= 0) lobby_format(tx(l, LT_HOST_CAN_START), other, NULL, v->status, sizeof v->status);
        } else if (!f.host && f.phase == CP_PHASE_LOBBY && f.connected) {
            if (!episode_ok(l, &f)) snprintf(v->status, sizeof v->status, "%s", tx(l, LT_EP1_ONLY)), v->status_kind = LSK_ERROR;
            else if (!cont_ok(l, &f)) snprintf(v->status, sizeof v->status, "%s", tx(l, LT_CONT_NEED)), v->status_kind = LSK_ERROR;   /* 0.2.1 */
            else if (!f.pl[f.pid].ready) snprintf(v->status, sizeof v->status, "%s", tx(l, LT_GUEST_PICK));
            else {
                char hn[64];
                lobby_format(tx(l, LT_GUEST_WAIT_HOST), f.has[0] ? shown_name(l, f.pl[0].name, hn, sizeof hn) : "?", NULL, v->status, sizeof v->status);
            }
        }
        if (f.phase != CP_PHASE_LOBBY) snprintf(v->status, sizeof v->status, "%s", tx(l, LT_STARTING));
    }
    snprintf(v->hint, sizeof v->hint, "%s", tx(l, l->screen == LS_JOIN_IP ? LT_HINT_IP : LT_HINT));
    snprintf(v->leg_ok, sizeof v->leg_ok, "%s", tx(l, LT_LEG_OK));
    snprintf(v->leg_back, sizeof v->leg_back, "%s", tx(l, LT_BACK));
}

/* ------------------------------------------------------------------ input */
static void step_sel(lobby_t *l, int dir)
{
    lobby_view v;
    lobby_view_get(l, &v);
    if (v.nrows == 0) return;
    int i = v.sel;
    for (int n = 0; n < v.nrows; n++) {
        i = (i + dir + v.nrows) % v.nrows;
        if (row_selectable(&v.rows[i])) {
            l->sel = i;
            return;
        }
    }
}

static void do_pick(lobby_t *l, int c)
{
    facts f;
    get_facts(l, &f);
    if (!f.active || !f.connected || f.phase != CP_PHASE_LOBBY) return;
    if (slot_taken_by_partner(&f, c)) {
        set_status(l, LSK_ERROR, 3000, "%s", tx(l, LT_SAME_SEL));
        return;
    }
    if (!f.host && (c >> 1) != f.ss.part) return;
    char out[160];
    run(l, out, sizeof out, "char %s", char_words[c & 3]);
    l->set.character = c;
    save(l);
    if (f.host && (c >> 1) != f.ss.part) {   /* the host's pick decides the half of the story */
        if (l->cont) l->cont = 0;            /* 0.2.1: a character of the other half is a new game of that half */
        push_session(l, f.ss.episode, c >> 1, l->cont ? f.ss.difficulty : l->set.difficulty);
    }
}

static void change_value(lobby_t *l, int action, int dir)
{
    facts f;
    get_facts(l, &f);
    if (!f.active || !f.host || f.phase != CP_PHASE_LOBBY) return;
    int ep = f.ss.episode ? f.ss.episode : 1, df = f.ss.difficulty;
    if (action == A_CONT) {   /* 0.2.1: Continue takes the half and the difficulty of the co-op save; the host's character moves to the same slot of that half */
        int part = -1, sdf = 1;
        if (!l->cont) {
            if (!own_save(l, &part, &sdf)) return;
            l->cont = 1;
            int c = f.connected ? f.pl[f.pid].character : CP_CHAR_NONE;
            if (c <= 3 && (c >> 1) != part) {
                char out[120];
                run(l, out, sizeof out, "char %s", char_words[part * 2 + (c & 1)]);
            }
            push_session(l, 1, part, sdf);
        } else {
            l->cont = 0;
            push_session(l, ep, f.ss.part, l->set.difficulty);
        }
        return;
    }
    if (action == A_EPISODE) ep = (ep - 1 + dir + 4) % 4 + 1;
    else if (action == A_DIFF && !l->cont) df = (df + dir + 4) % 4;
    else if (action == A_DOWNED) l->set.sflags = (int)(((unsigned)f.ss.flags ^ CP_SESS_DOWNED) & 7u);   /* the rule is a toggle (the bleed-out preset bits stay as they are) */
    else return;
    push_session(l, ep, f.ss.part, df);
    l->set.episode = ep, l->set.difficulty = df;
    save(l);
}

static void activate(lobby_t *l, int action)
{
    char out[200];
    facts f;
    get_facts(l, &f);
    if (action >= A_CHAR && action < A_CHAR + 4) {
        do_pick(l, action - A_CHAR);
        return;
    }
    if (action >= A_FRIEND_ENTRY && action < A_FRIEND_ENTRY + MAX_FR) {
        int i = action - A_FRIEND_ENTRY;
        if (i < l->nfr) begin_join(l, 0, l->fr[i].cmd, l->fr[i].label);
        return;
    }
    switch (action) {
    case A_HOST: goto_screen(l, LS_HOST_WAY), clear_status(l); break;
    case A_JOIN: {
        if (l->screen == LS_JOIN_IP) {
            if (l->input[0]) {
                snprintf(l->set.ip, sizeof l->set.ip, "%s", l->input);
                begin_join(l, 1, l->input, l->input);
            }
        } else
            goto_screen(l, LS_JOIN_WAY), clear_status(l);
        break;
    }
    case A_FRIEND:
        if (l->screen == LS_HOST_WAY) begin_host(l, 0);
        else {
            goto_screen(l, LS_FRIENDS);
            start_friend_search(l);
        }
        break;
    case A_LINK:
        if (l->screen == LS_HOST_WAY) begin_host(l, 1);
        else {
            goto_screen(l, LS_JOIN_IP);
            snprintf(l->input, sizeof l->input, "%s", l->set.ip);
            clear_status(l);
        }
        break;
    case A_REFRESH: start_friend_search(l); break;
    case A_INVITE:
        run(l, out, sizeof out, "steaminvite");
        if (strstr(out, "could not") || strstr(out, "refused") || strstr(out, "no Steam lobby")) set_status(l, LSK_ERROR, 5000, "%s", tx(l, LT_STEAM_NA));
        break;
    case A_EPISODE: change_value(l, A_EPISODE, 1); break;
    case A_DIFF: change_value(l, A_DIFF, 1); break;
    case A_DOWNED: change_value(l, A_DOWNED, 1); break;
    case A_CONT: change_value(l, A_CONT, 1); break;
    case A_READY:
        if (!episode_ok(l, &f)) {
            set_status(l, LSK_ERROR, 4000, "%s", tx(l, LT_EP1_ONLY));
            break;
        }
        if (f.active && f.connected) cs_ready(sess(l), !f.pl[f.pid].ready);
        break;
    case A_KICK:   /* host, room: "Remove partner" asks first (the cursor starts on "No") */
        if (f.active && f.host && f.other >= 0 && f.phase == CP_PHASE_LOBBY) {
            goto_screen(l, LS_KICK_ASK);
            l->sel = 1;
        }
        break;
    case A_START: {
        int need = 0;
        if (!can_start(l, &f, &need)) {
            if (!cont_ok(l, &f)) set_status(l, LSK_ERROR, 4000, "%s", tx(l, LT_CONT_NEED));   /* 0.2.1 */
            else set_status(l, LSK_INFO, 3000, "%s", tx(l, LT_NEED_READY));
            break;
        }
        run(l, out, sizeof out, f.count < 2 && l->set.solo ? "start force" : "start");
        if (!strstr(out, "START sent")) set_status(l, LSK_ERROR, 4000, "%s", tx(l, LT_NEED_READY));   /* (lang: was the console's English reply) */
        break;
    }
    case A_LEAVE:
        if (f.active && f.count >= 2) {
            l->ask_from = (char)l->screen;
            goto_screen(l, LS_LEAVE_ASK);
            l->sel = 1;   /* the cursor starts on "No": a stray confirm must not end the session */
        } else {
            leave_session(l);
            goto_screen(l, LS_TOP);
        }
        break;
    case A_YES:
        if (l->screen == LS_KICK_ASK) {
            if (f.active && f.host && f.other >= 0) run(l, out, sizeof out, "kick %d %d", f.other, CP_LEAVE_REMOVED);
            goto_screen(l, LS_ROOM), l->sel = -1;
            break;
        }
        leave_session(l);
        goto_screen(l, LS_TOP);
        break;
    case A_NO: goto_screen(l, LS_ROOM), l->sel = -1; break;
    case A_BACK:
        switch (l->screen) {
        case LS_TOP: l->request = LR_CLOSE; break;
        case LS_HOST_WAY:
        case LS_JOIN_WAY: goto_screen(l, LS_TOP); break;
        case LS_JOIN_IP:
        case LS_FRIENDS: goto_screen(l, LS_JOIN_WAY); break;
        case LS_CONNECTING:
            if (l->job) l->cancelled = 1;
            else leave_session(l);
            goto_screen(l, l->hosting ? LS_HOST_WAY : LS_JOIN_WAY);
            clear_status(l);
            break;
        default: break;
        }
        break;
    default: break;
    }
}

static void back(lobby_t *l)
{
    switch (l->screen) {
    case LS_ROOM: activate(l, A_LEAVE); break;
    case LS_LEAVE_ASK:
    case LS_KICK_ASK: activate(l, A_NO); break;
    default: activate(l, A_BACK); break;
    }
}

static int input_char_ok(int ch) { return isalnum((unsigned char)ch) || ch == '.' || ch == ':' || ch == '-' || ch == '_' || ch == '[' || ch == ']'; }

void lobby_event_in(lobby_t *l, const lobby_event *e)
{
    if (!l->open || !e) return;
    lobby_view v;
    switch (e->kind) {
    case LE_UP: step_sel(l, -1); break;
    case LE_DOWN: step_sel(l, 1); break;
    case LE_LEFT:
    case LE_RIGHT:
        lobby_view_get(l, &v);
        if (v.sel >= 0) change_value(l, v.rows[v.sel].action, e->kind == LE_RIGHT ? 1 : -1);
        break;
    case LE_OK:
    case LE_CLICK:
        lobby_view_get(l, &v);
        if (e->kind == LE_CLICK && e->row >= 0 && e->row < v.nrows && row_selectable(&v.rows[e->row])) l->sel = e->row, v.sel = e->row;
        if (v.sel >= 0 && v.sel < v.nrows) activate(l, v.rows[v.sel].action);
        break;
    case LE_HOVER:
        lobby_view_get(l, &v);
        if (e->row >= 0 && e->row < v.nrows && row_selectable(&v.rows[e->row])) l->sel = e->row;
        break;
    case LE_BACK: back(l); break;
    case LE_CHAR:
        if (l->screen == LS_JOIN_IP && input_char_ok(e->ch)) {
            size_t n = strlen(l->input);
            if (n + 1 < 62) l->input[n] = (char)e->ch, l->input[n + 1] = 0;
            l->sel = 0;   /* typing puts the cursor back on the connect row, so Enter connects */
        }
        break;
    case LE_BACKSPACE:
        if (l->screen == LS_JOIN_IP) {
            size_t n = strlen(l->input);
            if (n) l->input[n - 1] = 0, l->sel = 0;
            else back(l);
        }
        break;
    case LE_PASTE:
        if (l->screen == LS_JOIN_IP && l->env.clipboard) {
            char clip[200];
            if (l->env.clipboard(l->env.user, clip, sizeof clip)) {
                size_t n = strlen(l->input);
                for (const char *p = clip; *p && n + 1 < 62; p++) {
                    if (input_char_ok((unsigned char)*p)) l->input[n++] = *p;
                    else if (*p == '\r' || *p == '\n') break;
                }
                l->input[n] = 0;
                l->sel = 0;
            }
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ per frame */
void lobby_update(lobby_t *l)
{
    if (!l->open) return;
    uint32_t t = now(l);
    if (l->status_until && (int32_t)(t - l->status_until) >= 0) clear_status(l);
    poll_job(l);
    facts f;
    get_facts(l, &f);

    if (l->note_new) {
        l->note_new = 0;
        if (l->screen == LS_CONNECTING && (l->note_what == CS_N_REFUSED || l->note_what == CS_N_LOST)) {
            goto_screen(l, l->hosting ? LS_HOST_WAY : (l->way == 0 ? LS_FRIENDS : LS_JOIN_IP));
            /* REFUSED: menu.c put the reason in the player's language; LOST (the link dropped while joining): the session's text is English, the stock words instead */
            const char *why = l->note_what == CS_N_REFUSED ? l->note_text : tx(l, LT_CONN_LOST);
            set_status(l, LSK_ERROR, 8000, "%s%s%s", tx(l, LT_FAIL_JOIN), why[0] ? "  " : "", why);
            l->job = JOB_NONE;
        } else if ((l->screen == LS_ROOM || l->screen == LS_LEAVE_ASK || l->screen == LS_KICK_ASK) && l->note_what == CS_N_LOST) {
            /* the guest in the room: removed by the host, the host closed the room, or the link is gone */
            goto_screen(l, LS_TOP);
            set_status(l, LSK_ERROR, 8000, "%s", tx(l, l->note_a == 1 && l->note_b == CP_LEAVE_REMOVED ? LT_REMOVED : l->note_a == 1 ? LT_SESSION_OVER : LT_CONN_LOST));
        } else if ((l->screen == LS_ROOM || l->screen == LS_KICK_ASK) && l->note_what == CS_N_PEER_LEAVE && f.host && f.phase == CP_PHASE_LOBBY && l->note_b != CS_GONE_REMOVED &&
                   l->note_b != CS_GONE_REPLACED) {
            /* the host in the room: the partner left or was lost (the stock "<name> has left the session." / "... has disconnected.") */
            char nm[64];
            if (l->screen == LS_KICK_ASK) goto_screen(l, LS_ROOM), l->sel = -1;
            set_status(l, LSK_INFO, 5000, "%s", "");
            lobby_format(tx(l, l->note_a == 1 ? LT_PARTNER_LOST_ROOM : LT_PARTNER_LEFT_ROOM), shown_name(l, l->note_text, nm, sizeof nm), NULL, l->status, sizeof l->status);
        }
    }

    /* a session that appeared by itself (Join game click, console) while the player is on a menu screen: show its room */
    if (!l->job && !l->cancelled && f.active && f.connected && f.phase != CP_PHASE_NONE &&
        (l->screen == LS_TOP || l->screen == LS_HOST_WAY || l->screen == LS_JOIN_WAY || l->screen == LS_JOIN_IP || l->screen == LS_FRIENDS)) {
        enter_room(l);
        return;
    }
    switch (l->screen) {
    case LS_CONNECTING:
        if (!l->job && f.active && f.connected && f.phase != CP_PHASE_NONE) {
            enter_room(l);
        } else if (!l->job && !f.active && t - l->t_job > 400) {
            goto_screen(l, l->hosting ? LS_HOST_WAY : (l->way == 0 ? LS_FRIENDS : LS_JOIN_IP));
            set_status(l, LSK_ERROR, 8000, "%s", tx(l, l->hosting ? LT_FAIL_CREATE : LT_FAIL_JOIN));
        } else if (t - l->t_job > 45000) {
            leave_session(l);
            goto_screen(l, l->hosting ? LS_HOST_WAY : LS_JOIN_WAY);
            set_status(l, LSK_ERROR, 8000, "%s", tx(l, LT_FAIL_JOIN));
        }
        break;
    case LS_ROOM:
    case LS_LEAVE_ASK:
    case LS_KICK_ASK:
        if (!f.active) {
            goto_screen(l, LS_TOP);
            set_status(l, LSK_ERROR, 8000, "%s", tx(l, LT_CONN_LOST));
            break;
        }
        if (!l->started && f.phase != CP_PHASE_LOBBY && f.phase != CP_PHASE_NONE) {
            if (!episode_ok(l, &f)) {   /* 0.2.0: a (lab) host started another episode than 1: this PC does not start it */
                leave_session(l);
                goto_screen(l, LS_TOP);
                set_status(l, LSK_ERROR, 8000, "%s", tx(l, LT_EP1_ONLY));
                break;
            }
            l->started = 1;
            l->request = LR_START;
            set_status(l, LSK_OK, 0, "%s", tx(l, LT_STARTING));
            break;
        }
        /* a client follows the host's choice of the story half: its character moves to the same slot of the new pair */
        if (f.connected && !f.host && f.phase == CP_PHASE_LOBBY) {
            int c = f.pl[f.pid].character;
            if (c <= 3 && (c >> 1) != f.ss.part && t - l->last_auto_pick > 700) {
                int want = f.ss.part * 2 + (c & 1);
                char out[120];
                l->last_auto_pick = t;
                run(l, out, sizeof out, "char %s", char_words[want]);
            }
        }
        if (f.host && f.phase == CP_PHASE_LOBBY && f.connected) {   /* the host's character decides the half; keep the session in step */
            int c = f.pl[f.pid].character;
            if (c <= 3 && (c >> 1) != f.ss.part && t - l->last_auto_pick > 700) {
                l->last_auto_pick = t;
                if (l->cont) l->cont = 0;   /* 0.2.1 */
                push_session(l, f.ss.episode, c >> 1, f.ss.difficulty);
            }
        }
        if (f.other >= 0 && !l->partner_seen) {
            l->partner_seen = 1;
            /* the host only: the guest's "partner" is the host it just joined (run 5 showed "A partner has joined." on the guest); its line says what to do next */
            if (f.host) set_status(l, LSK_OK, 3000, "%s", tx(l, LT_PARTNER_JOINED));
        } else if (f.other < 0) l->partner_seen = 0;
        break;
    default: break;
    }
}
