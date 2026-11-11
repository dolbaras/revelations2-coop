/*
 * test_lang.c (lang, owner report 2026-11-08: "Co-Op Play" in English with Russian selected; "some co-op items are not fully translated"): every text of
 * native/lobby_text.c resolves to a text of the running language, never to the English one, unless the word is the same in that language (the exact list below:
 * names, "Steam", "Ping", the game's own words such as the German "Episode").
 *   1. every LT_* text has its own column in all 12 languages (lobby_own_text never falls back to English);
 *   2. a stock caption whose own column is Latin letters only in Japanese / Korean / Chinese is a name (menu.c env_text shows our column instead of a
 *      Latin-only stock entry of a CJK language, so that column must be a real word there);
 *   3. every English failure reason that native/coop_steam.c writes ("Steam: ...") maps to an own text (lobby_backend_reason), so none reaches the screen;
 *   4. with the game's files (GAME_DIR, or ../../game; read only): the stock caption the game itself would show in each language is not the English one, or our
 *      column replaces it (the same rule as env_text), or it is on the list.
 *   test_lang [game_dir]
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/lobby.h"

static int bad, n;
#define CHECK(c, ...) do { n++; if (!(c)) { bad++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *const L[12] = {"eng", "fre", "ita", "ger", "spa", "rus", "pol", "ptB", "jpn", "kor", "chS", "chT"};
#define ENUM_NAME(x) [x] = #x
static const char *const NAMES[LT_COUNT] = {
    ENUM_NAME(LT_COOP), ENUM_NAME(LT_HOST), ENUM_NAME(LT_JOIN), ENUM_NAME(LT_FRIEND), ENUM_NAME(LT_LINK), ENUM_NAME(LT_BACK), ENUM_NAME(LT_READY), ENUM_NAME(LT_START),
    ENUM_NAME(LT_DIFFICULTY), ENUM_NAME(LT_EPISODE), ENUM_NAME(LT_CLAIRE), ENUM_NAME(LT_MOIRA), ENUM_NAME(LT_BARRY), ENUM_NAME(LT_NATALIA), ENUM_NAME(LT_PLAYER),
    ENUM_NAME(LT_PARTNER), ENUM_NAME(LT_YES), ENUM_NAME(LT_NO), ENUM_NAME(LT_QUIT_SESSION), ENUM_NAME(LT_LEAVE), ENUM_NAME(LT_SAME_SEL), ENUM_NAME(LT_FAIL_JOIN),
    ENUM_NAME(LT_FAIL_CREATE), ENUM_NAME(LT_CONN_LOST), ENUM_NAME(LT_CASUAL), ENUM_NAME(LT_NORMAL), ENUM_NAME(LT_HARD), ENUM_NAME(LT_VERY_HARD), ENUM_NAME(LT_INVITE),
    ENUM_NAME(LT_REF_PROTO), ENUM_NAME(LT_REF_MODVER), ENUM_NAME(LT_REF_MODVER_OLD), ENUM_NAME(LT_PARTNER_SILENT), ENUM_NAME(LT_WAITING), ENUM_NAME(LT_PARTNER_JOINED),
    ENUM_NAME(LT_ENTER_ADDR), ENUM_NAME(LT_CONNECTING), ENUM_NAME(LT_NO_FRIENDS), ENUM_NAME(LT_REFRESH), ENUM_NAME(LT_NAME), ENUM_NAME(LT_PING), ENUM_NAME(LT_HINT),
    ENUM_NAME(LT_HINT_IP), ENUM_NAME(LT_YOUR_ADDR), ENUM_NAME(LT_STEAM_HINT), ENUM_NAME(LT_STEAM_NA), ENUM_NAME(LT_NEED_READY), ENUM_NAME(LT_NOT_READY),
    ENUM_NAME(LT_SEARCHING), ENUM_NAME(LT_CHARACTER), ENUM_NAME(LT_STARTING), ENUM_NAME(LT_YOU), ENUM_NAME(LT_HOST_TAG), ENUM_NAME(LT_PARTNER_READY),
    ENUM_NAME(LT_LEAVE_HINT), ENUM_NAME(LT_SAVE_WARN), ENUM_NAME(LT_WAIT_LOAD), ENUM_NAME(LT_LEG_OK), ENUM_NAME(LT_DOWNED), ENUM_NAME(LT_DOWN_SELF),
    ENUM_NAME(LT_DOWN_PARTNER), ENUM_NAME(LT_REMOVED), ENUM_NAME(LT_REF_BUILD), ENUM_NAME(LT_KICK_ASK), ENUM_NAME(LT_PARTNER_LEFT_ROOM), ENUM_NAME(LT_PARTNER_LOST_ROOM),
    ENUM_NAME(LT_SESSION_OVER), ENUM_NAME(LT_REF_FULL), ENUM_NAME(LT_REF_STARTED), ENUM_NAME(LT_REF_NOANSWER), ENUM_NAME(LT_REF_BAD), ENUM_NAME(LT_PARTNER_LEFT),
    ENUM_NAME(LT_PARTNER_LOST), ENUM_NAME(LT_HOST_ENDED), ENUM_NAME(LT_HOST_LOST), ENUM_NAME(LT_EP1_END), ENUM_NAME(LT_CONNECTING_TO), ENUM_NAME(LT_AWAITING),
    ENUM_NAME(LT_HOST_WAIT_READY), ENUM_NAME(LT_HOST_CAN_START), ENUM_NAME(LT_GUEST_PICK), ENUM_NAME(LT_GUEST_WAIT_HOST), ENUM_NAME(LT_KICK),
    ENUM_NAME(LT_HOST_NET_HINT), ENUM_NAME(LT_END_COOP), ENUM_NAME(LT_END_COOP_HELP), ENUM_NAME(LT_REF_GONE), ENUM_NAME(LT_REF_DENIED), ENUM_NAME(LT_REF_NOLOBBY),
    ENUM_NAME(LT_REF_NOTCOOP), ENUM_NAME(LT_REF_STEAM_NOANSWER), ENUM_NAME(LT_MS), ENUM_NAME(LT_CONTINUE), ENUM_NAME(LT_CONT_NEED),
    ENUM_NAME(LT_EP_STRUGGLE), ENUM_NAME(LT_EP_LITTLE), ENUM_NAME(LT_EVGENY), ENUM_NAME(LT_DARK_NATALIA),
};

/* the (text, language) pairs whose word is the English one in that language too; "all" = every language. Language codes as L[] */
static const struct { int text; const char *langs; } SAME[] = {
    {LT_CLAIRE, "all-rus"}, {LT_MOIRA, "all-rus"}, {LT_BARRY, "all-rus"}, {LT_NATALIA, "all-rus"},   /* names: Latin in the game's own tables but Russian */
    {LT_PING, "fre ita ger spa pol ptB"},                     /* "Ping" */
    {LT_NAME, "ger"},                                         /* "Name" */
    {LT_HOST_TAG, "ita ger"},                                 /* "Host" (i18n2: Polish "Gospodarz", the game's own word for the host) */
    {LT_LINK, "ita"},                                         /* the game's own Italian "System Link" */
    {LT_EPISODE, "ger"},                                      /* "Episode" (the game's own) */
    {LT_PARTNER, "ita ger pol"},                              /* "Partner" (the game's own) */
    {LT_NORMAL, "fre ger spa ptB"},                           /* "Normal" (the game's own) */
    {LT_CASUAL, "ptB"},                                       /* "Casual" (the game's own Brazilian Portuguese) */
    {LT_NO, "ita spa"},                                       /* "No" */
    {LT_MS, "fre ita ger spa pol ptB"},                       /* "%s ms": the unit symbol */
    {LT_EVGENY, "fre ita ger spa pol ptB"},                   /* the name "Evgeny" (the game's own, Latin letters in these languages; Russian and the CJK ones differ) */
    {LT_DARK_NATALIA, "fre ita"},                             /* the game's own French and Italian "Dark Natalia" */
};

static int lang_of(const char *code)
{
    for (int i = 0; i < 12; i++)
        if (!strcmp(code, L[i])) return i;
    return -1;
}

static int same_ok(int text, int lang)
{
    for (unsigned i = 0; i < sizeof SAME / sizeof SAME[0]; i++) {
        if (SAME[i].text != text) continue;
        const char *s = SAME[i].langs;
        if (!strncmp(s, "all", 3)) return !(strstr(s, "-") && strstr(s, L[lang]));
        char w[8];
        for (const char *p = s; *p;) {
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && k < 7) w[k++] = *p++;
            w[k] = 0;
            if (k && lang_of(w) == lang) return 1;
        }
    }
    return 0;
}

static int ascii_only(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

static int utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int k = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0) return 0;
        p++;
        for (int i = 0; i < k; i++, p++)
            if ((*p & 0xC0) != 0x80) return 0;
    }
    return 1;
}

static int is_name(int t) { return t == LT_CLAIRE || t == LT_MOIRA || t == LT_BARRY || t == LT_NATALIA; }

/* ---------------------------------------------------------------- 3. the English reasons coop_steam.c writes */
static char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long z = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)z + 1);
    if (b && fread(b, 1, (size_t)z, f) != (size_t)z) z = 0;
    fclose(f);
    if (b) b[z] = 0;
    if (len) *len = z;
    return b;
}

static void test_backend_reasons(void)
{
    long z = 0;
    char *src = slurp("../native/coop_steam.c", &z);
    CHECK(src != NULL, "../native/coop_steam.c not readable (run from tests/)");
    if (!src) return;
    int found = 0;
    /* every client_fail(s, "<literal>") and every snprintf(s->why, ..., "Steam: <literal>") */
    for (char *p = src; (p = strstr(p, "client_fail(s, \"")) != NULL; p++) {
        char lit[300];
        const char *q = p + strlen("client_fail(s, \"");
        int k = 0;
        while (*q && *q != '"' && k < 299) lit[k++] = *q++;
        lit[k] = 0;
        char why[340];
        snprintf(why, sizeof why, "Steam: %s", lit);
        int t = lobby_backend_reason(why);
        CHECK(t >= 0, "coop_steam.c reason without an own text: \"%s\"", lit);
        found++;
    }
    for (char *p = src; (p = strstr(p, "snprintf(s->why, sizeof s->why, \"Steam: ")) != NULL; p++) {
        char lit[300];
        const char *q = p + strlen("snprintf(s->why, sizeof s->why, \"Steam: ");
        int k = 0;
        while (*q && *q != '"' && k < 299) lit[k++] = *q++;
        lit[k] = 0;
        if (!strcmp(lit, "%s")) continue;   /* client_fail's own copy and the response text, both fed below */
        CHECK(lobby_backend_reason(lit) >= 0, "coop_steam.c reason without an own text: \"%s\"", lit);
        found++;
    }
    CHECK(found >= 8, "only %d reasons found in coop_steam.c", found);
    /* the JoinLobby responses (client_joining builds them with %d%s) */
    static const struct { const char *why; int text; } resp[] = {
        {"Steam: Steam would not let us into the lobby (response 2: it does not exist any more)", LT_REF_GONE},
        {"Steam: Steam would not let us into the lobby (response 4: full)", LT_REF_FULL},
        {"Steam: Steam would not let us into the lobby (response 3: not allowed (is it friends-only and you are not friends?))", LT_REF_DENIED},
        {"Steam: Steam would not let us into the lobby (response 5)", LT_REF_DENIED},
        {"Steam: no co-op lobby of that player was found (is the host running a Revelations 2 co-op session? friends only needs a Steam friend)", LT_REF_NOLOBBY},
        {"Steam: timed out", LT_REF_STEAM_NOANSWER},
    };
    for (unsigned i = 0; i < sizeof resp / sizeof resp[0]; i++) CHECK(lobby_backend_reason(resp[i].why) == resp[i].text, "reason \"%s\"", resp[i].why);
    CHECK(strstr(src, "(response %d%s)") != NULL, "coop_steam.c: the JoinLobby response text changed: update the test");
    CHECK(lobby_backend_reason("something new") == -1 && lobby_backend_reason("") == -1 && lobby_backend_reason(NULL) == -1, "unknown reasons map to none");
    /* and the own texts fit the status line like the other refusals (menu.c keeps 160 bytes) */
    int own[] = {LT_REF_GONE, LT_REF_DENIED, LT_REF_NOLOBBY, LT_REF_NOTCOOP, LT_REF_STEAM_NOANSWER};
    for (unsigned i = 0; i < sizeof own / sizeof own[0]; i++)
        for (int l = 0; l < 12; l++) CHECK(strlen(lobby_own_text(own[i], l)) < 160 && !strchr(lobby_own_text(own[i], l), '%'), "%s %s too long or has a %%", NAMES[own[i]], L[l]);
    free(src);
}

/* ---------------------------------------------------------------- 4. the game's own captions (GMD, the same layout as lab\gmd.py) */
static const char *const TABLE_FILE[64] = {[0] = "m01_sys_common_01", [3] = "m03_sys_title_topmenu", [6] = "m09_sys_cockpit", [14] = "m19_sys_network",
                                           [51] = "m34_sys_raidmatching", [52] = "m35_sys_raidjoin", [58] = "m40_sys_popup"};
static const char *const GAME_LANG[12] = {"eng", "fre", "ita", "ger", "spa", "rus", "pol", "ptB", "jpn", "kor", "chS", "chT"};

/* message `id` of a GMD file, markup removed like menu.c (strip_markup, the leading space), line breaks as spaces; 0 = not readable */
static int gmd_text(const char *dir, const char *table, const char *lang, int id, char *out, size_t size)
{
    char path[600];
    snprintf(path, sizeof path, "%s/nativePCNext/message/win/%s_%s.gmd", dir, table, lang);
    long z = 0;
    char *d = slurp(path, &z);
    if (!d) return 0;
    unsigned lc, sc, ls, ss, ns;
    if (z < 0x28 || memcmp(d, "GMD", 4)) { free(d); return 0; }
    memcpy(&lc, d + 0x14, 4), memcpy(&sc, d + 0x18, 4), memcpy(&ls, d + 0x1C, 4), memcpy(&ss, d + 0x20, 4), memcpy(&ns, d + 0x24, 4);
    long off = 0x28 + ns + 1 + (long)lc * 20 + 1024 + ls;
    if (off + (long)ss > z || id < 0 || (unsigned)id >= sc) { free(d); return 0; }
    const char *p = d + off;
    for (int i = 0; i < id; i++) p += strlen(p) + 1;
    size_t o = 0;
    int tag = 0;
    for (const char *q = p; *q && o + 1 < size; q++) {
        if (*q == '<') { tag = 1; continue; }
        if (tag) { if (*q == '>') tag = 0; continue; }
        char ch = (*q == '\r' || *q == '\n') ? ' ' : *q;
        if (ch == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
        out[o++] = ch;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    free(d);
    return 1;
}

static void test_game_captions(const char *dir)
{
    char probe[600];
    snprintf(probe, sizeof probe, "%s/nativePCNext/message/win/m03_sys_title_topmenu_eng.gmd", dir);
    FILE *f = fopen(probe, "rb");
    if (!f) { printf("(no game files at %s: the check of the game's own captions is skipped; set GAME_DIR)\n", dir); return; }
    fclose(f);
    int checked = 0;
    for (int t = 0; t < LT_COUNT; t++) {
        int tab = lobby_stock_ids[t].table, id = lobby_stock_ids[t].id;
        if (tab < 0 || tab >= 64 || !TABLE_FILE[tab]) continue;
        char en[400];
        if (!gmd_text(dir, TABLE_FILE[tab], "eng", id, en, sizeof en)) { CHECK(0, "%s: the English entry is not readable", NAMES[t]); continue; }
        for (int l = 1; l < 12; l++) {
            char g[400];
            CHECK(gmd_text(dir, TABLE_FILE[tab], GAME_LANG[l], id, g, sizeof g) && g[0] && strncmp(g, "Invalid Message", 15), "%s %s: the game has no entry", NAMES[t], L[l]);
            const char *shown = g;
            if (l >= 8 && ascii_only(g) && strcmp(lobby_own_text(t, l), lobby_own_text(t, 0))) shown = lobby_own_text(t, l);   /* menu.c env_text */
            checked++;
            if (!same_ok(t, l)) CHECK(strcmp(shown, en) != 0 && !(l >= 8 && ascii_only(shown)), "%s %s: the game's own caption \"%s\" is shown and is English", NAMES[t], L[l], shown);
        }
    }
    printf("game captions: %d checked in %s\n", checked, dir);
}

int main(int argc, char **argv)
{
    /* 1. own columns */
    for (int t = 0; t < LT_COUNT; t++) {
        CHECK(NAMES[t] != NULL, "text %d has no name in this test (a new LT_ id: add it to NAMES)", t);
        const char *en = lobby_own_text(t, 0);
        CHECK(en[0] && utf8_valid(en), "%s: English empty or not UTF-8", NAMES[t] ? NAMES[t] : "?");
        for (int l = 1; l < 12; l++) {
            const char *s = lobby_own_text(t, l);
            CHECK(s[0] && utf8_valid(s), "%s %s: empty or not UTF-8", NAMES[t], L[l]);
            if (!same_ok(t, l)) CHECK(strcmp(s, en) != 0, "%s %s falls back to English: \"%s\"", NAMES[t], L[l], s);
            /* 2. a Latin-only column in a CJK language is a name (else env_text could not replace a Latin-only stock entry) */
            if (l >= 8 && !is_name(t)) CHECK(!ascii_only(s), "%s %s is Latin letters only: \"%s\"", NAMES[t], L[l], s);
        }
    }
    /* the list itself: no pair is allowed that is not the same word any more (a stale entry would hide a later fallback) */
    for (unsigned i = 0; i < sizeof SAME / sizeof SAME[0]; i++)
        for (int l = 1; l < 12; l++)
            if (same_ok(SAME[i].text, l) && !is_name(SAME[i].text)) CHECK(!strcmp(lobby_own_text(SAME[i].text, l), lobby_own_text(SAME[i].text, 0)), "stale list entry %s %s", NAMES[SAME[i].text], L[l]);
    /* 3. backend reasons */
    test_backend_reasons();
    /* 4. the game's own captions */
    const char *dir = argc > 1 ? argv[1] : getenv("GAME_DIR");
    test_game_captions(dir && dir[0] ? dir : "../../game");
    printf("test_lang: %d checks, %d failed\n", n, bad);
    return bad ? 1 : 0;
}
