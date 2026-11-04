/*
 * Control interface of the fake game-side Steam (steam_fake32.c, built as a 32-bit "steam_api.dll"): the test exe gets a struct of function
 * pointers with GetProcAddress("fake_api") and scripts the Steam world with it (members joining, messages arriving, callbacks firing) while
 * coop_steam_real.c drives the fake interfaces through the same MSVC thiscall vtables it uses on the game's real ones.
 */
#ifndef STEAM_FAKE32_H
#define STEAM_FAKE32_H

#include <stdint.h>

typedef struct fake_sent {
    int via;            /* 0 = SteamNetworkingMessages, 1 = legacy SteamNetworking005 */
    uint64_t to;
    int flags;          /* Messages: send flags; legacy: EP2PSend */
    int channel, len;
    unsigned char data[600];
} fake_sent;

typedef struct fake_api {
    void *(*context)(void);                                /* the CSteamAPIContext array handed to cst_note_context */
    void (*reset)(void);
    void (*configure)(int generic_interfaces, int id_offset);   /* generic_interfaces 0: GetISteamGenericInterface returns NULL; id_offset 12 or 16 */
    uint64_t (*me)(void);
    uint64_t (*friend_id)(void);
    uint64_t (*stranger_id)(void);
    void (*fire)(int id, const void *pv);                  /* dispatch a callback to every registered object (MSVC slot order or the other one) */
    int (*registered)(int id);                             /* how many callback objects are registered for id */
    void (*set_slot_order)(int run_pv_first);              /* 0 (default) = MSVC order: slot 0 Run(pv,failure,call), slot 1 Run(pv) */
    /* lobby world */
    uint64_t (*lobby_owned_by_me)(void);
    int (*lobby_members)(uint64_t lobby, uint64_t *out, int max);
    const char *(*lobby_data)(uint64_t lobby, const char *key);
    int (*lobby_type)(uint64_t lobby);
    int (*lobby_max)(uint64_t lobby);
    uint64_t (*make_lobby)(uint64_t owner, int type, int max, const char *tag);   /* a lobby owned by someone else (tag may be NULL), with the owner as member */
    void (*member_join)(uint64_t lobby, uint64_t user);
    void (*member_leave)(uint64_t lobby, uint64_t user);
    const char *(*presence)(const char *key);
    int (*left_lobby)(uint64_t lobby);                     /* LeaveLobby calls seen for it */
    int (*invites)(void);
    int (*dialogs)(void);
    void (*set_friend_in_game)(uint64_t lobby);            /* the friend plays the game and is in this lobby (0: no lobby) */
    /* pipe */
    void (*inject)(int via, uint64_t from, const void *data, int len);
    int (*sent_count)(void);
    const fake_sent *(*sent_at)(int i);
    void (*sent_clear)(void);
    int (*accepted)(uint64_t who);                         /* AcceptSessionWithUser / AcceptP2PSessionWithUser calls for him */
    int (*closed)(uint64_t who);
    int (*releases)(void);                                 /* messages released by the code under test */
    int (*callbacks_seen)(void);                           /* calls of the cb size / run functions that came back (sanity) */
} fake_api;

#endif
