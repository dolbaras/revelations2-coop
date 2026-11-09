/*
 * test_lobbymust.c: the 0.2.0 in-game rules of native/lobbymust_logic.h (a lost partner / host, End Co-Op, the end of Episode 1) driven with the orders the
 * glue sees: notes from the session, then passes with game facts. Also checks that the values the header repeats are the session's.
 */
#include <stdio.h>
#include <string.h>

#include "../native/coop_session.h"
#include "../native/lobbymust_logic.h"

static int bad, n;
#define CHECK(c) do { n++; if (!(c)) { bad++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static lm_frame_in play(void)   /* plain play in Barry's half, session running */
{
    lm_frame_in f;
    memset(&f, 0, sizeof f);
    f.session_running = 1, f.glue_running = 1, f.in_episode = 1, f.agame_state = 3, f.area = 153, f.episode = 1, f.part = 1;
    return f;
}

static lm_note_in note(int what, int a, int b, int is_host)
{
    lm_note_in x = {what, a, b, 1, 1, 1, is_host};
    return x;
}

/* run passes of 16 ms from t for ms milliseconds; returns the OR of the actions, *first = the time the first action came */
static int run(lm_t *m, const lm_frame_in *f, uint32_t *t, int ms, uint32_t *first)
{
    int all = 0;
    for (int k = 0; k < ms; k += 16) {
        *t += 16;
        int a = lm_tick(m, f, *t);
        if (a && first && !*first) *first = *t;
        all |= a;
    }
    return all;
}

static void test_values(void)
{
    CHECK(LM_N_LOST == CS_N_LOST && LM_N_PEER_LEAVE == CS_N_PEER_LEAVE);
    CHECK(LM_GONE_REPLACED == CS_GONE_REPLACED && LM_GONE_REMOVED == CS_GONE_REMOVED && LM_GONE_EP_END == CS_GONE_EP_END && LM_LEAVE_EP_END == CP_LEAVE_EP_END);
}

static void test_host_partner_left(void)
{
    lm_t m;
    uint32_t t = 100000, first = 0;
    lm_frame_in f = play();
    /* product: strip + AI at once, the solo step 1.5 s later in plain play, once */
    lm_init(&m, 1);
    lm_note_in x = note(CS_N_PEER_LEAVE, 0, CS_GONE_LEFT, 1);
    int a = lm_on_note(&m, &x, t);
    CHECK(a == (LM_A_RELEASE | LM_A_STRIP) && m.strip == LM_S_PARTNER_LEFT);
    a = run(&m, &f, &t, 1400, &first);
    CHECK(a == 0);
    first = 0;
    a = run(&m, &f, &t, 300, &first);
    CHECK(a == LM_A_SOLO && first);
    CHECK(run(&m, &f, &t, 5000, NULL) == 0 && m.n_solo == 1);
    /* lost without a goodbye / given up after the silence limit: the "lost" strip */
    lm_init(&m, 1);
    x = note(CS_N_PEER_LEAVE, 1, CS_GONE_LEFT, 1);
    CHECK(lm_on_note(&m, &x, t) == (LM_A_RELEASE | LM_A_STRIP) && m.strip == LM_S_PARTNER_LOST);
    x = note(CS_N_PEER_LEAVE, 1, CS_GONE_SILENT, 1);
    CHECK(lm_on_note(&m, &x, t) == (LM_A_RELEASE | LM_A_STRIP) && m.strip == LM_S_PARTNER_LOST);
    /* the lab (rejoin allowed): strip + AI, no solo step, the session stays open for the returning player */
    lm_init(&m, 0);
    x = note(CS_N_PEER_LEAVE, 0, CS_GONE_LEFT, 1);
    CHECK(lm_on_note(&m, &x, t) == (LM_A_RELEASE | LM_A_STRIP));
    CHECK(run(&m, &f, &t, 5000, NULL) == 0 && m.n_solo == 0);
    /* the lab's rejoin replaced the old link: nothing to show */
    x = note(CS_N_PEER_LEAVE, 1, CS_GONE_REPLACED, 1);
    CHECK(lm_on_note(&m, &x, t) == LM_A_RELEASE && m.strip == LM_S_NONE);
    /* someone else than the partner (a third pid), or before the start: nothing */
    lm_init(&m, 1);
    x = note(CS_N_PEER_LEAVE, 0, CS_GONE_LEFT, 1);
    x.pid = 2;
    CHECK(lm_on_note(&m, &x, t) == 0);
    x = note(CS_N_PEER_LEAVE, 0, CS_GONE_LEFT, 1);
    x.in_game = 0;
    CHECK(lm_on_note(&m, &x, t) == 0 && !m.solo_at);
}

static void test_solo_waits_for_plain_play(void)
{
    lm_t m;
    uint32_t t = 5000, first = 0;
    lm_init(&m, 1);
    lm_note_in x = note(CS_N_PEER_LEAVE, 1, CS_GONE_LEFT, 1);
    lm_on_note(&m, &x, t);
    /* a level load (glue not running), then a cutscene, then the pause menu: the step waits through all of them */
    lm_frame_in f = play();
    f.glue_running = 0, f.in_episode = 0, f.agame_state = 8;
    CHECK(run(&m, &f, &t, 20000, NULL) == 0);
    f = play(), f.demo = 1;
    CHECK(run(&m, &f, &t, 20000, NULL) == 0);
    f = play(), f.paused = 1;
    CHECK(run(&m, &f, &t, 5000, NULL) == 0);
    f = play();
    CHECK(run(&m, &f, &t, 50, &first) == LM_A_SOLO);
    /* never plain play again for 10 minutes: dropped (the AI partner stays) */
    lm_init(&m, 1);
    lm_on_note(&m, &x, t);
    f = play(), f.demo = 1;
    CHECK(run(&m, &f, &t, LM_SOLO_GIVEUP_MS + 2000, NULL) == 0 && !m.solo_at);
    f = play();
    CHECK(run(&m, &f, &t, 3000, NULL) == 0);
    /* back at the title first: nothing pending any more */
    lm_init(&m, 1);
    lm_on_note(&m, &x, t);
    f = play(), f.at_menus = 1, f.agame_state = -1;
    CHECK(run(&m, &f, &t, 100, NULL) == 0 && !m.solo_at);
}

static void test_end_coop(void)
{
    lm_t m;
    uint32_t t = 7000;
    lm_frame_in f = play();
    /* the host's End Co-Op: strip + the solo step, also in the lab (the host chose to play alone); the PEER_LEAVE that cs_kick raises adds nothing new */
    lm_init(&m, 0);
    CHECK(lm_end_coop(&m, t) == (LM_A_RELEASE | LM_A_STRIP) && m.strip == LM_S_PARTNER_LEFT);
    lm_note_in x = note(CS_N_PEER_LEAVE, 0, CS_GONE_REMOVED, 1);
    lm_on_note(&m, &x, t + 5);
    CHECK(run(&m, &f, &t, 2000, NULL) == LM_A_SOLO && m.n_solo == 1);
    CHECK(run(&m, &f, &t, 5000, NULL) == 0);
}

static void test_guest_host_gone(void)
{
    lm_t m;
    uint32_t t = 20000, first = 0;
    lm_frame_in f = play();
    f.session_running = 0, f.glue_running = 0;   /* the guest's session is gone with the note */
    /* the host quit / ended co-op: "ended" strip, the quit 5 s later in plain play, once */
    lm_init(&m, 1);
    lm_note_in x = note(CS_N_LOST, 1, CP_LEAVE_QUIT, 0);
    CHECK(lm_on_note(&m, &x, t) == (LM_A_RELEASE | LM_A_STRIP) && m.strip == LM_S_HOST_ENDED);
    CHECK(run(&m, &f, &t, 4900, NULL) == 0);
    CHECK(run(&m, &f, &t, 200, &first) == LM_A_QUIT);
    CHECK(run(&m, &f, &t, 10000, NULL) == 0 && m.n_quit == 1);
    x = note(CS_N_LOST, 1, CP_LEAVE_END_COOP, 0);
    lm_init(&m, 1);
    CHECK(lm_on_note(&m, &x, t) == (LM_A_RELEASE | LM_A_STRIP) && m.strip == LM_S_HOST_ENDED);
    /* the link lost / the host silent for the limit: "lost" strip, the same quit */
    lm_init(&m, 1);
    x = note(CS_N_LOST, 0, 0, 0);
    CHECK(lm_on_note(&m, &x, t) == (LM_A_RELEASE | LM_A_STRIP) && m.strip == LM_S_HOST_LOST);
    x = note(CS_N_LOST, 2, 0, 0);
    CHECK(lm_on_note(&m, &x, t) == (LM_A_RELEASE | LM_A_STRIP) && m.strip == LM_S_HOST_LOST);
    /* the quit waits for plain play: the pause menu always, a cutscene at most 60 s */
    lm_init(&m, 1);
    lm_on_note(&m, &x, t);
    lm_frame_in g = f;
    g.paused = 1;
    CHECK(run(&m, &g, &t, 90000, NULL) == 0 && m.quit_at);
    g = f, g.demo = 1;
    first = 0;
    int a = run(&m, &g, &t, 5000, &first);   /* (90 s gone already: the cutscene is not waited out any more) */
    CHECK(a == LM_A_QUIT);
    lm_init(&m, 1);
    uint32_t t0 = t;
    lm_on_note(&m, &x, t);
    first = 0;
    a = run(&m, &g, &t, 70000, &first);
    CHECK(a == LM_A_QUIT && (int)(first - t0) > LM_QUIT_DEMO_MS && (int)(first - t0) < LM_QUIT_DEMO_MS + 500);
    /* a load / a state that never becomes plain play: dropped after 3 minutes */
    lm_init(&m, 1);
    lm_on_note(&m, &x, t);
    g = f, g.agame_state = 8;
    CHECK(run(&m, &g, &t, LM_QUIT_GIVEUP_MS + 2000, NULL) == 0 && !m.quit_at);
    /* already at the menus (no aGame): dropped */
    lm_init(&m, 1);
    lm_on_note(&m, &x, t);
    g = f, g.agame_state = -1;
    CHECK(run(&m, &g, &t, 9000, NULL) == 0 && !m.quit_at);
    /* the lab switch REV2_HOSTLOSS=stay: the strip, but the guest stays in the level */
    lm_init(&m, 1);
    m.stay_on_host_loss = 1;
    x = note(CS_N_LOST, 1, CP_LEAVE_QUIT, 0);
    CHECK(lm_on_note(&m, &x, t) == (LM_A_RELEASE | LM_A_STRIP) && m.strip == LM_S_HOST_ENDED && !m.quit_at);
    CHECK(run(&m, &f, &t, 10000, NULL) == 0);
    /* before the start (the room): the lobby shows the reason, no strip */
    lm_init(&m, 1);
    x.in_game = 0;
    CHECK(lm_on_note(&m, &x, t) == 0);
}

static void test_episode_end(void)
{
    lm_t m;
    uint32_t t = 30000;
    /* this PC reaches the result area 9000 of Episode 1 first: LEAVE(EP_END) once + strip; the flag stays until the title */
    lm_init(&m, 1);
    lm_frame_in f = play();
    CHECK(run(&m, &f, &t, 1000, NULL) == 0 && !m.ep_end);
    f.area = 9000;
    int a = lm_tick(&m, &f, t += 16);
    CHECK(a == LM_A_EP_LEAVE && m.strip == LM_S_NONE && m.ep_end && m.ep_left);   /* round 2: no strip over the load / the ending scene */
    f.session_running = 0;   /* (the session is closed now) */
    CHECK(run(&m, &f, &t, 5000, NULL) == 0 && m.ep_end && m.n_ep == 1);
    f.at_menus = 1, f.agame_state = -1;
    run(&m, &f, &t, 50, NULL);
    CHECK(!m.ep_end && !m.ep_left);
    /* another episode's result area (a lab Episode 2 session) is not the end */
    lm_init(&m, 1);
    f = play(), f.area = 9000, f.episode = 2;
    CHECK(run(&m, &f, &t, 1000, NULL) == 0 && !m.ep_end);
    /* the partner got there first: the host sees PEER_LEAVE b = EP_END, the guest LOST a = 1 b = EP_END: the strip, no solo, no quit */
    lm_init(&m, 1);
    lm_note_in x = note(CS_N_PEER_LEAVE, 0, CS_GONE_EP_END, 1);
    CHECK(lm_on_note(&m, &x, t) == LM_A_RELEASE && m.ep_end && m.ep_note);
    f = play();
    f.session_running = 0;
    CHECK(run(&m, &f, &t, 5000, NULL) == LM_A_STRIP && m.strip_ms == 0 && !m.ep_note);   /* (it plays on: the strip after LM_EP_NOTE_PLAY_MS) */
    f.area = 9000;   /* this PC then reaches its own result area: no second leave (no session) */
    CHECK(run(&m, &f, &t, 1000, NULL) == 0 && m.ep_end);
    lm_init(&m, 1);
    x = note(CS_N_LOST, 1, CP_LEAVE_EP_END, 0);
    CHECK(lm_on_note(&m, &x, t) == LM_A_RELEASE && m.ep_end && !m.quit_at);
    /* once the end was reached, a later loss of the link starts no quit (the session was over anyway) */
    x = note(CS_N_LOST, 0, 0, 0);
    CHECK(lm_on_note(&m, &x, t) == LM_A_RELEASE && !m.quit_at);
    /* a partner who left before: the episode end drops a pending solo step (the result screen) */
    lm_init(&m, 1);
    x = note(CS_N_PEER_LEAVE, 0, CS_GONE_LEFT, 1);
    lm_on_note(&m, &x, t);
    f = play(), f.area = 9000;
    CHECK(lm_tick(&m, &f, t + 16) == LM_A_EP_LEAVE && !m.solo_at);
    /* a new session start clears the flags */
    lm_session_start(&m);
    CHECK(!m.ep_end && !m.ep_left && !m.solo_at && !m.quit_at);
}

/* round 2 (live runs 5-7: the 20 s strip lay over the area load and the ending scene, which runs in aGame state 8 without an sDemo cutscene): no strip at the
 * leave; the strip comes with the stock episode-end menu (refreshed while it is up); the partner's end shows it only if this PC plays on */
static void test_episode_end_strip(void)
{
    lm_t m;
    uint32_t t = 50000, first = 0;
    lm_init(&m, 1);
    lm_frame_in f = play();
    f.area = 9000;   /* the StageClear order set the area, still in plain play */
    CHECK(lm_tick(&m, &f, t += 16) == LM_A_EP_LEAVE);
    f.session_running = 0, f.agame_state = 8;
    CHECK(run(&m, &f, &t, 50000, NULL) == 0);   /* the load and the ending scene: nothing */
    f.agame_state = 2;
    CHECK(run(&m, &f, &t, 60000, NULL) == 0);   /* the results */
    f.end_menu = 1;   /* the end menu without "next" */
    CHECK(lm_tick(&m, &f, t += 16) == LM_A_STRIP && m.strip == LM_S_EP_END && m.strip_ms == LM_STRIP_MENU_MS && !m.strip_quiet);
    CHECK(run(&m, &f, &t, LM_EP_MENU_REFRESH_MS - 40, NULL) == 0);
    int a = run(&m, &f, &t, 5000, &first);
    CHECK(a == LM_A_STRIP);   /* refreshed while the menu is up */
    t += LM_EP_MENU_REFRESH_MS;
    CHECK(lm_tick(&m, &f, t) == LM_A_STRIP && m.strip_quiet && m.strip_ms == LM_STRIP_MENU_MS);   /* a refresh: no log line */
    f.end_menu = 0;   /* the title row chosen */
    CHECK(run(&m, &f, &t, 10000, NULL) == 0);
    f.at_menus = 1, f.agame_state = -1;
    run(&m, &f, &t, 50, NULL);
    CHECK(!m.ep_end && !m.ep_menu_t && !m.ep_note);
    /* the end menu of a single-player game (no co-op end): no strip */
    lm_init(&m, 1);
    f = play(), f.session_running = 0, f.area = 9000, f.agame_state = 2, f.end_menu = 1;
    CHECK(run(&m, &f, &t, 3000, NULL) == 0);
    /* the partner's end while this PC plays on: the strip after LM_EP_NOTE_PLAY_MS of plain play, once */
    lm_init(&m, 1);
    lm_note_in x = note(CS_N_LOST, 1, CP_LEAVE_EP_END, 0);
    CHECK(lm_on_note(&m, &x, t) == LM_A_RELEASE && m.ep_note);
    f = play(), f.session_running = 0;
    first = 0;
    uint32_t t0 = t;
    a = run(&m, &f, &t, 6000, &first);
    CHECK(a == LM_A_STRIP && m.ep_note == 0 && first && (int32_t)(first - t0) >= LM_EP_NOTE_PLAY_MS && (int32_t)(first - t0) < LM_EP_NOTE_PLAY_MS + 100);
    CHECK(run(&m, &f, &t, 30000, NULL) == 0);
    /* the partner's end, then this PC's own stage clear within the wait: no strip until the end menu */
    lm_init(&m, 1);
    lm_on_note(&m, &x, t);
    f = play(), f.session_running = 0;
    CHECK(run(&m, &f, &t, 2000, NULL) == 0);
    f.area = 9000, f.agame_state = 8;
    CHECK(run(&m, &f, &t, 30000, NULL) == 0 && !m.ep_note);
    f.agame_state = 2, f.end_menu = 1;
    CHECK(lm_tick(&m, &f, t += 16) == LM_A_STRIP && m.strip_ms == LM_STRIP_MENU_MS);
    /* the note in a cutscene / the pause menu: the wait counts plain play only */
    lm_init(&m, 1);
    x = note(CS_N_PEER_LEAVE, 0, CS_GONE_EP_END, 1);
    CHECK(lm_on_note(&m, &x, t) == LM_A_RELEASE && m.ep_note);
    f = play(), f.session_running = 0, f.demo = 1;
    CHECK(run(&m, &f, &t, 20000, NULL) == 0);
    f.demo = 0, f.paused = 1;
    CHECK(run(&m, &f, &t, 5000, NULL) == 0);
    f.paused = 0;
    CHECK(run(&m, &f, &t, 4000, NULL) == LM_A_STRIP);
    /* the other strips are unchanged (a partner who left: its strip at once, LM_STRIP_MS) */
    lm_init(&m, 1);
    x = note(CS_N_PEER_LEAVE, 0, CS_GONE_LEFT, 1);
    CHECK(lm_on_note(&m, &x, t) == (LM_A_RELEASE | LM_A_STRIP) && m.strip == LM_S_PARTNER_LEFT && m.strip_ms == 0);
}

int main(void)
{
    test_values();
    test_host_partner_left();
    test_solo_waits_for_plain_play();
    test_end_coop();
    test_guest_host_gone();
    test_episode_end();
    test_episode_end_strip();
    printf("test_lobbymust: %d checks, %d failed\n", n, bad);
    return bad != 0;
}
