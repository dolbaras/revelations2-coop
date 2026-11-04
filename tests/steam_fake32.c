/*
 * A fake game-side Steam for coop_steam_real.c (32-bit, built as steam_api.dll for test_steam_real32.exe).
 *
 * It builds the interface objects the way the game's Steam context holds them: a pointer to an object whose first member is the vtable, with
 * MSVC thiscall methods (this in ecx, arguments on the stack, callee pops; a CSteamID result through a hidden first pointer argument). The
 * vtable layouts are those of the SDK-1.3x interfaces the game uses (slot lists = the tables in net.c, which the stage 2b/2c logs verified); every
 * slot the code under test has no business calling is a trap that names the slot and ends the process. The methods check their arguments.
 * This catches typos in slot numbers, argument orders, hidden-pointer results, struct offsets and stack imbalance; it cannot prove that the real
 * Steam client agrees with the layouts (the hardware test does that).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "steam_fake32.h"

#define TC __attribute__((thiscall))
#define EXPORT __declspec(dllexport)
#define ULL(x) ((unsigned long long)(x))

typedef struct { void **vt; } Obj;

static const char *n_client[] = {"CreateSteamPipe", "BReleaseSteamPipe", "ConnectToGlobalUser", "CreateLocalUser", "ReleaseUser", "GetISteamUser",
    "GetISteamGameServer", "SetLocalIPBinding", "GetISteamFriends", "GetISteamUtils", "GetISteamMatchmaking", "GetISteamMatchmakingServers",
    "GetISteamGenericInterface", "GetISteamUserStats", "GetISteamGameServerStats", "GetISteamApps", "GetISteamNetworking", "GetISteamRemoteStorage",
    "GetISteamScreenshots", "RunFrame", "GetIPCCallCount", "SetWarningMessageHook", "BShutdownIfAllPipesClosed", "GetISteamHTTP", "GetISteamUnifiedMessages",
    "GetISteamController", "GetISteamUGC", "GetISteamAppList", "GetISteamMusic", "GetISteamMusicRemote", "GetISteamHTMLSurface"};
static const char *n_user[] = {"GetHSteamUser", "BLoggedOn", "GetSteamID", "InitiateGameConnection", "TerminateGameConnection", "TrackAppUsageEvent",
    "GetUserDataFolder", "StartVoiceRecording", "StopVoiceRecording", "GetAvailableVoice", "GetVoice", "DecompressVoice", "GetVoiceOptimalSampleRate",
    "GetAuthSessionTicket", "BeginAuthSession", "EndAuthSession", "CancelAuthTicket", "UserHasLicenseForApp", "BIsBehindNAT", "AdvertiseGame",
    "RequestEncryptedAppTicket", "GetEncryptedAppTicket", "GetGameBadgeLevel", "GetPlayerSteamLevel"};
static const char *n_friends[] = {"GetPersonaName", "SetPersonaName", "GetPersonaState", "GetFriendCount", "GetFriendByIndex", "GetFriendRelationship",
    "GetFriendPersonaState", "GetFriendPersonaName", "GetFriendGamePlayed", "GetFriendPersonaNameHistory", "GetFriendSteamLevel", "GetPlayerNickname",
    "GetFriendsGroupCount", "GetFriendsGroupIDByIndex", "GetFriendsGroupName", "GetFriendsGroupMembersCount", "GetFriendsGroupMembersList", "HasFriend",
    "GetClanCount", "GetClanByIndex", "GetClanName", "GetClanTag", "GetClanActivityCounts", "DownloadClanActivityCounts", "GetFriendCountFromSource",
    "GetFriendFromSourceByIndex", "IsUserInSource", "SetInGameVoiceSpeaking", "ActivateGameOverlay", "ActivateGameOverlayToUser",
    "ActivateGameOverlayToWebPage", "ActivateGameOverlayToStore", "SetPlayedWith", "ActivateGameOverlayInviteDialog", "GetSmallFriendAvatar",
    "GetMediumFriendAvatar", "GetLargeFriendAvatar", "RequestUserInformation", "RequestClanOfficerList", "GetClanOwner", "GetClanOfficerCount",
    "GetClanOfficerByIndex", "GetUserRestrictions", "SetRichPresence", "ClearRichPresence", "GetFriendRichPresence", "GetFriendRichPresenceKeyCount",
    "GetFriendRichPresenceKeyByIndex", "RequestFriendRichPresence", "InviteUserToGame", "GetCoplayFriendCount", "GetCoplayFriend"};
static const char *n_utils[] = {"GetSecondsSinceAppActive", "GetSecondsSinceComputerActive", "GetConnectedUniverse", "GetServerRealTime", "GetIPCountry",
    "GetImageSize", "GetImageRGBA", "GetCSERIPPort", "GetCurrentBatteryPower", "GetAppID", "SetOverlayNotificationPosition", "IsAPICallCompleted",
    "GetAPICallFailureReason", "GetAPICallResult", "RunFrame", "GetIPCCallCount", "SetWarningMessageHook", "IsOverlayEnabled", "BOverlayNeedsPresent",
    "CheckFileSignature", "ShowGamepadTextInput", "GetEnteredGamepadTextLength", "GetEnteredGamepadTextInput", "GetSteamUILanguage"};
static const char *n_mm[] = {"GetFavoriteGameCount", "GetFavoriteGame", "AddFavoriteGame", "RemoveFavoriteGame", "RequestLobbyList",
    "AddRequestLobbyListStringFilter", "AddRequestLobbyListNumericalFilter", "AddRequestLobbyListNearValueFilter", "AddRequestLobbyListFilterSlotsAvailable",
    "AddRequestLobbyListDistanceFilter", "AddRequestLobbyListResultCountFilter", "AddRequestLobbyListCompatibleMembersFilter", "GetLobbyByIndex",
    "CreateLobby", "JoinLobby", "LeaveLobby", "InviteUserToLobby", "GetNumLobbyMembers", "GetLobbyMemberByIndex", "GetLobbyData", "SetLobbyData",
    "GetLobbyDataCount", "GetLobbyDataByIndex", "DeleteLobbyData", "GetLobbyMemberData", "SetLobbyMemberData", "SendLobbyChatMsg", "GetLobbyChatEntry",
    "RequestLobbyData", "SetLobbyGameServer", "GetLobbyGameServer", "SetLobbyMemberLimit", "GetLobbyMemberLimit", "SetLobbyType", "SetLobbyJoinable",
    "GetLobbyOwner", "SetLobbyOwner", "SetLinkedLobby"};
static const char *n_net[] = {"SendP2PPacket", "IsP2PPacketAvailable", "ReadP2PPacket", "AcceptP2PSessionWithUser", "CloseP2PSessionWithUser",
    "CloseP2PChannelWithUser", "GetP2PSessionState", "AllowP2PPacketRelay", "CreateListenSocket", "CreateP2PConnectionSocket", "CreateConnectionSocket",
    "DestroySocket", "DestroyListenSocket", "SendDataOnSocket", "IsDataAvailableOnSocket", "RetrieveDataFromSocket", "IsDataAvailable", "RetrieveData",
    "GetSocketInfo", "GetListenSocketInfo", "GetSocketConnectionType", "GetMaxPacketSize"};
static const char *n_msgs[] = {"SendMessageToUser", "ReceiveMessagesOnChannel", "AcceptSessionWithUser", "CloseSessionWithUser", "CloseChannelWithUser", "GetSessionConnectionInfo"};
static const char *n_nutils[] = {"AllocateMessage", "InitRelayNetworkAccess", "GetRelayNetworkStatus"};

enum { I_CLIENT = 1, I_USER, I_FRIENDS, I_UTILS, I_MM, I_NET, I_MSGS, I_NUTILS };
static const struct { const char **names; int count; const char *name; } itab[] = {
    {NULL, 0, "?"}, {n_client, 31, "ISteamClient"}, {n_user, 24, "ISteamUser"}, {n_friends, 52, "ISteamFriends"}, {n_utils, 24, "ISteamUtils"},
    {n_mm, 38, "ISteamMatchmaking"}, {n_net, 22, "ISteamNetworking"}, {n_msgs, 6, "ISteamNetworkingMessages"}, {n_nutils, 3, "ISteamNetworkingUtils"}};

static void __cdecl trap_c(int id)
{
    int i = id / 100, s = id % 100;
    printf("FAKE STEAM TRAP: the code under test called %s slot %d (%s), which it must not call here\n", itab[i].name, s, s < itab[i].count ? itab[i].names[s] : "?");
    fflush(stdout);
    ExitProcess(3);
}
static void *make_trap(int id)
{
    static unsigned char *mem;
    static int n;
    if (!mem) mem = (unsigned char *)VirtualAlloc(NULL, 64 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char *t = mem + 16 * n++;
    t[0] = 0x68, *(int *)(t + 1) = id;
    t[5] = 0xE8, *(int *)(t + 6) = (int)((unsigned char *)trap_c - (t + 10));
    return t;
}

#define NV 64
static void *vt_client[NV], *vt_user[NV], *vt_friends[NV], *vt_utils[NV], *vt_mm[NV], *vt_net[NV], *vt_msgs[NV], *vt_nutils[NV];
static Obj o_client = {vt_client}, o_user = {vt_user}, o_friends = {vt_friends}, o_utils = {vt_utils}, o_mm = {vt_mm}, o_net = {vt_net}, o_msgs = {vt_msgs}, o_nutils = {vt_nutils};
static void *g_ctx[12];

/* ---------------------------------------------------------------------------------------- world */
#define ME (0x0110000100000000ull + 1000ull)
#define FRIEND (0x0110000100000000ull + 1001ull)
#define STRANGER (0x0110000100000000ull + 1002ull)
#define LOBBY0 (0x0186000000000000ull + 7000ull)
#define APPID 287290u

typedef struct { int used; uint64_t id; int type, max; uint64_t owner; uint64_t m[8]; int nm; char k[16][40], v[16][100]; int nd; int left; } lob_t;
typedef struct { uint64_t id; int kind; int polls; uint64_t lobby; int resp; int count; } call_t;
typedef struct { int via; uint64_t from; unsigned char data[2048]; int len; } inmsg_t;

static struct {
    lob_t lob[6];
    int n_lob_made;
    call_t calls[8];
    int n_calls;
    uint64_t next_call;
    uint64_t search[8];
    int nsearch;
    char sf_k[2][40], sf_v[2][40];
    int nsf;
    char pk[8][40], pv[8][100];
    int np;
    inmsg_t in[64];
    int ih, it;
    fake_sent sent[256];
    int ns;
    uint64_t acc[16];
    int nacc;
    uint64_t clo[16];
    int ncl;
    int releases, invites, dialogs, cbseen;
    int generic_ok, idoff;
    int run_pv_first;
    uint64_t friend_lobby;
    int friend_in_game;
    void *cbs[32];
    int nc;
    int polls_needed;
} W;

static lob_t *lob_of(uint64_t id)
{
    for (int i = 0; i < 6; i++)
        if (W.lob[i].used && W.lob[i].id == id) return &W.lob[i];
    return NULL;
}
static int is_member(lob_t *l, uint64_t u)
{
    for (int i = 0; i < l->nm; i++)
        if (l->m[i] == u) return 1;
    return 0;
}

/* ---- ISteamClient ---- */
static void *TC cl_GetGeneric(void *self, int hUser, int hPipe, const char *ver)
{
    (void)self;
    if (hUser != 11 || hPipe != 22) { printf("FAKE: GetISteamGenericInterface with handles %d/%d\n", hUser, hPipe); ExitProcess(4); }
    if (!W.generic_ok) return NULL;
    if (!strcmp(ver, "SteamNetworkingMessages002")) return &o_msgs;
    if (!strcmp(ver, "SteamNetworkingUtils004")) return &o_nutils;
    return NULL;
}

/* ---- ISteamUser019 ---- */
static unsigned char TC us_BLoggedOn(void *self) { (void)self; return 1; }
static uint64_t *TC us_GetSteamID(void *self, uint64_t *ret) { (void)self; *ret = ME; return ret; }

/* ---- ISteamFriends015 ---- */
static const char *TC fr_GetPersonaName(void *self) { (void)self; return "YoungOn"; }
static int TC fr_GetFriendCount(void *self, int flags)
{
    (void)self;
    if (flags != 4) { printf("FAKE: GetFriendCount flags %d (want 4 = immediate)\n", flags); ExitProcess(4); }
    return 1;
}
static uint64_t *TC fr_GetFriendByIndex(void *self, uint64_t *ret, int i, int flags)
{
    (void)self;
    if (i != 0 || flags != 4) { printf("FAKE: GetFriendByIndex(%d, %d)\n", i, flags); ExitProcess(4); }
    *ret = FRIEND;
    return ret;
}
static int TC fr_GetFriendRelationship(void *self, uint64_t id) { (void)self; return id == FRIEND ? 3 : id == STRANGER ? 0 : 0; }
static const char *TC fr_GetFriendPersonaName(void *self, uint64_t id)
{
    (void)self;
    return id == FRIEND ? "BeBeBe" : id == ME ? "YoungOn" : id == STRANGER ? "Stranger" : "";
}
static unsigned char TC fr_GetFriendGamePlayed(void *self, uint64_t id, void *info)
{
    (void)self;
    if (id != FRIEND || !W.friend_in_game) return 0;
    unsigned char *b = (unsigned char *)info;   /* FriendGameInfo_t: u64 gameid, u32 ip, u16 port, u16 qport, u64 lobby */
    memset(b, 0, 24);
    uint64_t game = APPID;
    memcpy(b, &game, 8);
    memcpy(b + 16, &W.friend_lobby, 8);
    return 1;
}
static void TC fr_ActivateGameOverlayInviteDialog(void *self, uint64_t lobby)
{
    (void)self;
    if (!lob_of(lobby)) { printf("FAKE: invite dialog for an unknown lobby %llu\n", ULL(lobby)); ExitProcess(4); }
    W.dialogs++;
}
static unsigned char TC fr_SetRichPresence(void *self, const char *k, const char *v)
{
    (void)self;
    for (int i = 0; i < W.np; i++)
        if (!strcmp(W.pk[i], k)) {
            if (!v) W.pk[i][0] = 0, W.pv[i][0] = 0;
            else snprintf(W.pv[i], sizeof W.pv[0], "%s", v);
            return 1;
        }
    if (!v) return 1;
    snprintf(W.pk[W.np], sizeof W.pk[0], "%s", k), snprintf(W.pv[W.np], sizeof W.pv[0], "%s", v);
    W.np++;
    return 1;
}
static void TC fr_ClearRichPresence(void *self) { (void)self; W.np = 0; }

/* ---- ISteamUtils ---- */
static call_t *call_of(uint64_t id)
{
    for (int i = 0; i < W.n_calls; i++)
        if (W.calls[i].id == id) return &W.calls[i];
    return NULL;
}
static unsigned char TC ut_IsAPICallCompleted(void *self, uint64_t call, unsigned char *failed)
{
    (void)self;
    call_t *c = call_of(call);
    if (!c) { printf("FAKE: IsAPICallCompleted for an unknown call %llu\n", ULL(call)); ExitProcess(4); }
    *failed = 0;
    return ++c->polls > W.polls_needed;
}
static unsigned char TC ut_GetAPICallResult(void *self, uint64_t call, void *buf, int size, int expected, unsigned char *failed)
{
    (void)self;
    call_t *c = call_of(call);
    *failed = 0;
    if (!c) return 0;
    uint8_t *b = (uint8_t *)buf;
    int want_id = c->kind == 1 ? 513 : c->kind == 2 ? 504 : 510, want_size = c->kind == 1 ? 16 : c->kind == 2 ? 20 : 4;
    if (expected != want_id || size < want_size) { printf("FAKE: GetAPICallResult expected %d size %d (want %d, >= %d)\n", expected, size, want_id, want_size); ExitProcess(4); }
    memset(b, 0, (size_t)size);
    if (c->kind == 1) {   /* LobbyCreated_t { int result; u64 lobby } */
        *(int *)b = c->resp;
        memcpy(b + 8, &c->lobby, 8);
    } else if (c->kind == 2) {   /* LobbyEnter_t { u64 lobby; u32 perms; bool locked; u32 response } */
        memcpy(b, &c->lobby, 8);
        *(int *)(b + 16) = c->resp;
    } else
        *(int *)b = c->count;
    c->id = 0;   /* consumed */
    return 1;
}

/* ---- ISteamMatchmaking009 ---- */
static uint64_t new_call(int kind, uint64_t lobby, int resp, int count)
{
    call_t *c = &W.calls[W.n_calls++ % 8];
    memset(c, 0, sizeof *c);
    c->id = ++W.next_call + 1000;
    c->kind = kind, c->lobby = lobby, c->resp = resp, c->count = count;
    return c->id;
}
static uint64_t TC mm_CreateLobby(void *self, int type, int max)
{
    (void)self;
    for (int i = 0; i < 6; i++)
        if (!W.lob[i].used) {
            lob_t *l = &W.lob[i];
            memset(l, 0, sizeof *l);
            l->used = 1, l->id = LOBBY0 + (uint64_t)W.n_lob_made++, l->type = type, l->max = max, l->owner = ME;
            l->m[l->nm++] = ME;
            return new_call(1, l->id, 1, 0);
        }
    return 0;
}
static uint64_t TC mm_JoinLobby(void *self, uint64_t id)
{
    (void)self;
    lob_t *l = lob_of(id);
    int resp = !l ? 2 : l->nm >= l->max ? 4 : 1;
    if (l && resp == 1 && !is_member(l, ME)) l->m[l->nm++] = ME;
    return new_call(2, id, resp, 0);
}
static void TC mm_LeaveLobby(void *self, uint64_t id)
{
    (void)self;
    lob_t *l = lob_of(id);
    if (!l) return;
    l->left++;
    for (int i = 0; i < l->nm; i++)
        if (l->m[i] == ME) l->m[i] = l->m[--l->nm];
    if (l->nm == 0) l->owner = 0;   /* an empty lobby is gone */
    else if (l->owner == ME) l->owner = l->m[0];
}
static unsigned char TC mm_InviteUserToLobby(void *self, uint64_t lobby, uint64_t user)
{
    (void)self;
    if (!lob_of(lobby) || user != FRIEND) { printf("FAKE: InviteUserToLobby(%llu, %llu)\n", ULL(lobby), ULL(user)); ExitProcess(4); }
    W.invites++;
    return 1;
}
static int TC mm_GetNumLobbyMembers(void *self, uint64_t id) { (void)self; lob_t *l = lob_of(id); return l ? l->nm : 0; }
static uint64_t *TC mm_GetLobbyMemberByIndex(void *self, uint64_t *ret, uint64_t id, int i)
{
    (void)self;
    lob_t *l = lob_of(id);
    *ret = l && i >= 0 && i < l->nm ? l->m[i] : 0;
    return ret;
}
static const char *TC mm_GetLobbyData(void *self, uint64_t id, const char *key)
{
    (void)self;
    lob_t *l = lob_of(id);
    for (int d = 0; l && d < l->nd; d++)
        if (!strcmp(l->k[d], key)) return l->v[d];
    return "";
}
static unsigned char TC mm_SetLobbyData(void *self, uint64_t id, const char *key, const char *val)
{
    (void)self;
    lob_t *l = lob_of(id);
    if (!l || l->owner != ME || !key || !val) return 0;
    for (int d = 0; d < l->nd; d++)
        if (!strcmp(l->k[d], key)) { snprintf(l->v[d], sizeof l->v[0], "%s", val); return 1; }
    snprintf(l->k[l->nd], sizeof l->k[0], "%s", key), snprintf(l->v[l->nd], sizeof l->v[0], "%s", val);
    l->nd++;
    return 1;
}
static int TC mm_GetLobbyMemberLimit(void *self, uint64_t id) { (void)self; lob_t *l = lob_of(id); return l ? l->max : 0; }
static uint64_t *TC mm_GetLobbyOwner(void *self, uint64_t *ret, uint64_t id) { (void)self; lob_t *l = lob_of(id); *ret = l ? l->owner : 0; return ret; }
static void TC mm_AddStringFilter(void *self, const char *k, const char *v, int cmp)
{
    (void)self;
    if (cmp != 0 || W.nsf >= 2) { printf("FAKE: string filter cmp %d (want 0 = equal)\n", cmp); ExitProcess(4); }
    snprintf(W.sf_k[W.nsf], 40, "%s", k), snprintf(W.sf_v[W.nsf], 40, "%s", v);
    W.nsf++;
}
static void TC mm_AddDistanceFilter(void *self, int d) { (void)self; if (d != 3) { printf("FAKE: distance filter %d (want 3 = worldwide)\n", d); ExitProcess(4); } }
static void TC mm_AddResultCountFilter(void *self, int n) { (void)self; if (n <= 0 || n > 50) { printf("FAKE: result count %d\n", n); ExitProcess(4); } }
static uint64_t TC mm_RequestLobbyList(void *self)
{
    (void)self;
    W.nsearch = 0;
    for (int i = 0; i < 6; i++) {
        lob_t *l = &W.lob[i];
        if (!l->used || l->type != 2) continue;
        int ok = 1;
        for (int f = 0; f < W.nsf; f++) {
            int found = 0;
            for (int d = 0; d < l->nd; d++)
                if (!strcmp(l->k[d], W.sf_k[f]) && !strcmp(l->v[d], W.sf_v[f])) found = 1;
            ok &= found;
        }
        if (ok && W.nsearch < 8) W.search[W.nsearch++] = l->id;
    }
    int n = W.nsearch;
    W.nsf = 0;
    return new_call(3, 0, 0, n);
}
static uint64_t *TC mm_GetLobbyByIndex(void *self, uint64_t *ret, int i)
{
    (void)self;
    *ret = i >= 0 && i < W.nsearch ? W.search[i] : 0;
    return ret;
}

/* ---- ISteamNetworking005 (legacy P2P with channels) ---- */
static void sent_add(int via, uint64_t to, int flags, int channel, const void *d, int len)
{
    if (W.ns >= 256) return;
    fake_sent *s = &W.sent[W.ns++];
    s->via = via, s->to = to, s->flags = flags, s->channel = channel, s->len = len;
    memcpy(s->data, d, (size_t)(len < 600 ? len : 600));
}
static unsigned char TC nw_SendP2PPacket(void *self, uint64_t to, const void *d, unsigned len, int type, int channel)
{
    (void)self;
    sent_add(1, to, type, channel, d, (int)len);
    return 1;
}
static inmsg_t *first_in(int via)
{
    for (int i = W.ih; i != W.it; i = (i + 1) % 64)
        if (W.in[i].via == via) return &W.in[i];
    return NULL;
}
static void drop_in(inmsg_t *m)
{
    int idx = (int)(m - W.in);
    while (idx != W.ih) {   /* shift the earlier entries up by one */
        int prev = (idx + 63) % 64;
        W.in[idx] = W.in[prev];
        idx = prev;
    }
    W.ih = (W.ih + 1) % 64;
}
static unsigned char TC nw_IsP2PPacketAvailable(void *self, unsigned *size, int channel)
{
    (void)self;
    if (channel == 0 || channel == 905) { printf("FAKE: the code under test polled the GAME's channel %d\n", channel); ExitProcess(5); }
    inmsg_t *m = first_in(1);
    if (!m) return 0;
    *size = (unsigned)m->len;
    return 1;
}
static unsigned char TC nw_ReadP2PPacket(void *self, void *dest, unsigned cap, unsigned *size, uint64_t *from, int channel)
{
    (void)self;
    if (channel == 0 || channel == 905) { printf("FAKE: the code under test read the GAME's channel %d\n", channel); ExitProcess(5); }
    inmsg_t *m = first_in(1);
    if (!m || (unsigned)m->len > cap) return 0;
    memcpy(dest, m->data, (size_t)m->len);
    *size = (unsigned)m->len, *from = m->from;
    drop_in(m);
    return 1;
}
static unsigned char TC nw_AcceptP2PSessionWithUser(void *self, uint64_t who) { (void)self; if (W.nacc < 16) W.acc[W.nacc++] = who; return 1; }
static unsigned char TC nw_CloseP2PChannelWithUser(void *self, uint64_t who, int channel)
{
    (void)self;
    if (channel == 0 || channel == 905) { printf("FAKE: the code under test closed the GAME's channel %d\n", channel); ExitProcess(5); }
    if (W.ncl < 16) W.clo[W.ncl++] = who;
    return 1;
}
static unsigned char TC nw_GetP2PSessionState(void *self, uint64_t who, void *st)
{
    (void)self, (void)who;
    memset(st, 0, 20);
    ((unsigned char *)st)[0] = 1;   /* active */
    return 1;
}

/* ---- ISteamNetworkingMessages002 ---- */
static void make_ident(unsigned char *b, uint64_t id) { memset(b, 0, 160), *(int *)b = 16, *(int *)(b + 4) = 8, memcpy(b + 8, &id, 8); }
static uint64_t ident_get(const unsigned char *b)
{
    uint64_t id = 0;
    if (*(const int *)b != 16 || *(const int *)(b + 4) != 8) { printf("FAKE: identity with type %d size %d (want 16 / 8)\n", *(const int *)b, *(const int *)(b + 4)); ExitProcess(4); }
    memcpy(&id, b + 8, 8);
    return id;
}
static int TC ms_SendMessageToUser(void *self, const void *ident, const void *d, unsigned len, int flags, int channel)
{
    (void)self;
    if (channel == 0) { printf("FAKE: Messages channel 0 used\n"); ExitProcess(5); }
    sent_add(0, ident_get((const unsigned char *)ident), flags, channel, d, (int)len);
    return 1;
}
static void __cdecl fake_msg_release(void *m)
{
    W.releases++;
    free(*(void **)m);
    free(m);
}
static int TC ms_ReceiveMessagesOnChannel(void *self, int channel, void **out, int max)
{
    (void)self;
    int n = 0;
    inmsg_t *m;
    while (n < max && (m = first_in(0)) != NULL) {
        unsigned char *msg = (unsigned char *)calloc(1, 256);
        void *data = malloc((size_t)m->len);
        memcpy(data, m->data, (size_t)m->len);
        *(void **)msg = data;
        *(int *)(msg + 4) = m->len;
        *(unsigned *)(msg + 8) = 77;   /* connection */
        make_ident(msg + W.idoff, m->from);   /* identity at +12 or +16 */
        *(void **)(msg + W.idoff + 164) = (void *)fake_msg_release;
        out[n++] = msg;
        drop_in(m);
    }
    if (channel == 0) { printf("FAKE: ReceiveMessagesOnChannel(0)\n"); ExitProcess(5); }
    return n;
}
static unsigned char TC ms_AcceptSessionWithUser(void *self, const void *ident) { (void)self; if (W.nacc < 16) W.acc[W.nacc++] = ident_get((const unsigned char *)ident); return 1; }
static unsigned char TC ms_CloseSessionWithUser(void *self, const void *ident) { (void)self; if (W.ncl < 16) W.clo[W.ncl++] = ident_get((const unsigned char *)ident); return 1; }
static int TC ms_GetSessionConnectionInfo(void *self, const void *ident, void *info, void *status)
{
    (void)self;
    ident_get((const unsigned char *)ident);
    if (info) { printf("FAKE: GetSessionConnectionInfo got an info pointer, the code passes NULL\n"); ExitProcess(4); }
    ((int *)status)[0] = 3;    /* connected */
    ((int *)status)[1] = 42;   /* ping */
    return 3;
}

/* ---- ISteamNetworkingUtils004 ---- */
static void TC nu_InitRelayNetworkAccess(void *self) { (void)self; }
static int TC nu_GetRelayNetworkStatus(void *self, void *d)
{
    (void)self;
    memset(d, 0, 512);
    ((int *)d)[0] = 100, ((int *)d)[2] = 100;
    return 100;
}

/* ---------------------------------------------------------------------------------------- exports of the "steam_api.dll" */
EXPORT int __cdecl SteamAPI_GetHSteamUser(void) { return 11; }
EXPORT int __cdecl SteamAPI_GetHSteamPipe(void) { return 22; }

typedef struct { void **vt; unsigned char flags; int id; } cbobj;
EXPORT void __cdecl SteamAPI_RegisterCallback(void *cb, int id)
{
    cbobj *o = (cbobj *)cb;
    o->flags |= 1, o->id = id;
    if (W.nc < 32) W.cbs[W.nc++] = cb;
}
EXPORT void __cdecl SteamAPI_UnregisterCallback(void *cb)
{
    for (int i = 0; i < W.nc; i++)
        if (W.cbs[i] == cb) W.cbs[i] = W.cbs[--W.nc];
}

static void f_fire(int id, const void *pv)
{
    for (int i = 0; i < W.nc; i++) {
        cbobj *o = (cbobj *)W.cbs[i];
        if (o->id != id) continue;
        /* how the DLL dispatches a broadcast callback: Run(param) through the slot of its own compiler's layout */
        void *fn = W.run_pv_first ? o->vt[0] : o->vt[1];
        ((void (TC *)(void *, void *))fn)(o, (void *)pv);
        int sz = ((int (TC *)(void *))o->vt[2])(o);
        if (sz <= 0) { printf("FAKE: callback %d reports size %d\n", id, sz); ExitProcess(4); }
        W.cbseen++;
    }
}
static int f_registered(int id)
{
    int n = 0;
    for (int i = 0; i < W.nc; i++) n += ((cbobj *)W.cbs[i])->id == id;
    return n;
}

static void fill(void **vt, int iface)
{
    for (int i = 0; i < NV; i++) vt[i] = make_trap(iface * 100 + i);
}

static void f_reset(void)
{
    void *cbs[32];
    int nc = W.nc, order = W.run_pv_first, gen = W.idoff ? W.generic_ok : 1, off = W.idoff ? W.idoff : 12;   /* configure() survives a reset */
    memcpy(cbs, W.cbs, sizeof cbs);
    memset(&W, 0, sizeof W);
    W.generic_ok = gen, W.idoff = off, W.polls_needed = 1, W.run_pv_first = order;
    memcpy(W.cbs, cbs, sizeof cbs);
    W.nc = nc;
}

static void f_configure(int generic, int idoff) { W.generic_ok = generic, W.idoff = idoff; }
static void *f_context(void) { return g_ctx; }
static uint64_t f_me(void) { return ME; }
static uint64_t f_friend(void) { return FRIEND; }
static uint64_t f_stranger(void) { return STRANGER; }
static void f_set_order(int pv_first) { W.run_pv_first = pv_first; }
static uint64_t f_owned(void)
{
    for (int i = 0; i < 6; i++)
        if (W.lob[i].used && W.lob[i].owner == ME) return W.lob[i].id;
    return 0;
}
static int f_members(uint64_t id, uint64_t *out, int max)
{
    lob_t *l = lob_of(id);
    int n = 0;
    for (int i = 0; l && i < l->nm && i < max; i++) out[n++] = l->m[i];
    return n;
}
static const char *f_ldata(uint64_t id, const char *key) { return mm_GetLobbyData(NULL, id, key); }
static int f_ltype(uint64_t id) { lob_t *l = lob_of(id); return l ? l->type : -1; }
static int f_lmax(uint64_t id) { lob_t *l = lob_of(id); return l ? l->max : 0; }
static uint64_t f_make_lobby(uint64_t owner, int type, int max, const char *tag)
{
    for (int i = 0; i < 6; i++)
        if (!W.lob[i].used) {
            lob_t *l = &W.lob[i];
            memset(l, 0, sizeof *l);
            l->used = 1, l->id = LOBBY0 + (uint64_t)W.n_lob_made++, l->type = type, l->max = max, l->owner = owner;
            l->m[l->nm++] = owner;
            char v[32];
            if (tag) {
                snprintf(l->k[l->nd], 40, "r2c"), snprintf(l->v[l->nd], 100, "%s", tag), l->nd++;
                snprintf(v, sizeof v, "%llu", ULL(owner));
                snprintf(l->k[l->nd], 40, "r2c_host"), snprintf(l->v[l->nd], 100, "%s", v), l->nd++;
            }
            return l->id;
        }
    return 0;
}
static void f_join(uint64_t id, uint64_t u) { lob_t *l = lob_of(id); if (l && l->nm < 8 && !is_member(l, u)) l->m[l->nm++] = u; }
static void f_leave(uint64_t id, uint64_t u)
{
    lob_t *l = lob_of(id);
    for (int i = 0; l && i < l->nm; i++)
        if (l->m[i] == u) l->m[i] = l->m[--l->nm];
}
static const char *f_presence(const char *key)
{
    for (int i = 0; i < W.np; i++)
        if (!strcmp(W.pk[i], key)) return W.pv[i];
    return "";
}
static int f_left(uint64_t id) { lob_t *l = lob_of(id); return l ? l->left : 0; }
static int f_invites(void) { return W.invites; }
static int f_dialogs(void) { return W.dialogs; }
static void f_friend_game(uint64_t lobby) { W.friend_in_game = 1; W.friend_lobby = lobby; }
static void f_inject(int via, uint64_t from, const void *d, int len)
{
    inmsg_t *m = &W.in[W.it];
    m->via = via, m->from = from, m->len = len;
    memcpy(m->data, d, (size_t)len);
    W.it = (W.it + 1) % 64;
}
static int f_sent_count(void) { return W.ns; }
static const fake_sent *f_sent_at(int i) { return i >= 0 && i < W.ns ? &W.sent[i] : NULL; }
static void f_sent_clear(void) { W.ns = 0; }
static int f_accepted(uint64_t who) { int n = 0; for (int i = 0; i < W.nacc; i++) n += W.acc[i] == who; return n; }
static int f_closed(uint64_t who) { int n = 0; for (int i = 0; i < W.ncl; i++) n += W.clo[i] == who; return n; }
static int f_releases(void) { return W.releases; }
static int f_cbseen(void) { return W.cbseen; }

static fake_api api = {f_context, f_reset, f_configure, f_me, f_friend, f_stranger, f_fire, f_registered, f_set_order, f_owned, f_members, f_ldata, f_ltype, f_lmax, f_make_lobby, f_join, f_leave, f_presence, f_left,
                       f_invites, f_dialogs, f_friend_game, f_inject, f_sent_count, f_sent_at, f_sent_clear, f_accepted, f_closed, f_releases, f_cbseen};
EXPORT fake_api *__cdecl fake_api_get(void) { return &api; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h, (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    fill(vt_client, I_CLIENT), fill(vt_user, I_USER), fill(vt_friends, I_FRIENDS), fill(vt_utils, I_UTILS), fill(vt_mm, I_MM), fill(vt_net, I_NET), fill(vt_msgs, I_MSGS), fill(vt_nutils, I_NUTILS);
    vt_client[12] = (void *)cl_GetGeneric;
    vt_user[1] = (void *)us_BLoggedOn, vt_user[2] = (void *)us_GetSteamID;
    vt_friends[0] = (void *)fr_GetPersonaName, vt_friends[3] = (void *)fr_GetFriendCount, vt_friends[4] = (void *)fr_GetFriendByIndex, vt_friends[5] = (void *)fr_GetFriendRelationship,
    vt_friends[7] = (void *)fr_GetFriendPersonaName, vt_friends[8] = (void *)fr_GetFriendGamePlayed, vt_friends[33] = (void *)fr_ActivateGameOverlayInviteDialog,
    vt_friends[43] = (void *)fr_SetRichPresence, vt_friends[44] = (void *)fr_ClearRichPresence;
    vt_utils[11] = (void *)ut_IsAPICallCompleted, vt_utils[13] = (void *)ut_GetAPICallResult;
    vt_mm[4] = (void *)mm_RequestLobbyList, vt_mm[5] = (void *)mm_AddStringFilter, vt_mm[9] = (void *)mm_AddDistanceFilter, vt_mm[10] = (void *)mm_AddResultCountFilter,
    vt_mm[12] = (void *)mm_GetLobbyByIndex, vt_mm[13] = (void *)mm_CreateLobby, vt_mm[14] = (void *)mm_JoinLobby, vt_mm[15] = (void *)mm_LeaveLobby, vt_mm[16] = (void *)mm_InviteUserToLobby,
    vt_mm[17] = (void *)mm_GetNumLobbyMembers, vt_mm[18] = (void *)mm_GetLobbyMemberByIndex, vt_mm[19] = (void *)mm_GetLobbyData, vt_mm[20] = (void *)mm_SetLobbyData,
    vt_mm[32] = (void *)mm_GetLobbyMemberLimit, vt_mm[35] = (void *)mm_GetLobbyOwner;
    vt_net[0] = (void *)nw_SendP2PPacket, vt_net[1] = (void *)nw_IsP2PPacketAvailable, vt_net[2] = (void *)nw_ReadP2PPacket, vt_net[3] = (void *)nw_AcceptP2PSessionWithUser,
    vt_net[5] = (void *)nw_CloseP2PChannelWithUser, vt_net[6] = (void *)nw_GetP2PSessionState;
    vt_msgs[0] = (void *)ms_SendMessageToUser, vt_msgs[1] = (void *)ms_ReceiveMessagesOnChannel, vt_msgs[2] = (void *)ms_AcceptSessionWithUser, vt_msgs[3] = (void *)ms_CloseSessionWithUser,
    vt_msgs[5] = (void *)ms_GetSessionConnectionInfo;
    vt_nutils[1] = (void *)nu_InitRelayNetworkAccess, vt_nutils[2] = (void *)nu_GetRelayNetworkStatus;
    g_ctx[0] = &o_client, g_ctx[1] = &o_user, g_ctx[2] = &o_friends, g_ctx[3] = &o_utils, g_ctx[4] = &o_mm, g_ctx[5] = (void *)0x1000, g_ctx[6] = (void *)0x1000, g_ctx[7] = (void *)0x1000, g_ctx[8] = &o_net;
    f_reset();
    return TRUE;
}
