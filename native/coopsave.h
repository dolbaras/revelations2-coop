/* coopsave.h: the co-op save of 0.2.1 (coopsave.c; logic in coopsave_logic.h, notes\coopsave.md). A co-op game reads and writes its own save in the mod's
 * folder; the player's solo campaign save (Steam's DATA0.DAT) is never opened by a co-op game. */
#ifndef COOPSAVE_H
#define COOPSAVE_H

#include "coop_session.h"

void coopsave_on_context(void *steam_ctx);   /* steam.c: the game's CSteamAPIContext (wraps ISteamRemoteStorage once) */
int coopsave_active(void);                   /* 1 = a co-op game runs: the save name goes to the co-op file */
/* a co-op game starts on this PC (game thread, the menus): keep the solo save image, route the save, load the co-op save. 1 = done, 0 = the game's save manager
 * is busy (try again next frame), -1 = refused (no story block, the feature is off) */
int coopsave_enter(const char *why);
/* the stock title Continue of the campaign slot (cDemoCampaignMenu_OnSelect case 0x11, game type 0). follow: apply the host's checkpoint record first (guest).
 * 1 = the game manager was told to load */
int coopsave_continue_start(int follow, int game_type);   /* 0.3.1: game type 0 (Episodes 1-4) or 3 / 4 (the bonus episodes) */
void coopsave_frame(int in_menus, int title_menus, int session_running);   /* menu.c, every pass: gives the solo save back after the co-op game */
/* the checkpoint this PC's co-op save can continue (Episode 1): 1 + part / difficulty / stage / start position, 0 none */
int coopsave_summary(int *part, int *difficulty, int *stage, int *startpos);
int coopsave_summary_ep(int *episode, int *part, int *difficulty);   /* 0.3.1: + the room episode (1-4, 5, 9) */
unsigned coopsave_pflags(void);              /* CP_PF_SAVE0 / CP_PF_SAVE1 of this PC's co-op save */
int coopsave_send_record(cs_t *s, char *err, int esz);   /* host, before START of a Continue: the record of its co-op save as events; 1 = sent */
void coopsave_on_event(const void *body, unsigned len);  /* the glue's note callback: a record event (CP_EV_USER + 0x80) from the host */
int coopsave_follow_ready(void);             /* guest: the host's record arrived complete */
void coopsave_status(char *out, int size);   /* console `menu coopsave` */
#define COOPSAVE_EV_KIND 0x180u              /* CP_EV_USER + 0x80 */

#endif
