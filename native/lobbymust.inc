/* lobbymust.inc: the 0.2.0 in-game rules for a lost partner, a lost host, End Co-Op and the end of an episode (0.3.0: any episode; rules: lobbymust_logic.h, notes\lobby-must.md).
 * Included at the end of coop3.c (after coop_game.inc, world.inc, downed.inc): uses the glue's state (g_cs, g_state, g_my_slot, g_rem_slot), run_cmd (the coop3
 * console), set_slot_type, game_paused, w_demo_running, game_area, do_call.
 *
 * Console: net lm (state) | net endcoop (the host's End Co-Op, as the pause row does it) | net solo (the solo step now) | net quittitle (the guest's quit now)
 */
#include "lobby.h"
#include "lobbymust_logic.h"

static lm_t g_lm;
static int g_lm_init;
static char g_lm_partner[CP_NAME_MAX + 1];   /* the name of the partner who left (the strip) */
static unsigned g_lm_solo_done, g_lm_quit_done;
static unsigned g_lm_ep_seen;   /* the story's episode id at the last pass (the log line of the episode end) */

static void lm_glue_init(int no_rejoin)
{
    char v[16];
    lm_init(&g_lm, no_rejoin);
    g_lm.stay_on_host_loss = r2_env("REV2_HOSTLOSS", v, sizeof v) && !_stricmp(v, "stay");   /* lab: the guest stays in the level after a host loss */
    g_lm_init = 1;
}

/* the guest's way back to the title: the call the stock pause menu's Quit makes once its popup is confirmed (sBioMain_vf6 0xA90750, pause result 3 ->
 * FUN_00A91D50: in the campaign FUN_00436460(aGame, 1, 0) = aGame state 0xE (fade out) -> 0xD -> Game_ResetToTitle(0)); the bytes are checked first */
static int lm_quit_to_title(void)
{
    static const unsigned char sig[8] = {0x8B, 0x0D, 0x00, 0xAE, 0x57, 0x01, 0x6A, 0xFF};   /* mov ecx,[0x157AE00]; push -1; call Area_IsEpisodeArea */
    unsigned char cur[13];
    int rel = 0;
    if (!r2_safe_read((void *)0xA91D50, cur, sizeof cur) || memcmp(cur, sig, sizeof sig) || cur[8] != 0xE8 || (memcpy(&rel, cur + 9, 4), 0xA91D5Du + (unsigned)rel) != 0x887910u) {
        r2_log(NETLOG_PREFIX "quit to the title: the code at 00A91D50 is not the supported build's, not called");
        return 0;
    }
    do_call(0, 0xA91D50, NULL, 0);
    return 1;
}

static int lm_agame_state(void)
{
    unsigned ar = game_area();
    return ar ? (int)(rd32(ar + 4) & 0xFF) : -1;
}

/* host, partner gone (or End Co-Op): single player again with the game's own leave of the split screen, so the AI partner and the stock character swap work
 * (sPlayer_RequestSwapCharacter 0x6E9FC0 refuses every swap while gm+0x8F0 == 1). Coop_ApplyJoinLeave(leave) demotes gm[0x7A8] ^ 1: when this PC's human is not
 * the main slot (the host plays Moira / Natalia), the main slot (the partner's puppet) becomes this PC's human first (seat 0 = this PC's input), so after the
 * leave the player has the main character of the half and the stock swap to the other one. Then the session is closed (product rule: nobody joins again). */
static void lm_go_solo(void)
{
    unsigned gm = rd32(0x157AE00);
    if (!gm || !game_in_episode()) return;
    int main_slot = (int)rd32(gm + 0x7A8);
    if (main_slot < 0 || main_slot > 1) main_slot = 0;
    r2_log(NETLOG_PREFIX "partner gone: back to single player (my slot %d, main slot %d, coop flag %u, types %u,%u, seats %d,%d)", g_my_slot, main_slot, rd32(gm + 0x8F0),
           rd32(gm + 0x7B4), rd32(gm + 0x7B8), (int)rd32(gm + 0x7D4), (int)rd32(gm + 0x7D8));
    run_cmd("viewguard off");
    if (g_my_slot != main_slot) {
        set_slot_type(main_slot, 0);
        unsigned a[3] = {gm, (unsigned)main_slot, 0};
        do_call(2, 0x8880E0, a, 3);   /* GameMan_SetSlotSeat(main, 0) */
        if (player_obj(main_slot)) {
            unsigned b[2] = {rd32(0x1567EAC), (unsigned)main_slot};
            do_call(2, 0x6E6110, b, 2);   /* sPlayer_RefreshSlotFromGameMan */
        }
    }
    run_cmd("leave now");   /* Coop_ApplyJoinLeave(leave): the partner slot -> AI (type 1, seat -1), coop flag 0, viewport 1 off, camera refreshed */
    {
        char c[32];
        snprintf(c, sizeof c, "vp 0 %d", main_slot);
        run_cmd(c);
    }
    run_cmd("view 0 0");   /* the stock camera again (the co-op set-up locked mode 0) */
    kbm_patch(0);
    testmode_pad_human(0);   /* the real pad is game user 0 = logical player 1 again */
    g_my_slot = main_slot, g_rem_slot = main_slot ^ 1, g_rem_pid = -1;
    glue_set_state(G_OFF);
    if (g_cs && cs_active(g_cs)) cs_leave(g_cs, CP_LEAVE_QUIT);
    g_lm_solo_done++;
    r2_log(NETLOG_PREFIX "single player: coop flag %u, types %u,%u, seats %d,%d, main %u; the session is closed", rd32(gm + 0x8F0), rd32(gm + 0x7B4), rd32(gm + 0x7B8),
           (int)rd32(gm + 0x7D4), (int)rd32(gm + 0x7D8), rd32(gm + 0x7A8));
}

static void lm_apply(int act)
{
    if ((act & LM_A_RELEASE) && g_state >= G_SETUP) release_puppet();
    if (act & LM_A_STRIP) {
        const char *who = g_lm.strip == LM_S_PARTNER_LEFT || g_lm.strip == LM_S_PARTNER_LOST ? g_lm_partner : "";
        menu_banner(g_lm.strip, who, g_lm.strip_ms ? g_lm.strip_ms : LM_STRIP_MS);
        if (!g_lm.strip_quiet)
            r2_log(NETLOG_PREFIX "strip %d (%s)%s", g_lm.strip, who, g_lm.strip == LM_S_EP_END && g_lm.strip_ms == LM_STRIP_MENU_MS ? " with the episode-end menu" : "");
    }
    if (act & LM_A_EP_LEAVE) {
        r2_log(NETLOG_PREFIX "the result area of the episode (episode %u): co-op ends here in this version, LEAVE(EP_END)", g_lm_ep_seen);
        if (g_cs && cs_active(g_cs)) cs_leave(g_cs, CP_LEAVE_EP_END);
    }
    if (act & LM_A_SOLO) lm_go_solo();
    if (act & LM_A_QUIT) {
        r2_log(NETLOG_PREFIX "the host is gone: back to the title (aGame state %d)", lm_agame_state());
        if (lm_quit_to_title()) g_lm_quit_done++;
    }
}

/* from handle_notes, before the glue reacts (the glue state tells whether the game started for this PC) */
static void lm_note(const gnote_t *n)
{
    if (!g_lm_init) return;
    if (n->what == CS_N_START || n->what == CS_N_CONNECTED) {
        lm_session_start(&g_lm);
        return;
    }
    if (n->what == CS_N_PEER_LEAVE) snprintf(g_lm_partner, sizeof g_lm_partner, "%s", n->text);
    lm_note_in in = {n->what, n->a, n->b, n->pid, g_rem_pid, g_state >= G_SETUP, g_cs && cs_is_host(g_cs)};
    if (n->what == CS_N_LOST) in.is_host = 0;   /* (the session is reset before the note: it was a client) */
    lm_apply(lm_on_note(&g_lm, &in, GetTickCount()));
}

/* every pass of the game loop (also without a session: the guest's quit runs after its session is gone) */
static void lm_frame(void)
{
    if (!g_lm_init) return;
    lm_frame_in f;
    memset(&f, 0, sizeof f);
    unsigned sb = story_block();
    f.session_running = g_cs && lobby_session_running(g_cs);
    f.glue_running = g_state == G_RUN;
    f.in_episode = game_in_episode();
    f.agame_state = lm_agame_state();
    f.paused = game_paused();
    f.demo = w_demo_running();
    if (sb) f.area = rd32(sb + 0x140380), f.episode = rd32(sb + 0x140378), f.part = rd32(sb + 0x15C4A8);
    g_lm_ep_seen = f.episode;
    {
        unsigned ag = game_area();   /* the stock episode-end menu (FUN_00431190 via the end flow FUN_00430D80: aGame state 2, +0x624 = 8; live runs 2-7) */
        f.end_menu = ag && f.area == 9000 && f.agame_state == 2 && rd32(ag + 0x624) == 8;
    }
    f.at_menus = !game_area() && rd32(0x15DE720) && ((unsigned(__cdecl *)(void))0x44D620)() != 0;   /* Menu_GetDemoArea: the title / menu area */
    lm_apply(lm_tick(&g_lm, &f, GetTickCount()));
    unplaced_human_frame();   /* warp.inc: Episode 4 stage 454 */
    second_half_frame();   /* warp.inc: the lobby's start of Barry's half of Episodes 2-4 */
}

/* menu.c: the stock episode-end menu leaves out "next episode" while this is 1 */
int coop_ep_end_active(void) { return g_lm_init && g_lm.ep_end; }

/* the host's End Co-Op (pause menu row, menu.c; console `net endcoop`): the partner is told (LEAVE END_COOP), this PC plays on alone */
static int lm_end_coop_now(char *out, int size)
{
    if (!g_cs || !cs_active(g_cs) || !cs_is_host(g_cs) || !lobby_session_running(g_cs)) {
        snprintf(out, size, "end co-op: only the host of a running game");
        return 0;
    }
    int pid = cs_remote_pid(g_cs);
    cs_player_info pi;
    g_lm_partner[0] = 0;
    if (pid >= 0 && cs_player(g_cs, pid, &pi)) snprintf(g_lm_partner, sizeof g_lm_partner, "%s", pi.name);
    int ok = pid >= 0 && cs_kick(g_cs, pid, CP_LEAVE_END_COOP);
    lm_apply(lm_end_coop(&g_lm, GetTickCount()));
    r2_log(NETLOG_PREFIX "End Co-Op by the host: partner %d told %s", pid, ok ? "to leave" : "nothing (gone already)");
    snprintf(out, size, "end co-op: partner %d %s; the single-player step follows in plain play", pid, ok ? "told to leave" : "was gone already");
    return 1;
}

static int lm_command(const char *cmd, const char *rest, char *out, int size)
{
    (void)rest;
    if (!strcmp(cmd, "lm")) {
        snprintf(out, size, "lobby-must: no_rejoin %d, solo pending %d (done %u), quit pending %d (done %u), episode end %d (left %d), counts solo %u quit %u ep %u, aGame state %d, at menus %d",
                 g_lm.no_rejoin, g_lm.solo_at != 0, g_lm_solo_done, g_lm.quit_at != 0, g_lm_quit_done, g_lm.ep_end, g_lm.ep_left, g_lm.n_solo, g_lm.n_quit, g_lm.n_ep,
                 lm_agame_state(), !game_area());
        return 1;
    }
    if (!strcmp(cmd, "endcoop") || !strcmp(cmd, "solo") || !strcmp(cmd, "quittitle")) {
        if (!lt_on_game_thread()) {
            char c[64];
            snprintf(c, sizeof c, "net %s", cmd);
            lt_post(c, out, size);
            return 1;
        }
        if (!strcmp(cmd, "endcoop")) lm_end_coop_now(out, size);
        else if (!strcmp(cmd, "solo")) {
            lm_go_solo();
            snprintf(out, size, "solo step done (count %u)", g_lm_solo_done);
        } else snprintf(out, size, lm_quit_to_title() ? "quit to the title called (aGame state was %d)" : "quit to the title NOT called", lm_agame_state());
        return 1;
    }
    return 0;
}
