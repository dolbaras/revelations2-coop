/* pickup_logic.h: the pure decision of the PICKUP replay (world.inc EV_W_PICKUP) so that tests/test_pickup.c can replay it without a game.
 * fix032 (2026-11-13, guest interaction audit, pair run g032b / g032d).
 *
 * What a world pick-up does in the game: the pick-up code calls AddItem(player, item, count, uItem object, flag) (0x6EA6A0). When everything was added, AddItem's tail calls
 * sItem::RemoveItemByFlag(flag, ...) (0x63DEE0): it walks the sItem list for the object whose id (`+0x19C4`) equals `flag`, unlinks it and destroys it. `flag` IS the item object's
 * own id (pair run g032d, probe on 0x63DEE0: AddItem flag 252 = object +0x19C4 252 on both PCs). The id is a running number the creation function (0xA93440, `+0x19C4 = mgr->next()`)
 * hands out in the order the objects were created ON THAT PC, so it equals on both PCs only when both PCs created their objects in the same order (a warp / a fresh load) and
 * differs after a natural stage change (pair run g032b: the guest's AddItem flag was 128).
 * The PICKUP event carries the SENDER's flag; the receiver passed it unchanged to its own AddItem, so RemoveItemByFlag looked for the sender's number among ITS ids: nothing was
 * removed (the item stayed on the receiver's floor: the other human could pick it up again = a duplicate) or, when another object there had that number, the WRONG object was
 * removed. The replay must pass the receiver's own id of the object it found by LayoutID.
 * -DPK_MODEL_BEFORE_G032 = the rule before (the sender's flag as it came). */
#ifndef PICKUP_LOGIC_H
#define PICKUP_LOGIC_H

/* the flag argument for the replayed AddItem: sender_flag is the one in the event (0 = a call without an object id), have_own says that this PC's object was found and own_id is its
 * `+0x19C4`. */
static inline int pk_replay_flag(int sender_flag, int have_own, int own_id)
{
#ifdef PK_MODEL_BEFORE_G032
    (void)have_own, (void)own_id;
    return sender_flag;
#else
    if (!sender_flag || !have_own) return sender_flag;
    return own_id;
#endif
}

/* the replay used another number than the sender's (log line / counter) */
static inline int pk_flag_differs(int sender_flag, int used_flag) { return sender_flag != 0 && sender_flag != used_flag; }

/* 0.3.2 final regression (r032fs, also sc032g): the partner's GRANT of a story item for THIS PC's own human reached it before this PC's own story granted it at the end of the
 * same in-engine scene (the guest's knife table scene ends after the host's: the host's GRANT of Moira's flashlight 0155 came 29 ms before the guest's own scene end). The
 * GRANT put the item into the bag without the stock equip; this PC's own story op then found it there (story.inc h_putitem: not a second copy) and did not equip it either: the
 * guest's Moira ended the scene with the light in her bag and weapon slot but hand slot 0 (the host's copy of her: 4). A GRANT for this PC's own human therefore waits while an
 * in-engine scene plays here and PK_GRANT_AFTER_MS after its end: this PC's own story grants it with its own equip, and the waiting GRANT is dropped (the item is there);
 * when no own grant comes it is applied as before. A GRANT for the partner's copy and one outside a scene are applied at once (0.3.1).
 * -DPK_MODEL_BEFORE_GRANTHOLD = the rule before (always at once). */
#define PK_GRANT_AFTER_MS 2000u
#define PK_GRANT_HOLD_MAX_MS 90000u   /* never longer than this (a scene that does not end): applied as before */
enum { PK_GR_APPLY = 0, PK_GR_HOLD = 1, PK_GR_DROP = 2 };
/* own_human: the GRANT is for this PC's own human; have: this PC's copy of that slot holds the item already; scene_plays: an in-engine scene plays here now;
 * since_scene_end: ms since the last one here ended (0xFFFFFFFF = none since the session start); held_ms: how long the GRANT has waited */
static inline int pk_grant_decide(int own_human, int have, int scene_plays, unsigned since_scene_end, unsigned held_ms)
{
    if (have) return PK_GR_DROP;
#ifdef PK_MODEL_BEFORE_GRANTHOLD
    (void)own_human, (void)scene_plays, (void)since_scene_end, (void)held_ms;
    return PK_GR_APPLY;
#else
    if (!own_human || held_ms >= PK_GRANT_HOLD_MAX_MS) return PK_GR_APPLY;
    if (scene_plays || since_scene_end < PK_GRANT_AFTER_MS) return PK_GR_HOLD;
    return PK_GR_APPLY;
#endif
}

#endif
