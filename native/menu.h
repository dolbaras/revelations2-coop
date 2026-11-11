/*
 * Co-op entry in the game's own menus (stage 8): glue between the game (menu objects, page tables, pause menu, start sequence), the lobby model
 * (lobby.c) and the overlay (overlay.c). See docs/lobby.md.
 *
 *   menu_install()   once, from the first Present (the Steam stub has decrypted .text): patches the "Co-Op Play" row into the campaign page,
 *                    hooks the menu object of the dormant session screen, the pause menu's "Leave" row; reads the settings file
 *   menu_frame()     every pass of the game's loop (game thread, from coop3.c frame_callbacks): auto-leave back at the title, auto-open for an invitation
 *   menu_present()   from the Present hook, before the frame is presented: draws the lobby overlay when it is open
 *   menu_before_reset() from the Reset hook: releases the overlay's Direct3D resources
 *   menu_command()   console `menu ...`
 *   menu_on_note()   a session notification (coop_game.inc note_cb, any thread)
 */
#ifndef MENU_H
#define MENU_H

#include "coop_session.h"

void menu_install(void);
void menu_frame(void);
void menu_present(void *d3d9_device);
void menu_before_reset(void);
void menu_command(const char *arg, char *out, int size);
void menu_on_note(const cs_note *n);
/* stage 6i: a one-line strip at the top of the screen (any thread). MB_WAIT = "Waiting for <who> to finish loading..." for `ms` milliseconds; MB_NONE clears it */
enum { MB_NONE = 0, MB_WAIT = 1, MB_DOWN_SELF = 2, MB_DOWN_PARTNER = 3,   /* stage 9: the downed / revive strips; `who` carries the seconds left as text */
       MB_PARTNER_LEFT = 4, MB_PARTNER_LOST = 5, MB_HOST_ENDED = 6, MB_HOST_LOST = 7, MB_EP_END = 8 };   /* 0.2.0 (lobbymust_logic.h LM_S_*): who = the partner's name */
void menu_banner(int kind, const char *who, unsigned ms);
const char *menu_player_name(void);  /* the name of the lobby settings (the Steam persona name once the lobby asked Steam), "" when none */

/* supplied by coop_game.inc */
int coop_net_command(const char *arg, char *out, int size);
void coop_second_half_arm(int episode);   /* warp.inc: the lobby started part 0 of this episode (2..4) to hop into Barry's half in play; 0 clears */
int coop_ep_end_active(void);        /* lobbymust.inc: the co-op end of Episode 1 was reached (the stock episode-end menu then offers no next episode) */
cs_t *coop_net_session(void);        /* creates the session object on first use */
cs_t *coop_net_session_peek(void);   /* NULL until something created it */

#endif
