/* warp.inc: the lab stage warp of Episode 1 (console `warp`, always on the game thread; included by coop3.c after coop_game.inc).
 *
 *   warp status | warp            the story's stage / start position, the restart copy [aGame+0x608] (stage, start position, the flag slots with their set flags)
 *   warp list                     the checkpoints of Episode 1 (stage, start position, CHP flag, position) in walk order
 *   warp <stage> [startpos] [set|go] [keep] [+flag ...] [-flag ...]
 *                                 writes the checkpoint into the restart copy: mStageNo, mStartPosIdx, mSceNo 0, and the stage's flag words (the CHP flags of that stage up to the
 *                                 target, the extras; `keep` keeps the stage's other flags of the restart copy) and, with `go` (default), asks the game for its own restart
 *                                 (FUN_004362E0, the "retry from checkpoint" request): the game copies the restart copy over its data and loads the stage at the start position.
 *                                 `set` writes only: the next retry (death screen) uses it. Only inside the half that is loaded (Claire: 100-102, Barry: 151-154).
 * In a session send it to BOTH copies (each loads its own copy; the glue treats the two loads like a retry). Lab only: the console exists only with REV2_CONSOLE (test copies),
 * nothing of this is in the lobby or the product's menus. Logic + table: warp_logic.h (tests/test_warp.c). Notes: notes\ep1-cover.md. */
#include "warp_logic.h"
#include "lobby.h"   /* lobby_second_half_stage / startpos (the lobby's start of Barry's half of Episodes 2-4) */

#define WARP_LOG "warp: "
#define WARP_A_FREE_ID 0x126087Cu      /* DAT_0126087C: the id of a free flag slot */
#define WARP_A_RESTART_REQ 0x4362E0u   /* FUN_004362E0: sBioMain+0x285B8 |= 0x10000 (the restart request Coop_UpdateMain answers with FUN_004286C0 -> aGame state 0xC) */
static unsigned g_warp_n, g_warp_req;
static char g_warp_last[200];

static int warp_ptr_ok(unsigned p) { return p >= 0x10000 && p < 0x7FFF0000u; }

static int warp_list_flags(const unsigned *w, char *out, int size)
{
    int o = 0, n = 0;
    for (int f = 0; f < WL_FLAGS_N && o < size - 8; f++)
        if (wl_bit_get(w, f)) o += snprintf(out + o, size - o, " %d", f), n++;
    if (!n && size > 6) o += snprintf(out + o, size - o, " none");
    return o;
}

static void warp_status(char *out, int size);
static void warp_status(char *out, int size)
{
    unsigned sb = story_block(), ar = game_area(), r = ar ? rd32(ar + 0x608) : 0;
    unsigned st = ar ? rd32(ar + 4) : 0;
    int o = snprintf(out, size, "warp: story episode %u stage %u startpos %u scene %u part %u; aGame %08x state %u sub %u; warps %u (restart requests %u)\n",
                     sb ? rd32(sb + 0x140378) : 0, sb ? rd32(sb + 0x140380) : 0, sb ? rd32(sb + 0x140384) : 0, sb ? rd32(sb + 0x140388) : 0, sb ? rd32(sb + 0x15C4A8) : 0,
                     ar, st & 0xFF, (st >> 8) & 0xFF, g_warp_n, g_warp_req);
    if (!warp_ptr_ok(r)) {
        o += snprintf(out + o, size - o, "restart copy: none (not in an episode)");
        return;
    }
    o += snprintf(out + o, size - o, "restart copy %08x: stage %u startpos %u scene %u; flag slots:", r, rd32(r + WL_GI_STAGE), rd32(r + WL_GI_STARTPOS), rd32(r + WL_GI_SCENO));
    static unsigned tab[WL_SLOTS * (WL_SLOT_BYTES / 4)];
    unsigned freeid = rd32(WARP_A_FREE_ID);
    if (!r2_safe_read((void *)(r + WL_GI_FLAGS), tab, sizeof tab)) {
        o += snprintf(out + o, size - o, " (not readable)");
        return;
    }
    for (int i = 0; i < WL_SLOTS && o < size - 40; i++) {
        unsigned *e = &tab[i * (WL_SLOT_BYTES / 4)];
        if (e[0] == freeid) continue;
        o += snprintf(out + o, size - o, "\n  slot %d stage %u:", i, e[0]);
        o += warp_list_flags(e + 1, out + o, size - o);
    }
    if (g_warp_last[0] && o < size - 20) snprintf(out + o, size - o, "\nlast: %s", g_warp_last);
}

static void warp_list2(char *out, int size, int from)
{
    int o = snprintf(out, size, "Episode 2-4 start positions (stage startpos flag position name), rows %d..:", from);
    for (int i = from; i < WL_CHP2_N && o < size - 90; i++) {
        const wl_chp_t *c = &g_wl_chp2[i];
        o += snprintf(out + o, size - o, "\n  %d %d SF%d (%d,%d,%d) %s", c->stage, c->startpos, c->flag, c->x, c->y, c->z, c->name);
    }
}

static void warp_list(char *out, int size)
{
    int o = snprintf(out, size, "Episode 1 checkpoints (stage startpos flag position name), walk order:");
    for (int i = 0; i < WL_CHP_N && o < size - 80; i++) {
        const wl_chp_t *c = &g_wl_chp[i];
        o += snprintf(out + o, size - o, "\n  %d %d SF%d (%d,%d,%d) %s", c->stage, c->startpos, c->flag, c->x, c->y, c->z, c->name);
    }
}

#define EV_R_RETRY (CP_EV_USER + 0x70)   /* 0x170: body u8 reason (WL_RC_PAUSE / WL_RC_WARP), u8 sender slot, then (warp) the warp line */
static int g_rc_on = 1;                 /* the retry coupling (`warp couple 0|1`): on by default, it is the product's rule for the pause menu's Retry */
static unsigned g_rc_tx, g_rc_rx, g_rc_restart, g_rc_warp, g_rc_ignored;
static void warp_do(const char *arg, char *out, int size, int from_partner);

static void rc_send(int reason, const char *line)
{
    if (!g_rc_on || !g_cs || !cs_active(g_cs) || cs_phase(g_cs) != CP_PHASE_PLAYING) return;
    unsigned char b[CP_EVENT_MAX];
    int n = 0;
    b[n++] = (unsigned char)reason, b[n++] = (unsigned char)(g_my_slot & 0xFF);
    if (line) {
        int l = (int)strlen(line);
        if (l > CP_EVENT_MAX - 3) l = CP_EVENT_MAX - 3;
        memcpy(b + n, line, (size_t)l), n += l;
    }
    b[n] = 0;
    if (cs_send_event(g_cs, EV_R_RETRY, b, (unsigned)n + 1)) g_rc_tx++;
    r2_log(WARP_LOG "retry coupling: told the partner (%s%s%s)", reason == WL_RC_WARP ? "warp " : "pause Retry", line ? line : "", "");
}

/* menu.c: the pause menu's result 2 (the Retry row's "Yes") in a running net game: the partner retries too (the stock restart of this PC follows by itself) */
void coop_pause_retry(void)
{
    r2_log(WARP_LOG "pause menu: Retry confirmed in a net game");
    rc_send(WL_RC_PAUSE, NULL);
}

/* 0.3.1 R1 (warp_logic.h wl_sh_*): the Barry-half start jump is repeated by the host until the partner answers HALF_OK from a stage of part 1 */
#define EV_R_HALFOK (CP_EV_USER + 0x71)   /* 0x171: partner -> host, body u16 the partner's story stage */
static wl_sh_host_t g_shh;
static int g_shh_acked, g_shg_have, g_shg_pending;
static unsigned g_shg_applied_t, g_sh_logs, g_sh_resends, g_sh_answers;
static char g_shh_line[48], g_shg_line[48];   /* the host's start jump; the last half jump this PC applied as the partner */
static unsigned g_sh_hold_until;   /* lab: `warp couple hold <ms>` */
static int sh_held(void) { return (int)(g_sh_hold_until - GetTickCount()) > 0; }
static int sh_in_play(void)
{
    unsigned ar = game_area();
    return ar && (int)(rd32(ar + 4) & 0xFF) == 3 && game_in_episode() && !sh_held();
}
static int sh_my_stage(void)
{
    unsigned sb = story_block();
    return sb ? (int)rd32(sb + 0x140380) : -1;
}
static void sh_answer(const char *why)
{
    if (!g_cs || !cs_active(g_cs)) return;
    unsigned char b[2];
    unsigned short st = (unsigned short)sh_my_stage();
    memcpy(b, &st, 2);
    if (cs_send_event(g_cs, EV_R_HALFOK, b, 2)) g_sh_answers++;
    if (g_sh_logs++ < 40) r2_log(WARP_LOG "Barry's half: told the host I am in stage %u (%s)", st, why);
}
/* coop_game.inc handle_notes: the partner's HALF_OK */
static void sh_ok_on_event(const cp_event *e)
{
    unsigned short st = 0;
    if (e->len >= 2) memcpy(&st, e->body, 2);
    if (!g_shh_acked && g_shh.active && g_sh_logs++ < 40) r2_log(WARP_LOG "Barry's half: the partner is in stage %u: no more repeats of the jump", st);
    g_shh_acked = 1;
}
/* every frame of the glue (second_half_frame): the host's repeats, the partner's answer after its jump */
static void sh_retry_frame(void)
{
    static unsigned t;
    unsigned now = GetTickCount();
    if (now - t < 500) return;
    t = now;
    int session = g_cs && cs_active(g_cs);
    if (g_shg_pending && wl_sh_guest_answer(g_shg_pending, sh_my_stage(), sh_in_play() && g_state == G_RUN)) {
        g_shg_pending = 0;
        sh_answer("after the jump");
    }
    if (!g_shh.active) return;
    const char *why = "";
    int playing = session && cs_phase(g_cs) == CP_PHASE_PLAYING;
    int s = wl_sh_host_step(&g_shh, session, playing, sh_my_stage(), sh_in_play() && g_state == G_RUN, g_shh_acked, now, &why);
    if (s == WL_SH_SEND) {
        rc_send(WL_RC_WARP, g_shh_line);
        g_sh_resends++;
        if (g_sh_logs++ < 40) r2_log(WARP_LOG "Barry's half: %s ('%s', repeat %u)", why, g_shh_line, g_shh.sent);
    } else if (s == WL_SH_STOP && g_sh_logs++ < 40)
        r2_log(WARP_LOG "Barry's half: the repeats of the jump end: %s (%u repeats)", why, g_shh.sent);
}

/* coop_game.inc handle_notes: the partner's RETRY (any glue state: the partner's load notice may already have moved this PC to "waiting") */
static void retry_on_event(const cp_event *e)
{
    if (e->len < 2) return;
    g_rc_rx++;
    int reason = e->body[0], slot = e->body[1];
    unsigned ar = game_area(), lp = player_obj(g_my_slot);
    int st = ar ? (int)(rd32(ar + 4) & 0xFF) : -1, hp = lp ? (int)rd32(lp + 0x1A08) : 0;
    const char *why = "";
    int half = reason == WL_RC_WARP && e->len > 2 && strstr((const char *)e->body + 2, " half") != NULL;   /* the lobby's Barry-half jump (R1) */
    if (sh_held() && st == 3) st = 8;   /* lab: `warp couple hold` (a guest that is still loading) */
    if (half) {
        unsigned now = GetTickCount();
        const char *jl = (const char *)e->body + 2;
        int same = g_shg_have && !strncmp(jl, g_shg_line, sizeof g_shg_line - 1);
        int g = wl_sh_guest_decide(sh_my_stage(), sh_in_play(), atoi(jl), same ? now - g_shg_applied_t : 0xFFFFFFFFu, &why);
        if (g == WL_SH_G_ACK) { g_rc_ignored++; sh_answer(why); return; }
        if (g == WL_SH_G_IGNORE) {
            g_rc_ignored++;
            if (g_sh_logs++ < 40) r2_log(WARP_LOG "retry coupling: the partner's jump into Barry's half again: %s", why);
            return;
        }
    }
    int act = wl_retry_decide(g_rc_on, reason, st, hp, &why);
    r2_log(WARP_LOG "retry coupling: the partner (slot %d) %s; aGame state %d, my hp %d: %s", slot, reason == WL_RC_WARP ? "warped" : "chose Retry in its pause menu", st, hp, why);
    if (act == WL_RC_RESTART) {
        do_call(0, WARP_A_RESTART_REQ, NULL, 0);
        g_rc_restart++, g_warp_req++;
    } else if (act == WL_RC_APPLY_WARP) {
        char line[CP_EVENT_MAX], out[300];
        int l = e->len - 2;
        if (l > (int)sizeof line - 1) l = (int)sizeof line - 1;
        memcpy(line, e->body + 2, (size_t)l), line[l] = 0;
        warp_do(line, out, sizeof out, 1);
        g_rc_warp++;
        if (half && strncmp(out, "warp: stage", 11) == 0) {   /* written and the restart asked for: answer the host from that stage */
            g_shg_have = 1, g_shg_applied_t = GetTickCount(), g_shg_pending = atoi(line);
            snprintf(g_shg_line, sizeof g_shg_line, "%s", line);
        }
        r2_log(WARP_LOG "retry coupling: applied the partner's warp '%s': %s", line, out);
    } else g_rc_ignored++;
}

/* ---- the lobby's start of Barry's half of Episodes 2-4 (0.3.0 all-episodes): part 0 starts (the only start the game takes), and once the stage is in play the HOST hops into part 1
 * (warp_do with `half`; the retry coupling carries the same hop to the partner, whose game is in the same stage: the barrier is open). Nothing is written before that, so a game that
 * never gets there (a death in the opening, a lost partner) simply stays in part 0. The first stage of part 0: 201 / 306 / 400. */
static int g_sh_ep;
static unsigned g_sh_since, g_sh_done;

void coop_second_half_arm(int episode)
{
    g_sh_ep = episode >= 2 && episode <= 4 ? episode : 0;
    g_sh_since = 0;
    memset(&g_shh, 0, sizeof g_shh), g_shh_acked = 0, g_shg_have = 0, g_shg_pending = 0;   /* R1: a new start forgets the repeats / answers of an earlier one */
    if (g_sh_ep) r2_log(WARP_LOG "second half of episode %d armed: the host hops into part 1 once part 0 is in play", g_sh_ep);
}

/* Stage 454 (Episode 4, "below the cliff"): its human slot 1 (the Claire family, uPl2240) is not placed by the part's opening, which the restart hop skips: live allepP15 showed it
 * parked at (110,-375,75) on both PCs and not controllable while Barry stood at (-74000,3800,-96000). The PC whose own human is parked near the origin while the partner is far away
 * puts its human next to the partner (8 s apart), through the game's own teleport (what `net tp` does). Nothing else is touched. */
static void unplaced_human_frame(void)
{
    static unsigned t, last_fix;
    if (!(g_cs && cs_active(g_cs)) || g_state != G_RUN) return;
    unsigned now = GetTickCount();
    if (now - t < 1000) return;
    t = now;
    unsigned id = story_area_id();
    if (id != 454) return;
    if (last_fix && now - last_fix < 8000) return;   /* one placement, then 8 s of peace */
    unsigned me = player_obj(g_my_slot), other = player_obj(1 - g_my_slot);
    if (!me || !other) return;
    float mx = rdf(me + 0x40), my = rdf(me + 0x44), mz = rdf(me + 0x48), ox = rdf(other + 0x40), oy = rdf(other + 0x44), oz = rdf(other + 0x48);
    if (fabsf(mx) > 1500.0f || fabsf(mz) > 1500.0f) return;   /* placed somewhere real: leave it (it may be parked later in the load, so keep looking) */
    if (fabsf(ox) < 20000.0f && fabsf(oz) < 20000.0f) return;   /* the partner is not placed yet either: wait */
    float x = ox + 120.0f, y = oy, z = oz, yaw = 0.0f;
    wr(me + 0x39A0, &x, 4), wr(me + 0x39A4, &y, 4), wr(me + 0x39A8, &z, 4), wr(me + 0x39C4, &yaw, 4);
    unsigned a1[1] = {me};
    do_call(3, 0x710FD0, a1, 1);
    last_fix = now | 1u;
    r2_log(WARP_LOG "stage 454: my human was parked at (%.0f,%.0f,%.0f): placed next to the partner at (%.0f,%.0f,%.0f)", mx, my, mz, x, y, z);
}

static void second_half_frame(void)
{
    sh_retry_frame();   /* R1: repeats / answer, also after g_sh_ep is done */
    if (!g_sh_ep) return;
    unsigned sb = story_block(), ar = game_area();
    if (!sb || !ar) { g_sh_since = 0; return; }
    if (rd32(sb + 0x15C4A8) == 1) { r2_log(WARP_LOG "second half of episode %d: part 1 is in the story, done", g_sh_ep); g_sh_ep = 0; return; }
    static const int first[5] = {0, 0, 201, 306, 400};
    int session = g_cs && cs_active(g_cs);
    int host = !session || cs_is_host(g_cs);
    int ready = (int)rd32(sb + 0x140380) == first[g_sh_ep] && (int)(rd32(ar + 4) & 0xFF) == 3 && game_in_episode() && !game_paused() && (!session || g_state == G_RUN);
    if (!ready) { g_sh_since = 0; return; }
    unsigned now = GetTickCount();
    if (!g_sh_since) g_sh_since = now | 1u;
    if (!host || (int32_t)(now - g_sh_since) < 6000) return;   /* the partner waits for the host's hop (the retry coupling) */
    char line[48], out[300];
    int stage = lobby_second_half_stage(g_sh_ep, 1, 0);
    snprintf(line, sizeof line, "%d %d half", stage, lobby_second_half_startpos(g_sh_ep));
    warp_do(line, out, sizeof out, 0);
    r2_log(WARP_LOG "second half of episode %d: hop '%s': %s", g_sh_ep, line, out);
    if (session && strncmp(out, "warp: stage", 11) == 0) {   /* R1: repeated until the partner answers from part 1 */
        wl_sh_host_arm(&g_shh, stage, now);
        g_shh_acked = 0;
        snprintf(g_shh_line, sizeof g_shh_line, "%s", line);
    }
    g_sh_done++;
    g_sh_ep = 0;
}

static void warp_cmd(const char *arg, char *out, int size)
{
    while (*arg == ' ') arg++;
    if (!*arg || !strncmp(arg, "status", 6)) { warp_status(out, size); return; }
    if (!strncmp(arg, "list2", 5)) { warp_list2(out, size, atoi(arg + 5)); return; }   /* warp list2 [first row]: the Episode 2-4 table (about 60 rows fit one answer) */
    if (!strncmp(arg, "list", 4)) { warp_list(out, size); return; }
    if (!strncmp(arg, "couple hold", 11)) {   /* lab (R1 check): warp couple hold <ms>: this PC answers the partner's jumps as "not in play" for <ms> */
        g_sh_hold_until = GetTickCount() + (unsigned)atoi(arg + 11);
        snprintf(out, size, "retry coupling: this PC counts as not in play for %d ms (jumps of the partner ignored meanwhile); repeats sent %u, answers sent %u", atoi(arg + 11), g_sh_resends, g_sh_answers);
        return;
    }
    if (!strncmp(arg, "couple", 6)) {   /* warp couple [0|1]: the retry coupling (pause Retry, warp) */
        if (arg[6] == ' ') g_rc_on = atoi(arg + 7) != 0;
        snprintf(out, size, "retry coupling %s: sent %u, received %u (restarts %u, warps applied %u, ignored %u)", g_rc_on ? "ON" : "off", g_rc_tx, g_rc_rx, g_rc_restart, g_rc_warp, g_rc_ignored);
        return;
    }
    warp_do(arg, out, size, 0);
}

static void warp_do(const char *arg, char *out, int size, int from_partner)
{
    unsigned sb = story_block(), ar = game_area(), r = ar ? rd32(ar + 0x608) : 0;
    if (!sb || !ar || !warp_ptr_ok(r)) {
        snprintf(out, size, "warp: not in an episode (story %08x, aGame %08x, restart copy %08x): start the part first", sb, ar, r);
        return;
    }
    wl_plan_t p;
    char err[200];
    if (!wl_parse(arg, (int)rd32(sb + 0x140380), &p, err, sizeof err)) {
        snprintf(out, size, "warp: %s", err);
        return;
    }
    static unsigned tab[WL_SLOTS * (WL_SLOT_BYTES / 4)];
    if (!r2_safe_read((void *)(r + WL_GI_FLAGS), tab, sizeof tab)) {
        snprintf(out, size, "warp: the restart copy's flag table at %08x is not readable", r + WL_GI_FLAGS);
        return;
    }
    unsigned freeid = rd32(WARP_A_FREE_ID);
    int slot = wl_slot(tab, p.stage, freeid, 0), claimed = 0;
    if (slot < 0) {
        snprintf(out, size, "warp: no flag slot for stage %d and no free one in the restart copy", p.stage);
        return;
    }
    unsigned *e = &tab[slot * (WL_SLOT_BYTES / 4)];
    if (e[0] != (unsigned)p.stage) claimed = 1, memset(e + 1, 0, WL_WORDS * 4);
    unsigned words[WL_WORDS], id = (unsigned)p.stage;
    wl_words(&p, e + 1, words);
    unsigned ast = rd32(ar + 4) & 0xFF;
    if (p.go && ast != 3) {   /* the request is answered only by the in-game state 3 (Coop_UpdateMain): a request left pending would restart the game later, at a moment nobody chose */
        snprintf(out, size, "warp: aGame state %u (not 3 = in play: a scene, a death screen or a load): nothing written; retry when the game is in play, or use `set`", ast);
        return;
    }
    if (p.cross) {   /* the other half of the episode: the story part follows (the stage / episode fields of the restart copy are written below / unchanged inside an episode) */
        unsigned part = (unsigned)wl_part_of(p.stage);
        wr(sb + 0x15C4A8, &part, 4);
    }
    if (wl_bonus_episode(p.stage)) {   /* a bonus stage: the chapter number is the story's episode, also in the restart copy (the stock restart copies it back) */
        unsigned bep = (unsigned)wl_bonus_episode(p.stage);
        wr(sb + 0x140378, &bep, 4), wr(r + 0x10F058, &bep, 4);
    }
    unsigned sbase = r + WL_GI_FLAGS + (unsigned)slot * WL_SLOT_BYTES, sp = (unsigned)p.startpos, zero = 0, st = (unsigned)p.stage;
    int ok = wr(sbase, &id, 4) && wr(sbase + 4, words, sizeof words) && wr(r + WL_GI_STAGE, &st, 4) && wr(r + WL_GI_STARTPOS, &sp, 4) && wr(r + WL_GI_SCENO, &zero, 4);
    char fl[160];
    warp_list_flags(words, fl, sizeof fl);
    g_warp_n++;
    snprintf(g_warp_last, sizeof g_warp_last, "stage %d startpos %d (%s) from stage %u, slot %d%s, flags%s", p.stage, p.startpos, p.chp->name, rd32(sb + 0x140380), slot,
             claimed ? " (new)" : "", fl);
    if (!ok) {
        r2_log(WARP_LOG "write into the restart copy %08x FAILED (%s)", r, g_warp_last);
        snprintf(out, size, "warp: write into the restart copy %08x failed", r);
        return;
    }
    r2_log(WARP_LOG "restart copy %08x set: %s%s", r, g_warp_last, p.go ? "; restart requested" : "; no restart (set): the next retry uses it");
    if (p.go) {
        do_call(0, WARP_A_RESTART_REQ, NULL, 0);
        g_warp_req++;
        if (!from_partner) rc_send(WL_RC_WARP, arg);   /* in a session the partner takes the same checkpoint (it refuses when its game is not in play) */
    }
    snprintf(out, size, "warp: %s; %s (start position (%d,%d,%d))", g_warp_last, p.go ? "the stock restart requested" : "written only", p.chp->x, p.chp->y, p.chp->z);
}
