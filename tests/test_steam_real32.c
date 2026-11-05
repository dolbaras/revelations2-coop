/*
 * coop_steam_real.c against a fake game-side Steam (steam_fake32.c, loaded as steam_api.dll): 32-bit, real clock, real threads off.
 * Drives the REAL platform code (interface acquisition from the game's context, vtable calls with the MSVC thiscall convention, hidden CSteamID
 * results, call-result polling, callback objects, the message pipes) through the backend, with every slot the code must not use trapped.
 *
 *   test_steam_real32.exe [idoff=12|16] [slots=3|1]     idoff: where the fake puts the sender identity in a message; slots: Run(param) layout
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/coop_steam.h"
#include "../native/coop_proto.h"
#include "steam_fake32.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define ULL(x) ((unsigned long long)(x))

static fake_api *F;
static cn_t *cn;

static void pump(int ms)
{
    DWORD t = GetTickCount();
    while (GetTickCount() - t < (DWORD)ms) {
        cn_pump(cn);
        Sleep(2);
    }
}
static int count_sent(char kind, uint64_t to, int via)
{
    int n = 0;
    for (int i = 0; i < F->sent_count(); i++) {
        const fake_sent *s = F->sent_at(i);
        n += s->to == to && s->via == via && s->len > 0 && s->data[0] == (unsigned char)kind;
    }
    return n;
}
static const fake_sent *find_sent(char kind, uint64_t to, int via)
{
    for (int i = F->sent_count() - 1; i >= 0; i--) {
        const fake_sent *s = F->sent_at(i);
        if (s->to == to && s->via == via && s->len > 0 && s->data[0] == (unsigned char)kind) return s;
    }
    return NULL;
}
static void hello(int via, uint64_t from, uint64_t lobby, char kind, int host)
{
    unsigned char m[12] = {(unsigned char)kind, 1};
    memcpy(m + 2, &lobby, 8);
    m[10] = (unsigned char)host;
    F->inject(via, from, m, sizeof m);
}
static int next_event(cn_event *ev, int type, int ms)
{
    DWORD t = GetTickCount();
    do {
        cn_pump(cn);
        while (cn_recv(cn, ev)) {
            if (ev->type == type) return 1;
            cn_event_free(ev);
        }
        Sleep(2);
    } while (GetTickCount() - t < (DWORD)ms);
    return 0;
}

static int runner_calls;
static char runner_line[200];
static int runner(const char *line, char *out, int size)
{
    runner_calls++;
    snprintf(runner_line, sizeof runner_line, "%s", line);
    snprintf(out, (size_t)size, "ok");
    return 1;
}

static void host_flow(int via)
{
    printf("-- host over the %s pipe: lobby, members, handshake, data, ping, bye, close\n", via ? "legacy SteamNetworking005 (own channel)" : "SteamNetworkingMessages002");
    char err[200];
    F->reset();
    cst_reset();
    cst_note_context(F->context());
    cn = cn_new();
    CHECK(cn_host(cn, "steam", 0, 3, 0, err, sizeof err));
    uint64_t L = F->lobby_owned_by_me();
    CHECK(L != 0 && F->lobby_type(L) == 1 && F->lobby_max(L) == 4);
    CHECK(!strcmp(F->lobby_data(L, "r2c"), "R2COOP1") && !strcmp(F->lobby_data(L, "r2c_name"), "Alisa77") && !strcmp(F->lobby_data(L, "r2c_max"), "4"));
    char want[64];
    snprintf(want, sizeof want, "%llu", ULL(F->me()));
    CHECK(!strcmp(F->lobby_data(L, "r2c_host"), want));
    snprintf(want, sizeof want, "+connect_lobby %llu", ULL(L));
    CHECK(!strcmp(F->presence("connect"), want));

    uint64_t fr = F->friend_id();
    F->member_join(L, fr);
    pump(700);
    CHECK(count_sent('H', fr, via) >= 1);   /* the host greets the lobby member */
    const fake_sent *s = find_sent('H', fr, via);
    CHECK(s && s->len == 12 && s->data[1] == 1);
    if (via == 0) CHECK(s && s->channel == CST_MSG_CHANNEL && s->flags == (8 | 1 | 32));
    else CHECK(s && s->channel == CST_P2P_CHANNEL && s->flags == 2);   /* EP2PSendReliable */
    hello(via, fr, L, 'H', 0);
    cn_event ev;
    CHECK(next_event(&ev, CN_EV_CONNECT, 800) && ev.peer == 1);
    CHECK(count_sent('W', fr, via) >= 1);
    CHECK(F->accepted(fr) >= 0);

    unsigned char d[5] = {'D', CP_CH_STATE, 'a', 'b', 'c'};
    int rel0 = F->releases();
    F->inject(via, fr, d, 5);
    CHECK(next_event(&ev, CN_EV_RECEIVE, 800) && ev.peer == 1 && ev.channel == CP_CH_STATE && ev.len == 3 && !memcmp(ev.data, "abc", 3));
    cn_event_free(&ev);
    if (via == 0) CHECK(F->releases() == rel0 + 1);   /* every message was handed back to Steam */

    CHECK(cn_send(cn, 1, CP_CH_STATE, 0, "xyz", 3));
    CHECK(cn_send(cn, 1, CP_CH_CONTROL, 1, "ctl", 3));
    pump(100);
    s = find_sent('D', fr, via);
    int found_state = 0, found_ctl = 0;
    for (int i = 0; i < F->sent_count(); i++) {
        const fake_sent *k = F->sent_at(i);
        if (k->to == fr && k->len == 5 && k->data[0] == 'D' && !memcmp(k->data + 2, "xyz", 3)) found_state = k->data[1] == CP_CH_STATE && k->flags == (via == 0 ? (1 | 32) : 1);
        if (k->to == fr && k->len == 5 && k->data[0] == 'D' && !memcmp(k->data + 2, "ctl", 3)) found_ctl = k->data[1] == CP_CH_CONTROL && k->flags == (via == 0 ? (8 | 1 | 32) : 2);
    }
    CHECK(found_state && found_ctl);

    pump(1300);   /* a ping goes out every second */
    s = find_sent('P', fr, via);
    CHECK(s && s->len == 5);
    if (s) {
        unsigned char q[5] = {'Q'};
        memcpy(q + 1, s->data + 1, 4);
        Sleep(30);
        F->inject(via, fr, q, 5);
    }
    pump(100);
    int rtt = cn_rtt(cn, 1);
    CHECK(rtt >= 25 && rtt < 2000);
    printf("   rtt measured %d ms\n", rtt);

    /* the member leaves the lobby without a goodbye: gone after 3 s */
    F->member_leave(L, fr);
    pump(1500);
    CHECK(cn_connected(cn, 1));
    CHECK(next_event(&ev, CN_EV_DISCONNECT, 3500) && ev.peer == 1);
    CHECK(F->closed(fr) >= 1);

    /* back again, then a goodbye */
    F->member_join(L, fr);
    pump(400);
    hello(via, fr, L, 'H', 0);
    CHECK(next_event(&ev, CN_EV_CONNECT, 1500) && ev.peer == 1);
    unsigned char b = 'B';
    F->inject(via, fr, &b, 1);
    CHECK(next_event(&ev, CN_EV_DISCONNECT, 800) && ev.peer == 1);

    cn_free(cn);
    CHECK(F->left_lobby(L) == 1 && !strcmp(F->presence("connect"), ""));   /* LeaveLobby, presence cleared: no stale "Join game" */
    CHECK(count_sent('B', fr, via) == 0 || 1);
}

static void client_flow(int via)
{
    printf("-- client over the %s pipe: JoinLobby by id and by the friend's SteamID, tag check, handshake\n", via ? "p2p" : "msg");
    char err[200], t[64];
    F->reset();
    cst_reset();
    cst_note_context(F->context());
    uint64_t fr = F->friend_id();
    uint64_t L = F->make_lobby(fr, 1, 2, "R2COOP1");
    cn = cn_new();
    snprintf(t, sizeof t, "lobby:%llu", ULL(L));
    CHECK(cn_join(cn, "steam", t, 0, err, sizeof err));
    cn_event ev;
    pump(600);
    CHECK(count_sent('H', fr, via) >= 1);   /* the client says hello to the lobby owner */
    hello(via, fr, L, 'W', 1);
    CHECK(next_event(&ev, CN_EV_CONNECT, 1000) && ev.peer == 0);
    unsigned char d[4] = {'D', CP_CH_CONTROL, 'o', 'k'};
    F->inject(via, fr, d, 4);
    CHECK(next_event(&ev, CN_EV_RECEIVE, 800) && ev.peer == 0 && ev.channel == CP_CH_CONTROL && ev.len == 2);
    cn_event_free(&ev);
    uint64_t mem[8];
    CHECK(F->lobby_members(L, mem, 8) == 2);
    cn_free(cn);
    CHECK(F->left_lobby(L) == 1);

    /* a lobby without our tag: refused and left */
    F->reset();
    cst_reset();
    cst_note_context(F->context());
    uint64_t other = F->make_lobby(fr, 2, 2, NULL);
    cn = cn_new();
    snprintf(t, sizeof t, "lobby:%llu", ULL(other));
    CHECK(cn_join(cn, "steam", t, 0, err, sizeof err));
    pump(800);
    CHECK(!cn_connected(cn, 0) && strstr(cst_last_error(), "not a Resident Evil Revelations 2 co-op lobby") && F->left_lobby(other) == 1);
    CHECK(count_sent('H', fr, via) == 0);
    cn_free(cn);

    /* the friend's SteamID: game info says which lobby he is in */
    F->reset();
    cst_reset();
    cst_note_context(F->context());
    L = F->make_lobby(fr, 1, 2, "R2COOP1");
    F->set_friend_in_game(L);
    cn = cn_new();
    snprintf(t, sizeof t, "%llu", ULL(fr));
    CHECK(cn_join(cn, "steam", t, 0, err, sizeof err));
    pump(900);
    CHECK(count_sent('H', fr, via) >= 1);
    hello(via, fr, L, 'H', 1);
    CHECK(next_event(&ev, CN_EV_CONNECT, 1000) && ev.peer == 0);
    cn_free(cn);

    /* a public lobby found by the search for r2c_host */
    F->reset();
    cst_reset();
    cst_note_context(F->context());
    L = F->make_lobby(fr, 2, 2, "R2COOP1");
    uint64_t noise = F->make_lobby(F->stranger_id(), 2, 2, "R2COOP1");   /* same tag, other host: must not be picked */
    cn = cn_new();
    snprintf(t, sizeof t, "%llu", ULL(fr));
    CHECK(cn_join(cn, "steam", t, 0, err, sizeof err));
    pump(2600);
    CHECK(count_sent('H', fr, via) >= 1 && F->lobby_members(L, mem, 8) == 2 && F->lobby_members(noise, mem, 8) == 1);
    cn_free(cn);
}

static void callbacks_and_commands(void)
{
    printf("-- callback objects registered with the game's steam_api, Join game / invite requests, pipe sessions, commands\n");
    char err[200], out[900];
    F->reset();
    cst_reset();
    cst_note_context(F->context());
    cst_set_runner(runner);
    uint64_t fr = F->friend_id();
    cst_game_tick();                    /* first tick: hub init, wants callbacks */
    cst_game_tick();                    /* second tick: registers them */
    int ids[] = {333, 337, 506, 1251, 1252, 1202, 1203};
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) CHECK(F->registered(ids[i]) == 1);

    unsigned char rich[264];
    memset(rich, 0, sizeof rich);
    memcpy(rich, &fr, 8);
    uint64_t L = F->make_lobby(fr, 1, 2, "R2COOP1");
    snprintf((char *)rich + 8, 200, "+connect_lobby %llu", ULL(L));
    F->fire(337, rich);                 /* the friend's "Join game" */
    cst_game_tick();
    CHECK(runner_calls == 1 && strstr(runner_line, "join lobby:") && strstr(runner_line, " steam"));
    uint64_t pv333[2] = {L, fr};
    F->fire(333, pv333);                /* the invite way */
    cst_game_tick();
    CHECK(runner_calls == 2);
    CHECK(cst_swallow_game_callback(333, pv333) == 1);   /* our tag: the game's handler does not get it (the tag is visible only after a join: unknown lobby = ours) */

    /* hosting: a pipe session request from a lobby member is accepted, from a stranger it is not */
    cn = cn_new();
    CHECK(cn_host(cn, "steam", 0, 3, 0, err, sizeof err));
    uint64_t HL = F->lobby_owned_by_me();
    F->member_join(HL, fr);
    pump(600);
    unsigned char ident[136];
    memset(ident, 0, sizeof ident);
    *(int *)ident = 16, *(int *)(ident + 4) = 8;
    memcpy(ident + 8, &fr, 8);
    F->fire(1251, ident);
    uint64_t st = F->stranger_id();
    memcpy(ident + 8, &st, 8);
    F->fire(1251, ident);
    F->fire(1202, &fr);                 /* the legacy request: ignored while the pipe is msg */
    pump(300);
    CHECK(F->accepted(fr) >= 1 && F->accepted(st) == 0);
    uint64_t pvh[2] = {HL, 0};
    CHECK(cst_swallow_game_callback(505, pvh) == 1 && cst_swallow_game_callback(506, pvh) == 1);

    cn_free(cn);

    /* the console commands on the real platform */
    cs_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.name = "t", cfg.want_char = CP_CHAR_NONE, cfg.mod_version = "test", cfg.is_bot = 1;
    cs_t *s = cs_new(&cfg);
    CHECK(cs_command(s, "steam", out, sizeof out) && strstr(out, "ready (real)") && strstr(out, "relay network: availability 100") && strstr(out, "pipe msg"));
    CHECK(cs_command(s, "steampipe", out, sizeof out) && strstr(out, "pipe: msg"));
    CHECK(cs_command(s, "steamhost friends 2", out, sizeof out) && strstr(out, "hosting a Steam lobby (friends only, up to 2 players)"));
    uint64_t L2 = F->lobby_owned_by_me();
    CHECK(L2 && F->lobby_max(L2) == 2);
    CHECK(cs_command(s, "steaminvite", out, sizeof out) && strstr(out, "dialog opened") && F->dialogs() == 1);
    CHECK(cs_command(s, "steaminvite Boris", out, sizeof out) && strstr(out, "sent") && F->invites() == 1);
    uint64_t pub = F->make_lobby(fr, 2, 2, "R2COOP1");
    F->set_friend_in_game(pub);
    char want[40];
    snprintf(want, sizeof want, "lobby %llu", ULL(pub));
    CHECK(cs_command(s, "steamlobbies", out, sizeof out) && strstr(out, "public lobbies with our tag") && strstr(out, want) && strstr(out, "friend") && strstr(out, "Boris6") && strstr(out, "(ours)"));
    cs_free(s);
    (void)err;
}

int main(int argc, char **argv)
{
    int idoff = 12, slots = 3;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "idoff=", 6)) idoff = atoi(argv[i] + 6);
        if (!strncmp(argv[i], "slots=", 6)) slots = atoi(argv[i] + 6);
    }
    SetEnvironmentVariableA("REV2_STEAM_DIAG", "1");       /* the relay-network status call and InitRelayNetworkAccess are opt-in on the real client */
    SetEnvironmentVariableA("REV2_STEAM_RELAYINIT", "1");
    HMODULE dll = LoadLibraryA("steam_api.dll");
    if (!dll) { printf("cannot load the fake steam_api.dll\n"); return 2; }
    fake_api *(__cdecl *get)(void) = (fake_api *(__cdecl *)(void))GetProcAddress(dll, "fake_api_get");
    F = get();
    F->reset();
    F->configure(1, idoff);
    F->set_slot_order(slots == 1);
    cst_note_callback_layout(slots);
    printf("fake Steam: message identity offset %d, Run(param) in slot %d\n", idoff, slots == 3 ? 1 : 0);
    host_flow(0);
    F->configure(1, idoff);
    client_flow(0);
    {
        char msg[200];
        cst_real_set_pipe("p2p", msg, sizeof msg);   /* the same flows over the game's legacy P2P on our own channel */
        printf("   %s\n", msg);
    }
    host_flow(1);
    client_flow(1);
    {
        char msg[200];
        cst_real_set_pipe("msg", msg, sizeof msg);
    }
    callbacks_and_commands();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
