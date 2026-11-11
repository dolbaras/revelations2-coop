/*
 * test_resil.c: the 0.2.0 resilience texts of native/lobby_text.c in 12 languages: the refusal reasons (lobby status line after "Failed to join session.",
 * menu.c keeps 160 bytes of it) and the strip while the partner's game sends nothing (menu.c formats it into 200 bytes). Checked: only %s sequences and the
 * right number of them, the values whole and in the right order, the length with the longest values (CP_MOD_MAX / CP_NAME_MAX), valid UTF-8 also when cut.
 */
#include <stdio.h>
#include <string.h>

#include "../native/lobby.h"

static int bad, n;
static void chk(int ok, const char *what, int lang)
{
    n++;
    if (!ok) {
        bad++;
        printf("FAIL: %s (language %d %s)\n", what, lang, lang >= 0 && lang < 12 ? lobby_lang_names[lang] : "");
    }
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

/* number of %s; *other = any other % sequence */
static int count_s(const char *f, int *other)
{
    int c = 0;
    *other = 0;
    for (const char *p = f; *p; p++)
        if (*p == '%') {
            if (p[1] == 's') c++, p++;
            else *other = 1;
        }
    return c;
}

/* 0.2.0 lobby-must: the new texts in 12 languages (formats, the room / strip / info budgets) and the small helpers around them */
static void lobbymust_texts(void)
{
    const char *name = "ABCDEFGHIJKLMNOPQRSTUVWX";                 /* CP_NAME_MAX = 24 bytes */
    const char *addr = "[2001:db8:85a3::8a2e:370:7334]:28410";      /* a long address the guest may type */
    static const int own[] = {LT_REF_FULL, LT_REF_STARTED, LT_REF_NOANSWER, LT_REF_BAD, LT_PARTNER_LEFT, LT_PARTNER_LOST, LT_HOST_ENDED, LT_HOST_LOST, LT_EP1_END,
                              LT_CONNECTING_TO, LT_AWAITING, LT_HOST_WAIT_READY, LT_HOST_CAN_START, LT_GUEST_PICK, LT_GUEST_WAIT_HOST, LT_KICK,
                              LT_HOST_NET_HINT, LT_END_COOP, LT_END_COOP_HELP};
    static const int room[] = {LT_CONNECTING_TO, LT_AWAITING, LT_HOST_WAIT_READY, LT_HOST_CAN_START, LT_GUEST_PICK, LT_GUEST_WAIT_HOST};
    static const int strip[] = {LT_PARTNER_LEFT, LT_PARTNER_LOST, LT_HOST_ENDED, LT_HOST_LOST, LT_EP1_END};
    char big[1100], out[240];
    for (int lang = 0; lang < 12; lang++) {
        for (unsigned k = 0; k < sizeof own / sizeof own[0]; k++) {
            int other;
            const char *f = lobby_own_text(own[k], lang);
            chk(f[0] && utf8_valid(f) && count_s(f, &other) == lobby_text_args(own[k]) && !other, "own text: valid UTF-8, exactly the %s it is used with", lang);
            chk(lang == 0 || strcmp(f, lobby_own_text(own[k], 0)), "own text: translated", lang);
        }
        for (unsigned k = 0; k < sizeof room / sizeof room[0]; k++) {
            const char *v = room[k] == LT_CONNECTING_TO || room[k] == LT_AWAITING ? addr : name;
            lobby_format(lobby_own_text(room[k], lang), v, NULL, big, sizeof big);
            chk(strlen(big) <= 239 && (!lobby_text_args(room[k]) || strstr(big, v)), "room line: whole value, <= 239 bytes (lobby_view.status)", lang);
            if (strlen(big) > 239) printf("   %u bytes: %s\n", (unsigned)strlen(big), big);
        }
        for (unsigned k = 0; k < sizeof strip / sizeof strip[0]; k++) {
            lobby_loss_line(strip[k], name, "Player", lang, big, sizeof big);
            chk(strlen(big) <= 199 && utf8_valid(big) && (!lobby_text_args(strip[k]) || strstr(big, name)), "strip: the name whole, <= 199 bytes (menu.c line buffer)", lang);
            if (strlen(big) > 199) printf("   %u bytes: %s\n", (unsigned)strlen(big), big);
            lobby_loss_line(strip[k], "Player 2", "Spieler", lang, out, sizeof out);
            chk(!lobby_text_args(strip[k]) || strstr(out, "Spieler 2"), "strip: a default name in the player's language", lang);
        }
        {   /* the info block: the port hint alone (0.3.0) fits lobby_view.info and the overlay's 479-byte line */
            char hint[400];
            lobby_format(lobby_own_text(LT_HOST_NET_HINT, lang), "65535", NULL, hint, sizeof hint);
            chk(strstr(hint, "65535") && strlen(hint) < sizeof hint - 1 && strlen(hint) <= 479 && utf8_valid(hint), "info block: fits lobby_view.info", lang);
        }
        chk(strlen(lobby_own_text(LT_KICK, lang)) <= 60, "Remove partner: a short row label", lang);
        chk(strlen(lobby_own_text(LT_END_COOP, lang)) <= 40 && strlen(lobby_own_text(LT_END_COOP_HELP, lang)) <= 159, "pause row label / help: short", lang);
    }
    /* stock-shaped formats: the English fallbacks carry the one %s that menu.c makes of <USER> / <SWAP> */
    static const int stockf[] = {LT_KICK_ASK, LT_PARTNER_LEFT_ROOM, LT_PARTNER_LOST_ROOM};
    for (unsigned k = 0; k < 3; k++) {
        int other;
        chk(count_s(lobby_own_text(stockf[k], 0), &other) == 1 && !other && lobby_text_args(stockf[k]) == 1, "stock format fallback: one %s", 0);
    }
    /* lobby_format: a % that is not %s stays a %; at most two values */
    lobby_format("50% of %s, %d", "x", NULL, out, sizeof out);
    chk(!strcmp(out, "50% of x, %d"), "lobby_format: other % printed as is", 0);
    lobby_format("%s %s %s", "a", "b", out, sizeof out);
    chk(!strcmp(out, "a b %s"), "lobby_format: a third %s stays text", 0);
    /* the strip of a note */
    chk(lobby_loss_text(CS_N_PEER_LEAVE, 0, CS_GONE_LEFT, 1) == LT_PARTNER_LEFT, "host: partner left", 0);
    chk(lobby_loss_text(CS_N_PEER_LEAVE, 1, CS_GONE_LEFT, 1) == LT_PARTNER_LOST, "host: partner lost (no goodbye)", 0);
    chk(lobby_loss_text(CS_N_PEER_LEAVE, 1, CS_GONE_SILENT, 1) == LT_PARTNER_LOST, "host: partner given up after the silence limit", 0);
    chk(lobby_loss_text(CS_N_PEER_LEAVE, 0, CS_GONE_REMOVED, 1) == LT_PARTNER_LEFT, "host: End Co-Op of the host itself", 0);
    chk(lobby_loss_text(CS_N_PEER_LEAVE, 0, CS_GONE_EP_END, 1) == LT_EP1_END, "host: the partner reached the end of the episode", 0);
    chk(lobby_loss_text(CS_N_PEER_LEAVE, 1, CS_GONE_REPLACED, 1) == -1, "host: a lab rejoin needs no strip", 0);
    chk(lobby_loss_text(CS_N_LOST, 1, CP_LEAVE_QUIT, 0) == LT_HOST_ENDED && lobby_loss_text(CS_N_LOST, 1, CP_LEAVE_END_COOP, 0) == LT_HOST_ENDED &&
            lobby_loss_text(CS_N_LOST, 1, CP_LEAVE_REMOVED, 0) == LT_HOST_ENDED, "guest: the host quit / ended co-op / removed", 0);
    chk(lobby_loss_text(CS_N_LOST, 1, CP_LEAVE_EP_END, 0) == LT_EP1_END, "guest: the host reached the end of the episode", 0);
    chk(lobby_loss_text(CS_N_LOST, 0, 0, 0) == LT_HOST_LOST && lobby_loss_text(CS_N_LOST, 2, 0, 0) == LT_HOST_LOST, "guest: link lost / host silent", 0);
    chk(lobby_loss_text(CS_N_LOST, 1, 0, 1) == -1 && lobby_loss_text(CS_N_PEER_LEAVE, 0, 0, 0) == -1 && lobby_loss_text(CS_N_LOBBY, 0, 0, 1) == -1, "other notes: none", 0);
    /* the shown name */
    lobby_display_name("Player 1", "Игрок", out, sizeof out);
    chk(!strcmp(out, "Игрок 1"), "display name: Player 1 in Russian", 5);
    lobby_display_name("Player 2", NULL, out, sizeof out);
    chk(!strcmp(out, "Player 2"), "display name: no word given", 0);
    lobby_display_name("Boris", "Joueur", out, sizeof out);
    chk(!strcmp(out, "Boris"), "display name: a real name stays", 1);
    lobby_display_name("", "Joueur", out, sizeof out);
    chk(!strcmp(out, "?"), "display name: empty", 1);
}

int main(void)
{
    const char *vh = "0.2.0-test-1234", *vm = "0.10.10-rc-9999";   /* CP_MOD_MAX = 15 bytes each */
    const char *name = "ABCDEFGHIJKLMNOPQRSTUVWX";                 /* CP_NAME_MAX = 24 bytes */
    char out[160], big[600], line[200];
    for (int lang = 0; lang < 12; lang++) {
        int other;
        chk(count_s(lobby_own_text(LT_REF_PROTO, lang), &other) == 0 && !other, "LT_REF_PROTO: no format sequence", lang);
        chk(count_s(lobby_own_text(LT_REF_MODVER, lang), &other) == 2 && !other, "LT_REF_MODVER: exactly two %s", lang);
        chk(count_s(lobby_own_text(LT_REF_MODVER_OLD, lang), &other) == 1 && !other, "LT_REF_MODVER_OLD: exactly one %s", lang);
        chk(count_s(lobby_own_text(LT_PARTNER_SILENT, lang), &other) == 2 && !other, "LT_PARTNER_SILENT: exactly two %s", lang);

        /* another version on the host: both versions whole, the host's first, within 159 bytes uncut */
        const char *t = lobby_refusal_text(CP_REF_MODVER, vh, vm, lang, big, sizeof big);
        chk(t == big && strlen(big) <= 159, "MODVER fits 159 bytes with 15-byte versions", lang);
        if (strlen(big) > 159) printf("   %u bytes: %s\n", (unsigned)strlen(big), big);
        lobby_refusal_text(CP_REF_MODVER, vh, vm, lang, out, sizeof out);
        const char *ph = strstr(out, vh), *pm = strstr(out, vm);
        chk(ph && pm && ph < pm && utf8_valid(out), "MODVER: the host's version, then mine, valid UTF-8", lang);
        /* an older host (no version sent) */
        lobby_refusal_text(CP_REF_MODVER, "", vm, lang, big, sizeof big);
        chk(strlen(big) <= 159 && strstr(big, vm) && utf8_valid(big), "MODVER_OLD: my version, within 159 bytes", lang);
        if (strlen(big) > 159) printf("   %u bytes: %s\n", (unsigned)strlen(big), big);
        lobby_refusal_text(CP_REF_MODVER, NULL, NULL, lang, big, sizeof big);
        chk(utf8_valid(big) && big[0], "MODVER with NULL versions: a text, no crash", lang);
        /* the protocol */
        t = lobby_refusal_text(CP_REF_VERSION, vh, vm, lang, big, sizeof big);
        chk(t == big && strlen(big) <= 159 && utf8_valid(big) && !strcmp(big, lobby_own_text(LT_REF_PROTO, lang)), "VERSION: the own text, within 159 bytes", lang);
        if (strlen(big) > 159) printf("   %u bytes: %s\n", (unsigned)strlen(big), big);
        /* a backend's own reason (code 0, the Steam path) keeps the session's English text; every other code has a text in the language (0.2.0 lobby-must) */
        chk(!lobby_refusal_text(0, vh, vm, lang, out, sizeof out) && !lobby_refusal_text(99, vh, vm, lang, out, sizeof out), "code 0 / unknown: NULL", lang);
        {
            static const int codes[] = {CP_REF_FULL, CP_REF_STARTED, CP_REF_BAD, CS_REF_NOANSWER, CP_REF_BUILD, CP_REF_REMOVED};
            static const int texts[] = {LT_REF_FULL, LT_REF_STARTED, LT_REF_BAD, LT_REF_NOANSWER, LT_REF_BUILD, LT_REMOVED};
            for (int k = 0; k < 6; k++) {
                t = lobby_refusal_text(codes[k], vh, vm, lang, big, sizeof big);
                chk(t == big && !strcmp(big, lobby_own_text(texts[k], lang)) && strlen(big) <= 159 && utf8_valid(big) && !strchr(big, '%'), "refusal text: the own text, <= 159 bytes", lang);
                if (strlen(big) > 159) printf("   code %d: %u bytes: %s\n", codes[k], (unsigned)strlen(big), big);
            }
        }
        /* the strip: the longest name and 3-digit seconds within 199 bytes, the name before the seconds */
        lobby_silent_text(name, 599, lang, big, sizeof big);
        chk(strlen(big) <= 199, "silent strip fits 199 bytes", lang);
        lobby_silent_text(name, 599, lang, line, sizeof line);
        const char *pn = strstr(line, name), *ps = strstr(line, "599");
        chk(pn && ps && pn < ps && utf8_valid(line), "silent strip: the name, then the seconds, valid UTF-8", lang);
        lobby_silent_text(NULL, -5, lang, line, sizeof line);
        chk(strstr(line, "?") && strstr(line, "0") && utf8_valid(line), "silent strip without a name / negative seconds", lang);
        /* any cut stays valid UTF-8 and shorter than the buffer */
        for (size_t k = 1; k < 120; k++) {
            char small[120];
            lobby_refusal_text(CP_REF_MODVER, vh, vm, lang, small, k);
            int ok = strlen(small) < k && utf8_valid(small);
            lobby_silent_text(name, 42, lang, small, k);
            ok = ok && strlen(small) < k && utf8_valid(small);
            if (!ok) {
                chk(0, "cut text: valid UTF-8 shorter than the buffer", lang);
                break;
            }
        }
        chk(1, "cuts checked", lang);
    }
    lobbymust_texts();
    printf("%d checks, %d failed\n", n, bad);
    return bad != 0;
}
