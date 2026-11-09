/*
 * lobbymust_logic.h: the 0.2.0 rules for what happens in the game when a co-op session loses a player, ends co-op or reaches the end of Episode 1
 * (lobby-must, notes\lobby-must.md). Pure C, no game access: lobbymust.inc feeds it the session notes and a few game facts every pass and carries out
 * the actions it returns; tests/test_lobbymust.c drives the same code with recorded orders.
 *
 *   host, partner left / lost after the start  -> strip (LT_PARTNER_LEFT / LT_PARTNER_LOST); the partner's character goes back to the game's AI at once;
 *                                                product rule (no rejoin): about 1.5 s later, in plain play, the stock leave of the split screen
 *                                                (single player again: the stock character swap works) and the session is closed (LM_A_SOLO)
 *   guest, host left / ended / lost after the start -> strip (LT_HOST_ENDED / LT_HOST_LOST), then the stock quit to the title as soon as the game is in plain
 *                                                play (aGame state 3, no pause menu, no cutscene; a cutscene is waited out at most LM_QUIT_DEMO_MS) (LM_A_QUIT)
 *   either PC reaches the result area of Episode 1 (area 9000) in a running session -> LEAVE(EP_END) (LM_A_EP_LEAVE); the other PC sees the reason (no solo,
 *                                                no quit); while the flag is up the stock episode-end menu offers no "next episode" (menu.c), so the stock
 *                                                "Return to title" is the way out on both PCs. The strip LT_EP1_END says why while that end menu is on screen
 *                                                (the area load and the ending scene stay clean: live runs 5-7); a PC that gets the partner's end while it
 *                                                still plays on (no area 9000 of its own within LM_EP_NOTE_PLAY_MS of plain play) gets the strip at once
 */
#ifndef LOBBYMUST_LOGIC_H
#define LOBBYMUST_LOGIC_H

#include <stdint.h>
#include <string.h>

/* the values of coop_session.h / coop_proto.h this logic reads, repeated so the header stands alone (tests/test_lobbymust.c checks that they are equal) */
#define LM_N_LOST 3
#define LM_N_PEER_LEAVE 5
#define LM_GONE_REPLACED 2
#define LM_GONE_REMOVED 3
#define LM_GONE_EP_END 4
#define LM_LEAVE_EP_END 4

#define LM_SOLO_DELAY_MS 1500     /* the downed rule stands a lying host up when the partner is gone (dl_set_present): it needs a few passes with the session open */
#define LM_SOLO_GIVEUP_MS 600000  /* no plain play for 10 minutes after the partner left: the solo step is dropped (the AI partner stays, the session stays open) */
#define LM_QUIT_DELAY_MS 5000     /* the strip is read first */
#define LM_QUIT_DEMO_MS 60000     /* a cutscene is waited out this long, then the quit goes ahead in it */
#define LM_QUIT_GIVEUP_MS 180000  /* never in plain play (a stuck load): no quit; the player still has the pause menu */
#define LM_STRIP_MS 12000
#define LM_STRIP_EP_MS 20000      /* the partner's end of Episode 1 while this PC plays on */
#define LM_EP_NOTE_PLAY_MS 3000   /* ... after this much plain play outside area 9000 (both PCs clear the stage together: the own end usually follows) */
#define LM_STRIP_MENU_MS 3000     /* the strip with the episode-end menu: refreshed while the menu is up, gone this long after it */
#define LM_EP_MENU_REFRESH_MS 1000

/* strip kinds (menu.h MB_*) */
enum { LM_S_NONE = 0, LM_S_PARTNER_LEFT = 4, LM_S_PARTNER_LOST = 5, LM_S_HOST_ENDED = 6, LM_S_HOST_LOST = 7, LM_S_EP_END = 8 };
/* actions (bits) */
enum { LM_A_RELEASE = 1, LM_A_SOLO = 2, LM_A_QUIT = 4, LM_A_EP_LEAVE = 8, LM_A_STRIP = 16 };

typedef struct {
    int no_rejoin;          /* the product rule (the session refuses every join after the start) */
    int stay_on_host_loss;  /* lab (REV2_HOSTLOSS=stay): the guest gets the strip but stays in the level (the stage-4 behaviour) */
    uint32_t solo_at;       /* 0 = no solo step pending */
    uint32_t solo_since;
    uint32_t quit_at, quit_since;
    int ep_end;             /* the co-op Episode 1 end was reached (by this PC or the partner): the stock end menu offers no next episode */
    int ep_left;            /* this PC sent its LEAVE(EP_END) */
    int ep_note;            /* the partner's end arrived (note): the strip waits for LM_EP_NOTE_PLAY_MS of plain play outside area 9000 */
    uint32_t ep_play_since;
    uint32_t ep_menu_t;     /* the last strip refresh with the episode-end menu (0 = not shown yet) */
    int strip;              /* the strip of the last action (LM_S_*) */
    unsigned strip_ms;      /* its time (0 = LM_STRIP_MS) */
    int strip_quiet;        /* a refresh of a strip already up (no log line) */
    unsigned n_solo, n_quit, n_ep;
} lm_t;

typedef struct {
    int what, a, b, pid;    /* the session note */
    int rem_pid;            /* the glue's partner pid (the puppet's owner) */
    int in_game;            /* the glue state is set-up or later (the game started for this PC) */
    int is_host;
} lm_note_in;

typedef struct {
    int session_running;    /* a session exists and it is past the start (LOADING / PLAYING) */
    int glue_running;       /* the glue is in its running state (barrier open, roles in place) */
    int in_episode;         /* both player objects exist in an episode area */
    int agame_state;        /* aGame +4 (3 = plain play, Coop_UpdateMain), -1 = no aGame */
    int paused, demo;       /* the pause menu is open / a cutscene runs */
    unsigned area, episode, part;   /* story +0x140380 / +0x140378 / +0x15C4A8 */
    int at_menus;           /* the title menus are on screen (aDemo) */
    int end_menu;           /* the stock episode-end menu is up (area 9000, aGame state 2, aGame +0x624 == 8) */
} lm_frame_in;

static inline void lm_init(lm_t *m, int no_rejoin)
{
    memset(m, 0, sizeof *m);
    m->no_rejoin = no_rejoin;
}

static inline int lm_due(uint32_t now, uint32_t at) { return at && (int32_t)(now - at) >= 0; }

/* a session note; returns LM_A_* (with LM_A_STRIP: m->strip says which) */
static inline int lm_on_note(lm_t *m, const lm_note_in *n, uint32_t now)
{
    int act = 0;
    m->strip = LM_S_NONE, m->strip_ms = 0, m->strip_quiet = 0;
    if (!n->in_game) return 0;   /* before the start the lobby shows the reasons itself */
    if (n->is_host && n->what == LM_N_PEER_LEAVE && n->pid == n->rem_pid && n->pid >= 0) {
        act |= LM_A_RELEASE;   /* the puppet stops at once: the game's AI partner takes the character */
        if (n->b == LM_GONE_EP_END) {
            m->ep_end = 1, m->n_ep++;
            m->ep_note = 1, m->ep_play_since = 0;
            return act;
        }
        if (n->b == LM_GONE_REPLACED) return act;   /* the lab's rejoin: the same player is back on a new link */
        m->strip = n->a == 1 || n->b == 1 ? LM_S_PARTNER_LOST : LM_S_PARTNER_LEFT;
        act |= LM_A_STRIP;
        if (m->no_rejoin && !m->ep_end) m->solo_at = (now + LM_SOLO_DELAY_MS) | 1u, m->solo_since = now | 1u;
        return act;
    }
    if (!n->is_host && n->what == LM_N_LOST) {
        act |= LM_A_RELEASE;
        if (n->a == 1 && n->b == LM_LEAVE_EP_END) {
            m->ep_end = 1, m->n_ep++;
            m->ep_note = 1, m->ep_play_since = 0;
            return act;
        }
        if (m->ep_end) return act;   /* the session was over anyway (this PC is at its own episode end) */
        m->strip = n->a == 1 ? LM_S_HOST_ENDED : LM_S_HOST_LOST;
        if (!m->stay_on_host_loss) m->quit_at = (now + LM_QUIT_DELAY_MS) | 1u, m->quit_since = now | 1u;
        return act | LM_A_STRIP;
    }
    return 0;
}

/* the host itself ended co-op in the pause menu (it told the partner with LEAVE(END_COOP)): the same solo step as a partner who left */
static inline int lm_end_coop(lm_t *m, uint32_t now)
{
    m->strip = LM_S_PARTNER_LEFT, m->strip_ms = 0, m->strip_quiet = 0;
    m->solo_at = (now + LM_SOLO_DELAY_MS) | 1u, m->solo_since = now | 1u;
    return LM_A_RELEASE | LM_A_STRIP;
}

/* every pass; returns LM_A_SOLO / LM_A_QUIT / LM_A_EP_LEAVE / LM_A_STRIP (the end of Episode 1: m->strip, m->strip_ms) */
static inline int lm_tick(lm_t *m, const lm_frame_in *f, uint32_t now)
{
    int act = 0;
    m->strip = LM_S_NONE, m->strip_ms = 0, m->strip_quiet = 0;
    if (f->at_menus) {   /* back at the title: everything of the last game is over */
        m->solo_at = m->quit_at = 0;
        m->ep_end = m->ep_left = 0;
        m->ep_note = 0, m->ep_play_since = 0, m->ep_menu_t = 0;
        return 0;
    }
    /* the end of Episode 1: the result area 9000 of episode 1 (the end of Barry's half) while the session runs */
    if (f->session_running && !m->ep_left && f->area == 9000 && f->episode == 1) {
        m->ep_left = 1, m->ep_end = 1, m->n_ep++;
        m->solo_at = 0, m->ep_note = 0;
        act |= LM_A_EP_LEAVE;   /* no strip here: the area load and the ending scene (aGame state 8, no sDemo cutscene) stay clean */
    }
    /* the partner's end arrived first: the strip only if this PC plays on (its own end is told with the end menu) */
    if (m->ep_note) {
        if (f->area == 9000) m->ep_note = 0;
        else if (f->agame_state == 3 && !f->paused && !f->demo) {
            if (!m->ep_play_since) m->ep_play_since = now | 1u;
            else if ((int32_t)(now - m->ep_play_since) >= LM_EP_NOTE_PLAY_MS) {
                m->ep_note = 0;
                m->strip = LM_S_EP_END, m->strip_ms = LM_STRIP_EP_MS;
                act |= LM_A_STRIP;
            }
        } else
            m->ep_play_since = 0;
    }
    /* the stock episode-end menu without "next episode": the strip says why while the menu is up */
    if (m->ep_end && f->end_menu && (!m->ep_menu_t || (int32_t)(now - m->ep_menu_t) >= LM_EP_MENU_REFRESH_MS)) {
        m->strip_quiet = m->ep_menu_t != 0;
        m->ep_menu_t = now | 1u;
        m->strip = LM_S_EP_END, m->strip_ms = LM_STRIP_MENU_MS;
        act |= LM_A_STRIP;
    }
    if (m->solo_at && lm_due(now, m->solo_at)) {
        if (f->glue_running && f->in_episode && f->agame_state == 3 && !f->paused && !f->demo) {
            m->solo_at = 0, m->n_solo++;
            act |= LM_A_SOLO;
        } else if ((int32_t)(now - m->solo_since) > LM_SOLO_GIVEUP_MS)
            m->solo_at = 0;
    }
    if (m->quit_at && lm_due(now, m->quit_at)) {
        int waited = (int32_t)(now - m->quit_since);
        if (f->agame_state == 3 && !f->paused && (!f->demo || waited > LM_QUIT_DEMO_MS)) {
            m->quit_at = 0, m->n_quit++;
            act |= LM_A_QUIT;
        } else if (f->agame_state < 0 && waited > LM_QUIT_DELAY_MS + 2000)
            m->quit_at = 0;   /* no game area any more (already at the menus or loading the title) */
        else if (waited > LM_QUIT_GIVEUP_MS)
            m->quit_at = 0;
    }
    return act;
}

/* a new session started (CS_N_START or joined): the flags of the previous game are gone */
static inline void lm_session_start(lm_t *m)
{
    m->solo_at = m->quit_at = 0;
    m->ep_end = m->ep_left = 0;
    m->ep_note = 0, m->ep_play_since = 0, m->ep_menu_t = 0;
}

#endif
