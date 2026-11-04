/*
 * Lobby model tests (native/lobby.c) on the in-process "sim" transport with a virtual clock: two lobbies (host and client) are driven with
 * input events only, the way the menu does it. Covers: host and join screens, IP input (typing, backspace, paste), the room, the character
 * rules (taken slot, story half follows the host, the client follows), ready / start rules, the start request on both sides, leaving
 * (with the question), failures (no host, refused), re-opening on a running session, texts of all languages.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/lobby.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t vclock = 1000;

typedef struct {
    cs_t *s;
    lobby_t *l;
    char name[24];
    char job_out[6000];
    int job_ready;
    int saves;
    char clip[200];
    char local_name[24];
} Node;

static void on_note(void *u, const cs_note *n)
{
    Node *nd = (Node *)u;
    if (nd->l) lobby_note(nd->l, n->what, n->pid, n->a, n->b, n->text);
}
static uint32_t clk(void *u) { (void)u; return vclock; }

/* the glue's console grammar on top of the sim transport: "enet" means "sim" here, `name` is the glue's own command */
static int env_command(void *u, const char *line, char *out, int size)
{
    Node *nd = (Node *)u;
    char buf[200];
    snprintf(buf, sizeof buf, "%s", line);
    size_t n = strlen(buf);
    if (n > 5 && !strcmp(buf + n - 5, " enet")) strcpy(buf + n - 5, " sim");
    if (!strncmp(buf, "name ", 5)) {
        snprintf(nd->local_name, sizeof nd->local_name, "%s", buf + 5);
        snprintf(out, (size_t)size, "name '%s'", buf + 5);
        return 1;
    }
    char err[200] = "";
    int port = 0;
    char target[100] = "";
    if (sscanf(buf, "host %d sim", &port) == 1) {   /* (the console grammar starts a network thread; the virtual clock needs the pumped variant) */
        if (cs_host(nd->s, "sim", port, 0, err, sizeof err)) snprintf(out, (size_t)size, "hosting on sim port %d", port);
        else snprintf(out, (size_t)size, "host failed: %s", err);
        return 1;
    }
    if (sscanf(buf, "join %99s sim", target) == 1) {
        if (cs_join(nd->s, "sim", target, 0, err, sizeof err)) snprintf(out, (size_t)size, "joining %s ...", target);
        else snprintf(out, (size_t)size, "join failed: %s", err);
        return 1;
    }
    return cs_command(nd->s, buf, out, (size_t)size);
}
static cs_t *env_session(void *u) { return ((Node *)u)->s; }
static uint32_t env_now(void *u) { (void)u; return vclock; }
static const char *env_text(void *u, int id) { (void)u; return lobby_own_text(id, 0); }
static int env_async_start(void *u, const char *line)
{
    Node *nd = (Node *)u;
    if (nd->job_ready) return 0;
    if (!strcmp(line, "steamlobbies")) {
        snprintf(nd->job_out, sizeof nd->job_out,
                 "public lobbies with our tag (r2c=R2COOP1): 1\n  lobby 1099511627777  host 76561198052584162 \"Alice\"  1/2  lobby\n"
                 "friends in Revelations 2:\n  friend 76561198052584163 \"Bob\" is in the game, lobby 1099511627780 (ours)\n"
                 "  friend 76561198052584164 \"Carl\" is in the game (no lobby)");
    } else if (!strcmp(line, "steam")) {
        snprintf(nd->job_out, sizeof nd->job_out, "steam: ready (mock), you are 76561198052584162 \"Persona\", pipe msg, listening no, accepts friends\nno lobby");
    } else if (!strncmp(line, "steamhost", 9) || !strncmp(line, "steamjoin", 9)) {
        snprintf(nd->job_out, sizeof nd->job_out, "%s failed: steam not ready: no hub", line);
    } else
        env_command(u, line, nd->job_out, sizeof nd->job_out);
    nd->job_ready = 1;
    return 1;
}
static int env_async_poll(void *u, char *out, int size)
{
    Node *nd = (Node *)u;
    if (!nd->job_ready) return 0;
    snprintf(out, (size_t)size, "%s", nd->job_out);
    nd->job_ready = 0;
    return 1;
}
static void env_save(void *u, const lobby_settings *s) { (void)s; ((Node *)u)->saves++; }
static int env_clip(void *u, char *out, int size) { snprintf(out, (size_t)size, "%s", ((Node *)u)->clip); return ((Node *)u)->clip[0] != 0; }
static int env_addr(void *u, char *out, int size) { (void)u; snprintf(out, (size_t)size, "192.168.1.5"); return 1; }

static void node_init(Node *nd, const char *name, int port)
{
    memset(nd, 0, sizeof *nd);
    snprintf(nd->name, sizeof nd->name, "%s", name);
    cs_config c;
    memset(&c, 0, sizeof c);
    c.name = name, c.want_char = CP_CHAR_NONE, c.mod_version = "test", c.is_bot = 1, c.clock = clk, c.clock_user = nd, c.on_note = on_note, c.user = nd;
    nd->s = cs_new(&c);
    lobby_env e;
    memset(&e, 0, sizeof e);
    e.user = nd, e.command = env_command, e.session = env_session, e.now = env_now, e.text = env_text, e.async_start = env_async_start, e.async_poll = env_async_poll,
    e.save = env_save, e.clipboard = env_clip, e.local_address = env_addr;
    nd->l = lobby_new(&e);
    lobby_settings st;
    lobby_get_settings(nd->l, &st);
    st.port = port;
    snprintf(st.name, sizeof st.name, "%s", name);
    lobby_set_settings(nd->l, &st);
}

static Node *nodes[4];
static int n_nodes;
static void step(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        vclock += 5;
        cn_sim_set_time(vclock);
        for (int i = 0; i < n_nodes; i++) {
            cs_poll(nodes[i]->s);
            lobby_update(nodes[i]->l);
        }
    }
}

static void ev(Node *nd, int kind)
{
    lobby_event e = {kind, 0, 0};
    lobby_event_in(nd->l, &e);
}
static void ev_ch(Node *nd, int ch)
{
    lobby_event e = {LE_CHAR, ch, 0};
    lobby_event_in(nd->l, &e);
}
static void type(Node *nd, const char *s)
{
    for (; *s; s++) ev_ch(nd, *s);
}
static lobby_view view(Node *nd)
{
    lobby_view v;
    lobby_view_get(nd->l, &v);
    return v;
}
/* move the cursor to the row with this label (first match) and return its index, -1 when absent */
static int goto_row(Node *nd, const char *label)
{
    for (int tries = 0; tries < 20; tries++) {
        lobby_view v = view(nd);
        if (v.sel >= 0 && !strcmp(v.rows[v.sel].label, label)) return v.sel;
        ev(nd, LE_DOWN);
        step(10);
    }
    return -1;
}
static void press(Node *nd, const char *label)
{
    CHECK(goto_row(nd, label) >= 0);
    ev(nd, LE_OK);
    step(30);
}
static int row_kind(Node *nd, const char *label)
{
    lobby_view v = view(nd);
    for (int i = 0; i < v.nrows; i++)
        if (!strcmp(v.rows[i].label, label)) return v.rows[i].kind;
    return -1;
}
static const char *row_value(Node *nd, const char *label)
{
    static lobby_view v;
    v = view(nd);
    for (int i = 0; i < v.nrows; i++)
        if (!strcmp(v.rows[i].label, label)) return v.rows[i].value;
    return NULL;
}

/* replace the text of the address field */
static void retype(Node *nd, const char *s)
{
    for (size_t n = strlen(view(nd).input); n > 0; n--) ev(nd, LE_BACKSPACE);
    type(nd, s);
}

static void test_flow(void)
{
    Node A, B;
    node_init(&A, "Alice", 7);
    node_init(&B, "Bob", 7);
    nodes[0] = &A, nodes[1] = &B, n_nodes = 2;
    lobby_open(A.l);
    lobby_open(B.l);
    lobby_view v = view(&A);
    CHECK(v.screen == LS_TOP && v.nrows == 3 && v.sel == 0);
    CHECK(!strcmp(v.rows[0].label, "Find a Partner"));

    /* host: top -> way -> direct link -> room */
    press(&A, "Find a Partner");
    CHECK(view(&A).screen == LS_HOST_WAY);
    ev(&A, LE_BACK);
    CHECK(view(&A).screen == LS_TOP);
    press(&A, "Find a Partner");
    press(&A, "System Link");
    step(200);
    v = view(&A);
    CHECK(v.screen == LS_ROOM);
    CHECK(cs_active(A.s) && cs_is_host(A.s));
    CHECK(!strcmp(v.status, lobby_own_text(LT_WAITING, 0)));
    CHECK(strstr(v.subtitle, "192.168.1.5:7") != NULL);
    cp_session ss;
    cs_session_get(A.s, &ss);
    CHECK(ss.episode == 1 && ss.part == 0 && ss.difficulty == 1);
    CHECK(row_kind(&A, "Start Game") == LV_GREY);   /* alone: cannot start */
    CHECK(A.saves > 0);

    /* client: top -> way -> link -> IP field */
    press(&B, "Join a Partner");
    CHECK(view(&B).screen == LS_JOIN_WAY);
    press(&B, "System Link");
    v = view(&B);
    CHECK(v.screen == LS_JOIN_IP && v.input_on);
    CHECK(row_kind(&B, "Join a Partner") == LV_GREY);   /* empty address */
    type(&B, "sim:7x");
    ev(&B, LE_BACKSPACE);
    CHECK(!strcmp(view(&B).input, "sim:7"));
    strcpy(B.clip, "xx");
    ev(&B, LE_BACKSPACE), ev(&B, LE_BACKSPACE), ev(&B, LE_BACKSPACE), ev(&B, LE_BACKSPACE), ev(&B, LE_BACKSPACE);
    CHECK(!view(&B).input[0]);
    strcpy(B.clip, "sim:7\r\nignored");
    ev(&B, LE_PASTE);
    CHECK(!strcmp(view(&B).input, "sim:7"));
    ev(&B, LE_OK);   /* Enter in the field connects */
    step(40);
    CHECK(view(&B).screen == LS_CONNECTING || view(&B).screen == LS_ROOM);
    step(1500);
    v = view(&B);
    CHECK(v.screen == LS_ROOM);
    CHECK(v.nplayers == 2);
    CHECK(!strcmp(view(&A).status, lobby_own_text(LT_PARTNER_JOINED, 0)));
    step(3100);
    CHECK(!strcmp(view(&A).status, lobby_own_text(LT_NEED_READY, 0)));   /* partner joined, not ready */
    cs_player_info pi;
    CHECK(cs_player(A.s, 0, &pi) && pi.character == CP_CHAR_CLAIRE);
    CHECK(cs_player(A.s, 1, &pi) && pi.character == CP_CHAR_MOIRA);

    /* characters: rows, the partner's slot is grey */
    CHECK(row_kind(&A, "Moira") == LV_GREY && row_kind(&A, "Claire") == LV_NORMAL);
    CHECK(!strcmp(row_value(&A, "Moira"), "Bob"));
    CHECK(!strcmp(row_value(&A, "Claire"), lobby_own_text(LT_YOU, 0)));
    CHECK(row_kind(&B, "Barry") == LV_GREY && row_kind(&B, "Natalia") == LV_GREY);   /* the other half of the story */
    CHECK(row_kind(&B, "Claire") == LV_GREY);   /* taken by the host */

    /* the client asks for a taken character: refused, status shows the stock message */
    lobby_event click = {LE_CLICK, 0, 0};
    for (int i = 0; i < view(&B).nrows; i++)
        if (!strcmp(view(&B).rows[i].label, "Claire")) click.row = i;
    ev(&B, LE_DOWN);   /* (the click below only selects a grey row: it must not do anything) */
    lobby_event_in(B.l, &click);
    CHECK(cs_player(A.s, 1, &pi) && pi.character == CP_CHAR_MOIRA);

    /* the host picks Barry: the story half switches and the client follows with Natalia */
    press(&A, "Barry");
    step(1500);
    cs_session_get(A.s, &ss);
    CHECK(ss.part == 1);
    CHECK(cs_player(A.s, 0, &pi) && pi.character == CP_CHAR_BARRY);
    CHECK(cs_player(A.s, 1, &pi) && pi.character == CP_CHAR_NATALIA);
    CHECK(row_kind(&B, "Natalia") == LV_NORMAL && row_kind(&B, "Moira") == LV_GREY);
    /* and back */
    press(&A, "Claire");
    step(1500);
    cs_session_get(A.s, &ss);
    CHECK(ss.part == 0);
    CHECK(cs_player(A.s, 1, &pi) && pi.character == CP_CHAR_MOIRA);

    /* the host cannot even reach the partner's slot with the cursor */
    CHECK(goto_row(&A, "Moira") < 0);

    /* episode and difficulty: left/right on the host's rows, the client sees the values */
    goto_row(&A, "Episode");
    ev(&A, LE_RIGHT);
    step(300);
    cs_session_get(A.s, &ss);
    CHECK(ss.episode == 2);
    CHECK(!strcmp(row_value(&B, "Episode"), "2"));
    ev(&A, LE_LEFT), ev(&A, LE_LEFT);
    step(300);
    cs_session_get(A.s, &ss);
    CHECK(ss.episode == 4);   /* wraps */
    ev(&A, LE_RIGHT);
    step(300);
    goto_row(&A, "Difficulty");
    ev(&A, LE_RIGHT);
    step(300);
    cs_session_get(A.s, &ss);
    CHECK(ss.difficulty == 2 && ss.episode == 1);
    CHECK(!strcmp(row_value(&B, "Difficulty"), lobby_own_text(LT_HARD, 0)));
    CHECK(strlen(row_value(&A, "Difficulty")) > 0);
    CHECK(row_kind(&B, "Difficulty") == LV_INFO);

    /* start only when the partner is ready */
    CHECK(row_kind(&A, "Start Game") == LV_GREY);
    CHECK(goto_row(&A, "Start Game") < 0);
    CHECK(cs_phase(A.s) == CP_PHASE_LOBBY);
    press(&B, "Ready");
    step(300);
    CHECK(cs_player(A.s, 1, &pi) && pi.ready);
    CHECK(row_kind(&A, "Start Game") == LV_NORMAL);
    CHECK(!strcmp(row_value(&B, "Ready"), "[X]"));
    press(&A, "Start Game");
    step(600);
    CHECK(cs_phase(A.s) == CP_PHASE_LOADING);
    CHECK(lobby_take_request(A.l) == LR_START);
    CHECK(lobby_take_request(A.l) == LR_NONE);
    CHECK(lobby_take_request(B.l) == LR_START);
    int ep, part, df, ch, gt;
    lobby_start_params(A.l, &ep, &part, &df, &ch, &gt);
    CHECK(ep == 1 && part == 0 && df == 2 && ch == CP_CHAR_CLAIRE && gt == 0);
    lobby_start_params(B.l, &ep, &part, &df, &ch, &gt);
    CHECK(ep == 1 && part == 0 && df == 2 && ch == CP_CHAR_MOIRA);
    CHECK(lobby_session_running(A.s) && lobby_session_running(B.s));

    cs_free(A.s), cs_free(B.s);
    lobby_free(A.l), lobby_free(B.l);
    n_nodes = 0;
}

static void test_leave_and_errors(void)
{
    Node A, B;
    node_init(&A, "Alice", 8);
    node_init(&B, "Bob", 8);
    nodes[0] = &A, nodes[1] = &B, n_nodes = 2;
    lobby_open(A.l);
    lobby_open(B.l);

    /* joining nobody: back to the address screen with the stock error */
    press(&B, "Join a Partner");
    press(&B, "System Link");
    type(&B, "sim:99");
    ev(&B, LE_OK);
    step(300);
    lobby_view v = view(&B);
    CHECK(v.screen == LS_JOIN_IP);
    CHECK(v.status_kind == LSK_ERROR && strstr(v.status, lobby_own_text(LT_FAIL_JOIN, 0)) != NULL);
    CHECK(!cs_active(B.s));
    CHECK(!strcmp(v.input, "sim:99"));   /* the typed address stays for another try */

    /* Esc while connecting cancels (a live host, cancelled before it answers) */
    press(&A, "Find a Partner");
    press(&A, "System Link");
    step(100);
    retype(&B, "sim:8");
    ev(&B, LE_OK);
    CHECK(view(&B).screen == LS_CONNECTING);
    ev(&B, LE_BACK);
    CHECK(view(&B).screen == LS_JOIN_WAY);
    step(50);   /* the attempt had already started: it is closed when its job reports back */
    CHECK(!cs_active(B.s));
    ev(&B, LE_BACK);
    CHECK(view(&B).screen == LS_TOP);
    ev(&B, LE_BACK);
    CHECK(lobby_take_request(B.l) == LR_CLOSE);
    step(500);
    CHECK(cs_player_count(A.s) == 1);

    /* a normal session, then the client leaves with the question */
    press(&B, "Join a Partner");
    press(&B, "System Link");
    retype(&B, "sim:8");
    ev(&B, LE_OK);
    step(2500);
    CHECK(view(&B).screen == LS_ROOM);
    ev(&B, LE_BACK);
    CHECK(view(&B).screen == LS_LEAVE_ASK);
    CHECK(strstr(view(&B).prompt, "Quit session?") != NULL);
    CHECK(view(&B).sel == 1);   /* "No" is under the cursor */
    
    ev(&B, LE_BACK);   /* Esc = No */
    CHECK(view(&B).screen == LS_ROOM);
    press(&B, "Leave");
    CHECK(view(&B).screen == LS_LEAVE_ASK);
    press(&B, "Yes");
    CHECK(view(&B).screen == LS_TOP && !cs_active(B.s));
    step(1500);
    CHECK(view(&A).screen == LS_ROOM && cs_player_count(A.s) == 1);
    CHECK(!strcmp(view(&A).status, lobby_own_text(LT_WAITING, 0)));

    /* the host leaves: alone, no question */
    press(&A, "Leave");
    CHECK(view(&A).screen == LS_TOP && !cs_active(A.s));

    /* the host disappears under a client: the client gets the stock "connection terminated" */
    press(&A, "Find a Partner");
    press(&A, "System Link");
    step(100);
    press(&B, "Join a Partner");
    press(&B, "System Link");
    CHECK(!strcmp(view(&B).input, "sim:8"));   /* remembered from the last successful try */
    ev(&B, LE_OK);
    step(2500);
    CHECK(view(&B).screen == LS_ROOM);
    cs_leave(A.s, 0);
    step(3000);
    v = view(&B);
    CHECK(v.screen == LS_TOP && v.status_kind == LSK_ERROR);
    CHECK(!cs_active(B.s));

    /* a session that appears while the player sits on the top screen (a Join game click, the console): the room opens by itself */
    lobby_close(A.l);
    cs_leave(A.s, 0);
    lobby_open(A.l);
    CHECK(view(&A).screen == LS_TOP);
    CHECK(cs_host(A.s, "sim", 11, 0, (char[100]){0}, 100));
    step(20);
    CHECK(view(&A).screen == LS_ROOM);

    /* a session that exists when the screen opens: straight into the room */
    lobby_close(A.l);
    cs_leave(A.s, 0);
    CHECK(cs_host(A.s, "sim", 9, 0, (char[100]){0}, 100));
    lobby_open(A.l);
    CHECK(view(&A).screen == LS_ROOM);

    cs_free(A.s), cs_free(B.s);
    lobby_free(A.l), lobby_free(B.l);
    n_nodes = 0;
}

static void test_friends_and_steam(void)
{
    Node A;
    node_init(&A, "Alice", 10);
    nodes[0] = &A, n_nodes = 1;
    lobby_settings st;   /* no name in the settings: the Steam persona name is taken once */
    lobby_get_settings(A.l, &st);
    st.name[0] = 0;
    lobby_set_settings(A.l, &st);
    lobby_open(A.l);
    step(20);
    lobby_get_settings(A.l, &st);
    CHECK(!strcmp(st.name, "Persona") && !strcmp(A.local_name, "Persona"));
    press(&A, "Join a Partner");
    press(&A, "Friend");
    step(20);
    lobby_view v = view(&A);
    CHECK(v.screen == LS_FRIENDS);
    CHECK(v.nrows == 4);   /* Alice's public lobby, Bob's friend lobby, Refresh, Back (Carl has no lobby) */
    CHECK(!strcmp(v.rows[0].label, "Alice") || !strcmp(v.rows[1].label, "Alice"));
    CHECK(!strcmp(v.rows[0].label, "Bob") || !strcmp(v.rows[1].label, "Bob"));
    /* selecting a friend runs steamjoin, which the test platform answers with "not ready" */
    ev(&A, LE_OK);
    step(100);
    v = view(&A);
    CHECK(v.screen == LS_FRIENDS && v.status_kind == LSK_ERROR && strstr(v.status, lobby_own_text(LT_STEAM_NA, 0)) != NULL);
    /* the way of hosting through Steam in a process without Steam */
    ev(&A, LE_BACK);
    ev(&A, LE_BACK);
    CHECK(view(&A).screen == LS_TOP);
    press(&A, "Find a Partner");
    press(&A, "Friend");
    step(100);
    v = view(&A);
    CHECK(v.screen == LS_HOST_WAY && v.status_kind == LSK_ERROR);
    cs_free(A.s);
    lobby_free(A.l);
    n_nodes = 0;
}

static void test_texts(void)
{
    for (int t = 0; t < LT_COUNT; t++) {
        CHECK(lobby_own_text(t, 0)[0] != 0);
        for (int lang = 0; lang < 12; lang++) CHECK(lobby_own_text(t, lang)[0] != 0);
        if (lobby_stock_ids[t].table < 0) {   /* own strings are translated into all 11 other languages */
            int same = 0;
            for (int lang = 1; lang < 12; lang++) same += !strcmp(lobby_own_text(t, lang), lobby_own_text(t, 0));
            if (t != LT_PING && same > 3) {
                printf("text %d: %d languages still English\n", t, same);
                fails++;
            }
        }
    }
    CHECK(lobby_lang_from_game(1) == 0 && lobby_lang_from_game(10) == 5 && lobby_lang_from_game(21) == 7 && lobby_lang_from_game(0) == 8);
    /* 6i: the banner text is used as a printf format with ONE %s (the awaited player's name) in every language */
    for (int lang = 0; lang < 12; lang++) {
        const char *f = lobby_own_text(LT_WAIT_LOAD, lang);
        int n = 0;
        for (const char *p = f; *p; p++)
            if (*p == '%') n += p[1] == 's' ? 1 : 100;
        CHECK(n == 1);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_texts();
    test_flow();
    test_leave_and_errors();
    test_friends_and_steam();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
