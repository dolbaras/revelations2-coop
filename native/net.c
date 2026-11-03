/*
 * Read-only network capture (stage 2b). REV2_NETCAP=<prefix> turns it on (nothing is hooked without it):
 *   <prefix>.txt   one line per event: time, thread, call, arguments, result, caller as module+offset, first 64 payload bytes (hex)
 *   <prefix>.bin   binary records with the whole payload (up to 2048 bytes), see struct rec and tools\netcap.py
 *
 * What is logged (nothing is changed: every hook calls the original with the original arguments and returns its result):
 *   (a) the game's Winsock calls (ws2_32 import slots of the exe): socket, bind, listen, accept, connect, send, recv, sendto,
 *       recvfrom, closesocket, setsockopt, ioctlsocket, getsockname, WSAStartup; select only counted;
 *   (b) the game's calls into the Steam interfaces it gets from ISteamClient (steam.c / this file hook the accessors): every slot of
 *       ISteamUser, ISteamFriends, ISteamUtils, ISteamMatchmaking, ISteamNetworking, ISteamHTTP, ISteamApps is wrapped by a generic
 *       thunk. The thunk needs no signature: it swaps the return address on the stack for a trampoline (per-thread shadow stack),
 *       jumps to the original and, when it returns, logs the first stack arguments (strings are printed when an argument points to one),
 *       the 32/64-bit result, the caller and the payload of P2P / lobby-chat calls;
 *   (c) iphlpapi GetAdaptersAddresses / GetIfEntry (local address discovery).
 * Threads are named by their Win32 start address (exe+offset), so the module doing the traffic is visible.
 * Console: "net" (counters), "netmark <text>" (marker line in both files).
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "r2.h"

#define THISCALL __attribute__((thiscall))

/* ---------------------------------------------------------------- output ---- */
static HANDLE f_txt = INVALID_HANDLE_VALUE, f_bin = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION out_lock;
static int enabled;
static LARGE_INTEGER qpc_f, qpc_0;
static DWORD tick0;

#pragma pack(push, 1)
struct rec {
    unsigned t;        /* ms since net_install */
    unsigned tid;
    unsigned short kind;
    unsigned short chan;
    unsigned a, b, c;  /* kind specific (see enum) */
    unsigned len;      /* real payload length */
    unsigned caplen;   /* bytes that follow */
};
#pragma pack(pop)

enum {
    K_WS_SEND = 1, K_WS_RECV = 2, K_WS_SENDTO = 3, K_WS_RECVFROM = 4,   /* a=socket b=ip (network order) c=port (host order); sendto/recvfrom: the peer, send/recv: the connected peer or 0 */
    K_P2P_SEND = 10, K_P2P_READ = 11,                                   /* a=peer steamid low b=high c=send type (send) / 0; chan = channel */
    K_LOBBY_CHAT_SEND = 12, K_LOBBY_CHAT_ENTRY = 13,                    /* a=lobby low b=lobby high c=chat id / 0 */
    K_CALLBACK = 14,                                                    /* chan = callback id, a=callback object */
    K_MARK = 15
};

static unsigned now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (unsigned)((t.QuadPart - qpc_0.QuadPart) * 1000 / qpc_f.QuadPart);
}

static void put(HANDLE h, const void *p, DWORD n)
{
    DWORD w;
    if (h != INVALID_HANDLE_VALUE)
        WriteFile(h, p, n, &w, NULL);
}

static void hexs(const unsigned char *p, int n, int max, char *out, int size)
{
    int o = 0;
    if (n > max)
        n = max;
    for (int i = 0; i < n && o + 3 < size; i++)
        o += snprintf(out + o, size - o, "%02x", p[i]);
    out[o] = 0;
}

static const char *where(const void *addr, char *buf, int size)
{
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, '\\');
        snprintf(buf, size, "%s+%lx", b ? b + 1 : name, (unsigned long)((const char *)addr - (const char *)m));
    } else
        snprintf(buf, size, "%p", addr);
    return buf;
}

/* tid -> "start=exe+offset" (cached) */
typedef LONG (NTAPI *NtQueryInformationThread_t)(HANDLE, int, void *, ULONG, ULONG *);
static struct { DWORD tid; char desc[48]; } thr[96];
static int n_thr;
static const char *thread_desc(DWORD tid)
{
    for (int i = 0; i < n_thr; i++)
        if (thr[i].tid == tid)
            return thr[i].desc;
    if (n_thr >= 96)
        return "?";
    char d[48] = "?";
    static NtQueryInformationThread_t q;
    if (!q)
        q = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    if (h && q) {
        void *start = NULL;
        if (q(h, 9, &start, sizeof start, NULL) == 0 && start)
            where(start, d, sizeof d);
    }
    if (h)
        CloseHandle(h);
    thr[n_thr].tid = tid;
    snprintf(thr[n_thr].desc, sizeof thr[0].desc, "%s", d);
    return thr[n_thr++].desc;
}

static void txt(const char *fmt, ...)
{
    char line[1400];
    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD tid = GetCurrentThreadId();
    EnterCriticalSection(&out_lock);
    int o = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d +%u t%lu(%s) ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, now_ms(), tid, thread_desc(tid));
    va_list ap;
    va_start(ap, fmt);
    o += vsnprintf(line + o, sizeof line - o - 2, fmt, ap);
    va_end(ap);
    if (o > (int)sizeof line - 3)
        o = sizeof line - 3;
    line[o++] = '\n';
    put(f_txt, line, o);
    LeaveCriticalSection(&out_lock);
}

static void bin(int kind, int chan, unsigned a, unsigned b, unsigned c, const void *data, unsigned len)
{
    struct rec r;
    unsigned cap = len > 2048 ? 2048 : len;
    r.t = now_ms();
    r.tid = GetCurrentThreadId();
    r.kind = (unsigned short)kind;
    r.chan = (unsigned short)chan;
    r.a = a, r.b = b, r.c = c, r.len = len, r.caplen = data ? cap : 0;
    EnterCriticalSection(&out_lock);
    put(f_bin, &r, sizeof r);
    if (data && cap)
        put(f_bin, data, cap);
    LeaveCriticalSection(&out_lock);
}

static const char *addr_str(const struct sockaddr *sa, int len, char *buf, int size)
{
    if (sa && len >= (int)sizeof(struct sockaddr_in) && sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        snprintf(buf, size, "%u.%u.%u.%u:%u", in->sin_addr.S_un.S_un_b.s_b1, in->sin_addr.S_un.S_un_b.s_b2, in->sin_addr.S_un.S_un_b.s_b3,
                 in->sin_addr.S_un.S_un_b.s_b4, ntohs(in->sin_port));
    } else if (sa)
        snprintf(buf, size, "family%d/len%d", sa->sa_family, len);
    else
        snprintf(buf, size, "-");
    return buf;
}

/* ------------------------------------------------------------- sockets ---- */
static struct { SOCKET s; int type, proto; char local[32], remote[32]; unsigned sent, recvd, nsent, nrecv; DWORD tid; int live; } sk[256];

static int sk_find(SOCKET s, int create)
{
    int free_i = -1;
    for (int i = 0; i < 256; i++) {
        if (sk[i].live && sk[i].s == s)
            return i;
        if (!sk[i].live && free_i < 0)
            free_i = i;
    }
    if (create && free_i >= 0) {
        memset(&sk[free_i], 0, sizeof sk[0]);
        sk[free_i].s = s, sk[free_i].live = 1, sk[free_i].tid = GetCurrentThreadId();
        snprintf(sk[free_i].local, sizeof sk[0].local, "-"), snprintf(sk[free_i].remote, sizeof sk[0].remote, "-");
        return free_i;
    }
    return -1;
}

static int (WSAAPI *real_WSAStartup)(WORD, LPWSADATA);
static SOCKET (WSAAPI *real_socket)(int, int, int);
static int (WSAAPI *real_bind)(SOCKET, const struct sockaddr *, int);
static int (WSAAPI *real_listen)(SOCKET, int);
static SOCKET (WSAAPI *real_accept)(SOCKET, struct sockaddr *, int *);
static int (WSAAPI *real_connect)(SOCKET, const struct sockaddr *, int);
static int (WSAAPI *real_send)(SOCKET, const char *, int, int);
static int (WSAAPI *real_recv)(SOCKET, char *, int, int);
static int (WSAAPI *real_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static int (WSAAPI *real_recvfrom)(SOCKET, char *, int, int, struct sockaddr *, int *);
static int (WSAAPI *real_closesocket)(SOCKET);
static int (WSAAPI *real_setsockopt)(SOCKET, int, int, const char *, int);
static int (WSAAPI *real_ioctlsocket)(SOCKET, long, u_long *);
static int (WSAAPI *real_getsockname)(SOCKET, struct sockaddr *, int *);
static int (WSAAPI *real_select)(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
static volatile LONG n_select, n_wouldblock;

#define CALLER where(__builtin_return_address(0), cb_, sizeof cb_)

static int WSAAPI hook_WSAStartup(WORD v, LPWSADATA d)
{
    char cb_[80];
    int r = real_WSAStartup(v, d);
    txt("ws WSAStartup(0x%04x) -> %d caller %s", v, r, CALLER);
    return r;
}

static SOCKET WSAAPI hook_socket(int af, int type, int proto)
{
    char cb_[80];
    SOCKET s = real_socket(af, type, proto);
    int e = WSAGetLastError();
    int i = s != INVALID_SOCKET ? sk_find(s, 1) : -1;
    if (i >= 0)
        sk[i].type = type, sk[i].proto = proto;
    txt("ws socket(af=%d type=%d proto=%d) -> %u caller %s", af, type, proto, (unsigned)s, CALLER);
    WSASetLastError(e);
    return s;
}

static int WSAAPI hook_bind(SOCKET s, const struct sockaddr *a, int len)
{
    char cb_[80], ab[48];
    int r = real_bind(s, a, len), e = WSAGetLastError();
    int i = sk_find(s, 1);
    addr_str(a, len, ab, sizeof ab);
    if (i >= 0)
        snprintf(sk[i].local, sizeof sk[0].local, "%s", ab);
    txt("ws bind(%u, %s) -> %d err %d caller %s", (unsigned)s, ab, r, r ? e : 0, CALLER);
    WSASetLastError(e);
    return r;
}

static int WSAAPI hook_listen(SOCKET s, int backlog)
{
    char cb_[80];
    int r = real_listen(s, backlog), e = WSAGetLastError();
    txt("ws listen(%u, %d) -> %d err %d caller %s", (unsigned)s, backlog, r, r ? e : 0, CALLER);
    WSASetLastError(e);
    return r;
}

static SOCKET WSAAPI hook_accept(SOCKET s, struct sockaddr *a, int *len)
{
    char cb_[80], ab[48];
    SOCKET n = real_accept(s, a, len);
    int e = WSAGetLastError();
    if (n != INVALID_SOCKET) {
        int i = sk_find(n, 1);
        addr_str(a, len ? *len : 0, ab, sizeof ab);
        if (i >= 0)
            snprintf(sk[i].remote, sizeof sk[0].remote, "%s", ab), sk[i].type = SOCK_STREAM;
        txt("ws accept(%u) -> %u from %s caller %s", (unsigned)s, (unsigned)n, ab, CALLER);
    } else if (e != WSAEWOULDBLOCK)
        txt("ws accept(%u) failed err %d caller %s", (unsigned)s, e, CALLER);
    WSASetLastError(e);
    return n;
}

static int WSAAPI hook_connect(SOCKET s, const struct sockaddr *a, int len)
{
    char cb_[80], ab[48];
    int r = real_connect(s, a, len), e = WSAGetLastError();
    int i = sk_find(s, 1);
    addr_str(a, len, ab, sizeof ab);
    if (i >= 0)
        snprintf(sk[i].remote, sizeof sk[0].remote, "%s", ab);
    txt("ws connect(%u, %s) -> %d err %d caller %s", (unsigned)s, ab, r, r ? e : 0, CALLER);
    WSASetLastError(e);
    return r;
}

static void payload(const char *tag, SOCKET s, int kind, const struct sockaddr *peer, int plen, const char *buf, int len, int asked, int r, int e, const void *caller)
{
    char ab[48] = "", hx[140] = "", cb_[80];
    unsigned ip = 0, port = 0;
    if (peer && plen >= (int)sizeof(struct sockaddr_in) && peer->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)peer;
        ip = in->sin_addr.S_un.S_addr, port = ntohs(in->sin_port);
        addr_str(peer, plen, ab, sizeof ab);
    }
    int i = sk_find(s, 0);
    if (i >= 0) {
        if (kind == K_WS_SEND || kind == K_WS_SENDTO)
            sk[i].sent += len > 0 ? len : 0, sk[i].nsent++;
        else
            sk[i].recvd += len > 0 ? len : 0, sk[i].nrecv++;
        if (!ab[0])
            snprintf(ab, sizeof ab, "%s", sk[i].remote);
    }
    if (len > 0)
        hexs((const unsigned char *)buf, len, 64, hx, sizeof hx);
    txt("ws %s(%u%s%s) asked %d -> %d%s caller %s%s%s", tag, (unsigned)s, ab[0] ? " peer " : "", ab, asked, r, r < 0 ? " (error)" : "", where(caller, cb_, sizeof cb_),
        hx[0] ? " data " : "", hx);
    (void)e;
    if (len > 0)
        bin(kind, 0, (unsigned)s, ip, port, buf, len);
}

static int WSAAPI hook_send(SOCKET s, const char *buf, int len, int flags)
{
    int r = real_send(s, buf, len, flags), e = WSAGetLastError();
    payload("send", s, K_WS_SEND, NULL, 0, buf, r > 0 ? r : 0, len, r, e, __builtin_return_address(0));
    WSASetLastError(e);
    return r;
}

static int WSAAPI hook_recv(SOCKET s, char *buf, int len, int flags)
{
    int r = real_recv(s, buf, len, flags), e = WSAGetLastError();
    if (r < 0 && e == WSAEWOULDBLOCK) {
        InterlockedIncrement(&n_wouldblock);
        return r;
    }
    payload("recv", s, K_WS_RECV, NULL, 0, buf, r > 0 ? r : 0, len, r, e, __builtin_return_address(0));
    WSASetLastError(e);
    return r;
}

static int WSAAPI hook_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    int r = real_sendto(s, buf, len, flags, to, tolen), e = WSAGetLastError();
    payload("sendto", s, K_WS_SENDTO, to, tolen, buf, r > 0 ? r : 0, len, r, e, __builtin_return_address(0));
    WSASetLastError(e);
    return r;
}

static int WSAAPI hook_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    int r = real_recvfrom(s, buf, len, flags, from, fromlen), e = WSAGetLastError();
    if (r < 0 && e == WSAEWOULDBLOCK) {
        InterlockedIncrement(&n_wouldblock);
        return r;
    }
    payload("recvfrom", s, K_WS_RECVFROM, from, fromlen ? *fromlen : 0, buf, r > 0 ? r : 0, len, r, e, __builtin_return_address(0));
    WSASetLastError(e);
    return r;
}

static int WSAAPI hook_closesocket(SOCKET s)
{
    char cb_[80];
    int i = sk_find(s, 0);
    if (i >= 0)
        txt("ws closesocket(%u) type %d local %s remote %s sent %u bytes/%u calls, received %u bytes/%u calls, opened by t%lu, caller %s", (unsigned)s,
            sk[i].type, sk[i].local, sk[i].remote, sk[i].sent, sk[i].nsent, sk[i].recvd, sk[i].nrecv, sk[i].tid, CALLER);
    else
        txt("ws closesocket(%u) (unknown socket) caller %s", (unsigned)s, CALLER);
    int r = real_closesocket(s), e = WSAGetLastError();
    if (i >= 0)
        sk[i].live = 0;
    WSASetLastError(e);
    return r;
}

static int WSAAPI hook_setsockopt(SOCKET s, int level, int name, const char *val, int len)
{
    char cb_[80], hx[40] = "";
    if (val && len > 0)
        hexs((const unsigned char *)val, len, 16, hx, sizeof hx);
    int r = real_setsockopt(s, level, name, val, len), e = WSAGetLastError();
    txt("ws setsockopt(%u, level %d, opt %d, value %s) -> %d caller %s", (unsigned)s, level, name, hx, r, CALLER);
    WSASetLastError(e);
    return r;
}

static int WSAAPI hook_ioctlsocket(SOCKET s, long cmd, u_long *arg)
{
    char cb_[80];
    u_long v = arg ? *arg : 0;
    int r = real_ioctlsocket(s, cmd, arg), e = WSAGetLastError();
    txt("ws ioctlsocket(%u, 0x%lx, %lu) -> %d caller %s", (unsigned)s, (unsigned long)cmd, (unsigned long)v, r, CALLER);
    WSASetLastError(e);
    return r;
}

static int WSAAPI hook_getsockname(SOCKET s, struct sockaddr *a, int *len)
{
    char cb_[80], ab[48];
    int r = real_getsockname(s, a, len), e = WSAGetLastError();
    addr_str(a, len ? *len : 0, ab, sizeof ab);
    int i = sk_find(s, 0);
    if (i >= 0 && r == 0)
        snprintf(sk[i].local, sizeof sk[0].local, "%s", ab);
    txt("ws getsockname(%u) -> %d %s caller %s", (unsigned)s, r, ab, CALLER);
    WSASetLastError(e);
    return r;
}

static int WSAAPI hook_select(int n, fd_set *r, fd_set *w, fd_set *x, const struct timeval *t)
{
    InterlockedIncrement(&n_select);
    return real_select(n, r, w, x, t);
}

/* iphlpapi */
static ULONG (WINAPI *real_GetAdaptersAddresses)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
static DWORD (WINAPI *real_GetIfEntry)(PMIB_IFROW);
static volatile LONG n_ifentry;
static ULONG WINAPI hook_GetAdaptersAddresses(ULONG fam, ULONG flags, PVOID res, PIP_ADAPTER_ADDRESSES a, PULONG len)
{
    char cb_[80];
    ULONG r = real_GetAdaptersAddresses(fam, flags, res, a, len);
    txt("iphlpapi GetAdaptersAddresses(family %lu, flags 0x%lx) -> %lu caller %s", fam, flags, r, CALLER);
    return r;
}
static DWORD WINAPI hook_GetIfEntry(PMIB_IFROW row)
{
    DWORD r = real_GetIfEntry(row);
    InterlockedIncrement(&n_ifentry);
    return r;
}

/* ------------------------------------------------- generic Steam thunks ---- */
enum { F_CLIENT = 1, F_USER, F_FRIENDS, F_UTILS, F_MM, F_MMSERVERS, F_NET, F_HTTP, F_APPS };
static const char *fam_name[] = {"?", "ISteamClient", "ISteamUser", "ISteamFriends", "ISteamUtils", "ISteamMatchmaking", "ISteamMatchmakingServers",
                                  "ISteamNetworking", "ISteamHTTP", "ISteamApps"};

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
static const char *n_http[] = {"CreateHTTPRequest", "SetHTTPRequestContextValue", "SetHTTPRequestNetworkActivityTimeout", "SetHTTPRequestHeaderValue",
    "SetHTTPRequestGetOrPostParameter", "SendHTTPRequest", "SendHTTPRequestAndStreamResponse", "DeferHTTPRequest", "PrioritizeHTTPRequest",
    "GetHTTPResponseHeaderSize", "GetHTTPResponseHeaderValue", "GetHTTPResponseBodySize", "GetHTTPResponseBodyData", "GetHTTPStreamingResponseBodyData",
    "ReleaseHTTPRequest", "GetHTTPDownloadProgressPct", "SetHTTPRequestRawPostBody", "CreateCookieContainer", "ReleaseCookieContainer", "SetCookie",
    "SetHTTPRequestCookieContainer", "SetHTTPRequestUserAgentInfo", "SetHTTPRequestRequiresVerifiedCertificate", "SetHTTPRequestAbsoluteTimeoutMS",
    "GetHTTPRequestWasTimedOut"};

static const struct { int fam; const char **names; int count; } famtab[] = {
    {F_CLIENT, n_client, 31}, {F_USER, n_user, 24}, {F_FRIENDS, n_friends, 52}, {F_UTILS, n_utils, 24}, {F_MM, n_mm, 38}, {F_MMSERVERS, NULL, 14},
    {F_NET, n_net, 22}, {F_HTTP, n_http, 25}, {F_APPS, NULL, 22}};

typedef struct {
    void *orig;
    unsigned char fam, slot;
    unsigned calls, logged;
    unsigned long long last;
    int always;
    const char *name;
} Hk;
#define MAXHK 640
static Hk hk[MAXHK];
static int n_hk;
static unsigned char *thunks;
static void **hooked_vt[64];
static int n_vt;

typedef struct { unsigned ret, id, self, arg[8]; } Frame;
typedef struct { int depth; Frame f[40]; } Shadow;
static DWORD tls_idx = TLS_OUT_OF_INDEXES;

extern void net_common(void);
extern void net_leave_entry(void);

__asm__(
    ".text\n"
    ".globl _net_common\n"
    "_net_common:\n"            /* [esp]=id [esp+4]=return address [esp+8..]=arguments, ecx=this */
    "  push %ecx\n"
    "  lea 8(%esp), %eax\n"
    "  push %eax\n"
    "  push %ecx\n"
    "  push 12(%esp)\n"
    "  call _net_enter\n"
    "  add $12, %esp\n"
    "  pop %ecx\n"
    "  add $4, %esp\n"
    "  jmp *%eax\n"
    ".globl _net_leave_entry\n"
    "_net_leave_entry:\n"       /* the original returned here (its arguments are already popped); eax/edx = result */
    "  push $0\n"
    "  push %eax\n"
    "  push %edx\n"
    "  lea 8(%esp), %ecx\n"
    "  push %ecx\n"
    "  push %edx\n"
    "  push %eax\n"
    "  call _net_leave\n"
    "  add $12, %esp\n"
    "  pop %edx\n"
    "  pop %eax\n"
    "  ret\n");

static Shadow *shadow(void)
{
    Shadow *s = (Shadow *)TlsGetValue(tls_idx);
    if (!s) {
        s = (Shadow *)LocalAlloc(LPTR, sizeof(Shadow));
        TlsSetValue(tls_idx, s);
    }
    return s;
}

void *__cdecl net_enter(unsigned id, unsigned self, unsigned *p_ret)
{
    Hk *h = &hk[id];
    Shadow *s = shadow();
    if (!s || s->depth >= 40)
        return h->orig;
    Frame *f = &s->f[s->depth++];
    f->ret = *p_ret, f->id = id, f->self = self;
    memcpy(f->arg, p_ret + 1, sizeof f->arg);
    *p_ret = (unsigned)net_leave_entry;
    return h->orig;
}

static int is_text(const char *p, char *out, int size)
{
    unsigned char b[100];
    if ((unsigned)(uintptr_t)p < 0x10000 || !r2_safe_read(p, b, 1))
        return 0;
    int n = 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), p, b, sizeof b, &got) || got == 0)
        return 0;
    while (n < (int)got && b[n] >= 0x20 && b[n] < 0x7f)
        n++;
    if (n < 2 || (n < (int)got && b[n] != 0) || n >= (int)sizeof b - 1)
        return 0;
    if (n >= size)
        n = size - 1;
    memcpy(out, b, n), out[n] = 0;
    return 1;
}

static void net_iface(void *p, const char *ver, int fam);
static int ret_kind(int fam, int slot);
static int nargs(int fam, int slot);
static int p2p_channel;   /* SteamNetworking006+ has a channel argument; the game asks for 005 (no channel) */
static int fam_of_accessor(int slot)
{
    switch (slot) {
    case 5: return F_USER;
    case 8: return F_FRIENDS;
    case 9: return F_UTILS;
    case 10: return F_MM;
    case 11: return F_MMSERVERS;
    case 15: return F_APPS;
    case 16: return F_NET;
    case 23: return F_HTTP;
    }
    return 0;
}

/* special handling after the original returned */
static void post(Hk *h, Frame *f, unsigned eax, unsigned edx)
{
    unsigned *a = f->arg;
    if (h->fam == F_CLIENT) {
        int fam = fam_of_accessor(h->slot);
        char v[80];
        if (h->slot >= 5 && h->slot != 12 && h->slot != 13 && is_text((const char *)a[2], v, sizeof v)) {
            txt("steam ISteamClient::%s(\"%s\") -> %08x", h->name, v, eax);
            if (eax)
                net_iface((void *)eax, v, fam);
        }
        return;
    }
    if (h->fam == F_NET) {
        if (h->slot == 0) {       /* SendP2PPacket(steamid lo, hi, data, len, type, channel) */
            char hx[140];
            hexs((const unsigned char *)a[2], a[3] < 4096 ? a[3] : 0, 64, hx, sizeof hx);
            int ch = p2p_channel ? (int)a[5] : -1;
            txt("p2p SEND peer %08x%08x ch %d type %d len %u ok=%d data %s", a[1], a[0], ch, a[4], a[3], eax & 0xff, hx);
            bin(K_P2P_SEND, ch & 0xffff, a[0], a[1], a[4], (void *)a[2], a[3] < 65536 ? a[3] : 0);
        } else if (h->slot == 2 && (eax & 0xff)) {   /* ReadP2PPacket(dest, destsize, size*, steamid*, channel) */
            unsigned sz = 0, id[2] = {0, 0};
            r2_safe_read((void *)a[2], &sz, 4), r2_safe_read((void *)a[3], id, 8);
            char hx[140];
            hexs((const unsigned char *)a[0], sz < 4096 ? sz : 0, 64, hx, sizeof hx);
            int ch = p2p_channel ? (int)a[4] : -1;
            txt("p2p READ peer %08x%08x ch %d len %u data %s", id[1], id[0], ch, sz, hx);
            bin(K_P2P_READ, ch & 0xffff, id[0], id[1], 0, (void *)a[0], sz < 65536 ? sz : 0);
        }
    } else if (h->fam == F_MM) {
        if (h->slot == 26) {      /* SendLobbyChatMsg(lobby lo, hi, data, len) */
            char hx[140];
            hexs((const unsigned char *)a[2], a[3] < 4096 ? a[3] : 0, 64, hx, sizeof hx);
            txt("lobby CHAT SEND lobby %08x%08x len %u ok=%d data %s", a[1], a[0], a[3], eax & 0xff, hx);
            bin(K_LOBBY_CHAT_SEND, 0, a[0], a[1], 0, (void *)a[2], a[3] < 65536 ? a[3] : 0);
        } else if (h->slot == 27 && (int)eax > 0) {   /* GetLobbyChatEntry(lobby lo, hi, chatid, user*, data, size, type*) */
            unsigned id[2] = {0, 0};
            r2_safe_read((void *)a[3], id, 8);
            char hx[140];
            hexs((const unsigned char *)a[4], eax < 4096 ? eax : 0, 64, hx, sizeof hx);
            txt("lobby CHAT ENTRY lobby %08x%08x id %u from %08x%08x len %d data %s", a[1], a[0], a[2], id[1], id[0], (int)eax, hx);
            bin(K_LOBBY_CHAT_ENTRY, 0, a[0], a[1], a[2], (void *)a[4], eax < 65536 ? eax : 0);
        }
    }
}

void __cdecl net_leave(unsigned eax, unsigned edx, unsigned *slot)
{
    Shadow *s = shadow();
    Frame *f = &s->f[--s->depth];
    Hk *h = &hk[f->id];
    *slot = f->ret;
    h->calls++;
    unsigned long long res = ((unsigned long long)edx << 32) | eax;
    int changed = res != h->last;
    h->last = res;
    int special = h->fam == F_NET ? (h->slot == 0 || h->slot == 2) : 0;
    if (h->fam == F_MM && (h->slot == 26 || h->slot == 27))
        special = 1;
    if (h->fam == F_CLIENT || special)
        post(h, f, eax, edx);
    if (h->fam == F_CLIENT || special)
        return;
    /* generic line */
    if (!(h->always || h->logged < 30 || (changed && h->logged < 400)))
        return;
    h->logged++;
    char line[600], s1[100], cb_[80];
    int o = snprintf(line, sizeof line, "%s::%s(", fam_name[h->fam], h->name);
    int na = nargs(h->fam, h->slot);
    for (int i = 0; i < na; i++) {
        if (i)
            o += snprintf(line + o, sizeof line - o - 1, ", ");
        if (is_text((const char *)f->arg[i], s1, sizeof s1))
            o += snprintf(line + o, sizeof line - o - 1, "\"%s\"", s1);
        else
            o += snprintf(line + o, sizeof line - o - 1, "%x", f->arg[i]);
    }
    o += snprintf(line + o, sizeof line - o - 1, ") this=%x -> %x", f->self, eax);
    if (ret_kind(h->fam, h->slot) == 1)
        o += snprintf(line + o, sizeof line - o - 1, " (64-bit %08x%08x)", edx, eax);
    if (is_text((const char *)eax, s1, sizeof s1))
        o += snprintf(line + o, sizeof line - o - 1, " \"%s\"", s1);
    if (ret_kind(h->fam, h->slot) == 2) {
        unsigned id[2] = {0, 0};
        r2_safe_read((void *)f->arg[0], id, 8);
        o += snprintf(line + o, sizeof line - o - 1, " steamid %08x%08x", id[1], id[0]);
    }
    txt("steam %s caller %s", line, where((void *)f->ret, cb_, sizeof cb_));
}

/* results that are not one plain dword: 1 = 64-bit in edx:eax, 2 = CSteamID through a hidden first pointer argument (MSVC returns a class with a constructor in memory) */
static int ret_kind(int fam, int slot)
{
    if (fam == F_MM && (slot == 4 || slot == 13 || slot == 14))
        return 1;
    if (fam == F_MM && (slot == 12 || slot == 18 || slot == 35))
        return 2;
    if (fam == F_FRIENDS && (slot == 4 || slot == 19 || slot == 39 || slot == 41))
        return 2;
    if (fam == F_FRIENDS && (slot == 23 || slot == 38))
        return 1;
    if (fam == F_USER && (slot == 2 || slot == 20))
        return slot == 2 ? 2 : 1;
    return 0;
}

/* argument dwords after "this" worth printing (4 when not known) */
static int nargs(int fam, int slot)
{
    static const signed char mm[38] = {0, 4, 4, 4, 0, 3, 3, 2, 1, 1, 1, 2, 2, 2, 2, 2, 4, 2, 4, 3, 4, 2, 7, 3, 5, 4, 4, 7, 2, 6, 4, 3, 2, 3, 3, 3, 4, 4};
    static const signed char net[22] = {5, 1, 4, 2, 2, 3, 3, 1, 4, 4, 4, 1, 1, 4, 1, 4, 1, 4, 4, 4, 1, 0};
    static const signed char http[25] = {2, 3, 2, 3, 3, 2, 2, 1, 1, 2, 4, 2, 4, 5, 1, 2, 4, 0, 1, 4, 2, 2, 2, 2, 2};
    static const signed char fr[52] = {0, 1, 0, 1, 3, 2, 2, 2, 3, 3, 2, 2, 0, 1, 1, 1, 3, 3, 0, 2, 2, 2, 3, 2, 3, 4, 3, 1, 1, 3, 2, 3, 3, 3, 2, 2, 2, 3, 2, 2, 2, 3, 0, 2, 0, 4, 2, 4, 2, 5, 0, 3};
    static const signed char user[24] = {0, 0, 1, 6, 2, 1, 2, 0, 0, 1, 6, 6, 0, 3, 5, 2, 1, 2, 0, 2, 1, 4, 0, 0};
    switch (fam) {
    case F_MM: return slot < 38 ? mm[slot] : 4;
    case F_NET: return slot < 22 ? net[slot] : 4;
    case F_HTTP: return slot < 25 ? http[slot] : 4;
    case F_FRIENDS: return slot < 52 ? fr[slot] : 4;
    case F_USER: return slot < 24 ? user[slot] : 4;
    case F_UTILS: return slot == 9 ? 0 : slot == 13 ? 6 : slot == 11 ? 2 : 2;
    }
    return 4;
}

static int always_slot(int fam, int slot)
{
    switch (fam) {
    case F_MM: return slot == 4 || (slot >= 5 && slot <= 11) || slot == 12 || (slot >= 13 && slot <= 16) || slot == 20 || slot == 23 || slot == 25 || slot == 28 ||
                      slot == 29 || slot == 31 || slot == 33 || slot == 34 || slot == 35 || slot == 36 || slot == 37;
    case F_NET: return slot == 3 || slot == 4 || slot == 5 || slot == 6 || slot == 7 || slot == 8 || slot == 9 || slot == 10 || slot == 11;
    case F_HTTP: return 1;
    case F_FRIENDS: return slot == 28 || slot == 29 || slot == 30 || slot == 32 || slot == 33 || slot == 43 || slot == 44 || slot == 49;
    case F_USER: return slot == 3 || slot == 4 || (slot >= 13 && slot <= 16) || slot == 19 || slot == 20 || slot == 21;
    }
    return 0;
}

static int code_ptr(const void *fn, const void *vt)
{
    HMODULE m1 = NULL, m2 = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    if (!fn || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m1) ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt, &m2) || m1 != m2)
        return 0;
    if (!VirtualQuery(fn, &mbi, sizeof mbi))
        return 0;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static int want_slot(int fam, int i)
{
    if (fam != F_CLIENT)
        return 1;
    return i == 5 || i == 8 || i == 9 || i == 10 || i == 11 || i == 15 || i == 16 || i == 17 || i == 18 || (i >= 23 && i <= 26);
}

static void wrap_vtable(void **vt, int fam, int first, int skip12_13)
{
    const char **names = NULL;
    int count = 0;
    for (unsigned i = 0; i < sizeof famtab / sizeof famtab[0]; i++)
        if (famtab[i].fam == fam)
            names = famtab[i].names, count = famtab[i].count;
    for (int i = 0; i < n_vt; i++)
        if (hooked_vt[i] == vt)
            return;
    if (n_vt < 64)
        hooked_vt[n_vt++] = vt;
    int n = 0;
    for (int i = first; i < count; i++) {
        if ((skip12_13 && (i == 12 || i == 13)) || !want_slot(fam, i))
            continue;
        if (n_hk >= MAXHK || !code_ptr(vt[i], vt))
            continue;
        int id = n_hk++;
        hk[id].orig = vt[i], hk[id].fam = (unsigned char)fam, hk[id].slot = (unsigned char)i;
        hk[id].name = names && names[i] ? names[i] : "slot";
        hk[id].always = always_slot(fam, i);
        unsigned char *t = thunks + id * 16;
        t[0] = 0x68, *(unsigned *)(t + 1) = id;                           /* push id */
        t[5] = 0xE9, *(int *)(t + 6) = (int)((unsigned char *)net_common - (t + 10)); /* jmp net_common */
        DWORD old;
        if (VirtualProtect(&vt[i], sizeof(void *), PAGE_READWRITE, &old)) {
            vt[i] = t;
            VirtualProtect(&vt[i], sizeof(void *), old, &old);
            n++;
        }
    }
    char b[80];
    txt("steam wrapped %d slots of %s vtable %s (generic logging thunks)", n, fam_name[fam], where(vt, b, sizeof b));
}

static void net_iface(void *p, const char *ver, int fam)
{
    if (!enabled || !p)
        return;
    if (!fam) {
        if (!strncmp(ver, "SteamUser0", 10)) fam = F_USER;
        else if (!strncmp(ver, "SteamFriends", 12)) fam = F_FRIENDS;
        else if (!strncmp(ver, "SteamUtils", 10)) fam = F_UTILS;
        else if (!strncmp(ver, "SteamMatchMakingServers", 23)) fam = F_MMSERVERS;
        else if (!strncmp(ver, "SteamMatchMaking", 16)) fam = F_MM;
        else if (!strncmp(ver, "SteamNetworking", 15)) fam = F_NET;
        else if (!strncmp(ver, "STEAMHTTP_", 10)) fam = F_HTTP;
        else if (!strncmp(ver, "STEAMAPPS_", 10)) fam = F_APPS;
    }
    if (!fam || fam == F_MMSERVERS || fam == F_APPS) {   /* matchmaking servers / apps: version logged only (server browser, DLC checks) */
        txt("steam interface %s %p (not wrapped)", ver, p);
        return;
    }
    txt("steam interface %s -> %p (%s)", ver, p, fam_name[fam]);
    if (fam == F_NET)
        p2p_channel = strcmp(ver + strlen(ver) - 3, "006") >= 0;
    wrap_vtable(*(void ***)p, fam, 0, 0);
}

void net_on_iface(void *p, const char *ver)   /* called by steam.c for GetISteamGenericInterface */
{
    net_iface(p, ver, 0);
}

void net_on_client(void *client)              /* called by steam.c with the ISteamClient object */
{
    if (!enabled || !client)
        return;
    wrap_vtable(*(void ***)client, F_CLIENT, 5, 1);
}

void net_on_callback(int id, void *obj, const void *pv, int size, int fail, unsigned long long call)
{
    if (!enabled || id == 304 || id == 1101)
        return;
    char hx[140] = "";
    int n = size > 0 && size < 4096 ? size : 0;
    if (n > 0)
        hexs((const unsigned char *)pv, n, 64, hx, sizeof hx);
    txt("steam CALLBACK %d size %d%s data %s", id, size, call ? " (call result)" : "", hx);
    (void)obj, (void)fail;
    if (n > 0)
        bin(K_CALLBACK, id, (unsigned)(uintptr_t)obj, (unsigned)call, (unsigned)(call >> 32), pv, n);
}

int net_enabled(void) { return enabled; }

void net_mark(const char *text)
{
    if (!enabled)
        return;
    txt("MARK %s", text);
    bin(K_MARK, 0, 0, 0, 0, text, (unsigned)strlen(text));
}

void net_stats(char *out, int size)
{
    int o = snprintf(out, size, "net: %s, %d wrapped Steam slots, select %ld, would-block polls %ld, GetIfEntry %ld\n", enabled ? "on" : "off", n_hk, n_select,
                     n_wouldblock, n_ifentry);
    for (int i = 0; i < n_hk && o + 90 < size; i++)
        if (hk[i].calls)
            o += snprintf(out + o, size - o, "%s::%s %u calls\n", fam_name[hk[i].fam], hk[i].name, hk[i].calls);
}

/* ------------------------------------------------------------- install ---- */
void net_install(void)
{
    char prefix[MAX_PATH], path[MAX_PATH + 8];
    if (!r2_env("REV2_NETCAP", prefix, sizeof prefix))
        return;
    InitializeCriticalSection(&out_lock);
    QueryPerformanceFrequency(&qpc_f);
    QueryPerformanceCounter(&qpc_0);
    tick0 = GetTickCount();
    snprintf(path, sizeof path, "%s.txt", prefix);
    f_txt = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    snprintf(path, sizeof path, "%s.bin", prefix);
    f_bin = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f_txt == INVALID_HANDLE_VALUE)
        return;
    if (f_bin != INVALID_HANDLE_VALUE && GetFileSize(f_bin, NULL) == 0) {
        static const char magic[16] = "REV2NET1";
        put(f_bin, magic, 16);
    }
    tls_idx = TlsAlloc();
    thunks = (unsigned char *)VirtualAlloc(NULL, MAXHK * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!thunks || tls_idx == TLS_OUT_OF_INDEXES)
        return;
    enabled = 1;
    txt("netcap start pid %lu prefix %s", GetCurrentProcessId(), prefix);
#define PATCH(dll, name) real_##name = (void *)r2_patch_iat(dll, #name, (void *)hook_##name)
    PATCH("WS2_32.dll", WSAStartup);
    PATCH("WS2_32.dll", socket);
    PATCH("WS2_32.dll", bind);
    PATCH("WS2_32.dll", listen);
    PATCH("WS2_32.dll", accept);
    PATCH("WS2_32.dll", connect);
    PATCH("WS2_32.dll", send);
    PATCH("WS2_32.dll", recv);
    PATCH("WS2_32.dll", sendto);
    PATCH("WS2_32.dll", recvfrom);
    PATCH("WS2_32.dll", closesocket);
    PATCH("WS2_32.dll", setsockopt);
    PATCH("WS2_32.dll", ioctlsocket);
    PATCH("WS2_32.dll", getsockname);
    PATCH("WS2_32.dll", select);
    PATCH("IPHLPAPI.DLL", GetAdaptersAddresses);
    PATCH("IPHLPAPI.DLL", GetIfEntry);
}
