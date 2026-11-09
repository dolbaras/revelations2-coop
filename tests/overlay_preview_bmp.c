/*
 * overlay_preview_bmp.c - renders the lobby screens of every language into BMP files WITHOUT the game and without a Direct3D device (GDI only), to look at layout, fonts and colours.
 *   sh tests/build_preview_bmp.sh  ->  tests/build/overlay_preview_bmp.exe <out dir> [width height [backdrop.bmp [lobby-stock-12lang.tsv [lang code [arc folder]]]]]
 * backdrop.bmp (24 / 32-bit BMP) stands for the game frame behind the lobby (a stock menu screenshot); menu-strings.tsv is work\lobby-stock-12lang.tsv (the game's own words in 12 languages:
 * title, Back, Leave, Find a Partner ...), without it the English words are used everywhere. lang code: en fr it de es ru pl pt ja ko zhs zht (one language only; all = every language); arc folder = nativePCNext\\arc\\DX9NEXT of the game (its own fonts, key caps and rust frame; without it the imitation is drawn).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/overlay.h"

void r2_log(const char *fmt, ...) { (void)fmt; }

static const char *const codes[12] = {"en", "fr", "it", "de", "es", "ru", "pl", "pt", "ja", "ko", "zhs", "zht"};

/* the game's own words: work\lobby-stock-12lang.tsv (name, table, id, 12 languages), extracted from the message files of the test copy with the same (table, id) as lobby_stock_ids */
static struct { int table, id; char w[12][200]; } g_words[64];
static int g_nwords;
static void load_tsv(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static char line[4096];
    fgets(line, sizeof line, f);   /* header */
    while (fgets(line, sizeof line, f) && g_nwords < 64) {
        char *cols[16];
        int n = 0;
        for (char *p = line; n < 16;) {
            cols[n++] = p;
            char *t = strchr(p, '\t');
            if (!t) break;
            *t = 0, p = t + 1;
        }
        if (n < 15) continue;
        size_t l = strlen(cols[n - 1]);
        while (l && (cols[n - 1][l - 1] == '\n' || cols[n - 1][l - 1] == '\r')) cols[n - 1][--l] = 0;
        g_words[g_nwords].table = atoi(cols[1]), g_words[g_nwords].id = atoi(cols[2]);
        for (int i = 0; i < 12; i++) {
            char *w = cols[3 + i];
            if (!strncmp(w, "<BTN ", 5) || !strncmp(w, "<ICON ", 6)) {   /* a leading button tag (menu.c strips it) */
                char *e = strchr(w, '>');
                if (e) w = e + 1;
                while (*w == ' ') w++;
            }
            snprintf(g_words[g_nwords].w[i], sizeof g_words[0].w[0], "%s", w);
        }
        g_nwords++;
    }
    fclose(f);
}
/* the text the lobby would show for id: the game's stock string when the table has it, else the own string */
static const char *stk(int id, int lang)
{
    for (int i = 0; i < g_nwords; i++)
        if (g_words[i].table == lobby_stock_ids[id].table && g_words[i].id == lobby_stock_ids[id].id && g_words[i].w[lang][0]) {
            const char *w = g_words[i].w[lang], *own = lobby_own_text(id, lang);
            int ascii = 1;
            for (const char *c = w; *c; c++)
                if ((unsigned char)*c >= 0x80) ascii = 0;
            if (lang >= 8 && ascii && strcmp(own, lobby_own_text(id, 0))) return own;   /* the same rule as menu.c env_text */
            return w;
        }
    return lobby_own_text(id, lang);
}

static unsigned *read_bmp(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    if (fread(&fh, sizeof fh, 1, f) != 1 || fread(&ih, sizeof ih, 1, f) != 1 || ih.biCompression != BI_RGB || (ih.biBitCount != 24 && ih.biBitCount != 32)) {
        fclose(f);
        return NULL;
    }
    int bw = ih.biWidth, bh = ih.biHeight < 0 ? -ih.biHeight : ih.biHeight, bpp = ih.biBitCount / 8, stride = (bw * bpp + 3) & ~3;
    unsigned char *row = (unsigned char *)malloc((size_t)stride);
    unsigned *px = (unsigned *)malloc((size_t)bw * (size_t)bh * 4);
    fseek(f, (long)fh.bfOffBits, SEEK_SET);
    for (int y = 0; y < bh; y++) {
        if (fread(row, 1, (size_t)stride, f) != (size_t)stride) break;
        int dy = ih.biHeight < 0 ? y : bh - 1 - y;
        for (int x = 0; x < bw; x++) px[(size_t)dy * (size_t)bw + (size_t)x] = 0xFF000000u | ((unsigned)row[x * bpp + 2] << 16) | ((unsigned)row[x * bpp + 1] << 8) | row[x * bpp];
    }
    free(row);
    fclose(f);
    *w = bw, *h = bh;
    return px;
}

static int write_bmp(const char *path, unsigned *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    BITMAPFILEHEADER fh = {0x4d42, (DWORD)(sizeof fh + sizeof(BITMAPINFOHEADER) + (size_t)w * (size_t)h * 4), 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER ih = {sizeof ih, w, -h, 1, 32, BI_RGB, (DWORD)((size_t)w * (size_t)h * 4), 0, 0, 0, 0};
    fwrite(&fh, sizeof fh, 1, f), fwrite(&ih, sizeof ih, 1, f), fwrite(px, 4, (size_t)w * (size_t)h, f);
    fclose(f);
    return 1;
}

static void row(lobby_view *v, const char *label, const char *value, int kind)
{
    lobby_row *r = &v->rows[v->nrows++];
    memset(r, 0, sizeof *r);
    snprintf(r->label, sizeof r->label, "%s", label);
    snprintf(r->value, sizeof r->value, "%s", value);
    r->kind = kind, r->action = kind == LV_NORMAL;
}

static void common(lobby_view *v, int lang, const char *title)
{
    memset(v, 0, sizeof *v);
    snprintf(v->title, sizeof v->title, "%s", title);
    snprintf(v->hint, sizeof v->hint, "%s", lobby_own_text(LT_HINT, lang));
    snprintf(v->col_name, sizeof v->col_name, "%s", lobby_own_text(LT_NAME, lang));
    snprintf(v->col_char, sizeof v->col_char, "%s", lobby_own_text(LT_CHARACTER, lang));
    snprintf(v->col_ping, sizeof v->col_ping, "%s", lobby_own_text(LT_PING, lang));
    snprintf(v->t_ready, sizeof v->t_ready, "%s", lobby_own_text(LT_PARTNER_READY, lang));
    snprintf(v->t_not_ready, sizeof v->t_not_ready, "%s", lobby_own_text(LT_NOT_READY, lang));
    snprintf(v->t_host, sizeof v->t_host, "%s", lobby_own_text(LT_HOST_TAG, lang));
    snprintf(v->leg_ok, sizeof v->leg_ok, "%s", lobby_own_text(LT_LEG_OK, lang));
    snprintf(v->leg_back, sizeof v->leg_back, "%s", stk(LT_BACK, lang));
}

static void room(lobby_view *v, int lang)
{
    char val[96];
    common(v, lang, stk(LT_COOP, lang));
    snprintf(v->subtitle, sizeof v->subtitle, "%s: 192.168.1.5:28410", lobby_own_text(LT_YOUR_ADDR, lang));
    row(v, stk(LT_CLAIRE, lang), lobby_own_text(LT_YOU, lang), LV_NORMAL);
    row(v, stk(LT_MOIRA, lang), "Boris6", LV_GREY);
    row(v, stk(LT_BARRY, lang), "", LV_NORMAL);
    row(v, stk(LT_NATALIA, lang), "", LV_GREY);
    row(v, stk(LT_EPISODE, lang), "1", LV_INFO);   /* 0.2.0: Episode 1 only */
    snprintf(val, sizeof val, "<  %s  >", stk(LT_NORMAL, lang));
    row(v, stk(LT_DIFFICULTY, lang), val, LV_NORMAL);
    row(v, stk(LT_START, lang), "", LV_GREY);
    row(v, lobby_own_text(LT_KICK, lang), "", LV_NORMAL);   /* 0.2.0: Remove partner */
    row(v, stk(LT_LEAVE, lang), "", LV_NORMAL);
    v->sel = 5;
    v->nplayers = 2;
    snprintf(v->players[0].name, sizeof v->players[0].name, "Alisa77"), snprintf(v->players[0].chr, sizeof v->players[0].chr, "%s", stk(LT_CLAIRE, lang));
    v->players[0].host = 1, v->players[0].me = 1, v->players[0].ping = -1;
    snprintf(v->players[1].name, sizeof v->players[1].name, "Boris6"), snprintf(v->players[1].chr, sizeof v->players[1].chr, "%s", stk(LT_MOIRA, lang));
    v->players[1].ready = 0, v->players[1].ping = 12;
    lobby_format(lobby_own_text(LT_HOST_WAIT_READY, lang), "Boris6", NULL, v->status, sizeof v->status);   /* 0.2.0 waiting line */
    snprintf(v->note, sizeof v->note, "%s", lobby_own_text(LT_SAVE_WARN, lang));
    {   /* 0.2.0 info block: Episode 1 only + the System Link host's port hint */
        char hint[400];
        lobby_format(lobby_own_text(LT_HOST_NET_HINT, lang), "28410", NULL, hint, sizeof hint);
        snprintf(v->info, sizeof v->info, "%s%c%s", lobby_own_text(LT_EP1_ONLY, lang), 10, hint);
    }
}

/* 0.2.0: the host's "Remove partner" question (the game's own m19 #45 in the game; the preview has its English fallback unless the word table holds it) */
static void kickask(lobby_view *v, int lang)
{
    room(v, lang);
    v->nrows = 0;
    lobby_format(stk(LT_KICK_ASK, lang), "Boris6", NULL, v->prompt, sizeof v->prompt);
    row(v, stk(LT_YES, lang), "", LV_NORMAL), row(v, stk(LT_NO, lang), "", LV_NORMAL);
    v->sel = 1;
    v->status[0] = 0, v->info[0] = 0, v->note[0] = 0;
}

static void top(lobby_view *v, int lang)
{
    common(v, lang, stk(LT_COOP, lang));
    row(v, stk(LT_HOST, lang), "", LV_NORMAL), row(v, stk(LT_JOIN, lang), "", LV_NORMAL), row(v, stk(LT_BACK, lang), "", LV_NORMAL);
    v->sel = 1;
    snprintf(v->status, sizeof v->status, "%s", stk(LT_FAIL_JOIN, lang));
    v->status_kind = LSK_ERROR;
}

static void ipin(lobby_view *v, int lang)
{
    char t[200];
    snprintf(t, sizeof t, "%s - %s", stk(LT_JOIN, lang), stk(LT_LINK, lang));
    common(v, lang, t);
    snprintf(v->prompt, sizeof v->prompt, "%s", lobby_own_text(LT_ENTER_ADDR, lang));
    v->input_on = 1;
    snprintf(v->input, sizeof v->input, "192.168.1.145:28410");
    row(v, stk(LT_JOIN, lang), "", LV_NORMAL), row(v, stk(LT_BACK, lang), "", LV_NORMAL);
    snprintf(v->hint, sizeof v->hint, "%s", lobby_own_text(LT_HINT_IP, lang));
    snprintf(v->status, sizeof v->status, "%s", lobby_own_text(LT_CONNECTING, lang));
    v->busy = 1;
}

static void friends(lobby_view *v, int lang)
{
    common(v, lang, stk(LT_FRIEND, lang));
    for (int i = 0; i < 12; i++) {
        char n[48];
        snprintf(n, sizeof n, "Steam friend number %d", i + 1);
        row(v, n, i % 3 == 0 ? "" : "(1/2)", LV_NORMAL);
    }
    row(v, stk(LT_BACK, lang), "", LV_NORMAL);
    v->sel = 4;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    int w = argc > 3 ? atoi(argv[2]) : 1920, h = argc > 3 ? atoi(argv[3]) : 1080;
    CreateDirectoryA(dir, NULL);
    if (argc > 4) {
        int bw = 0, bh = 0;
        unsigned *bpx = read_bmp(argv[4], &bw, &bh);
        if (bpx) overlay_set_preview_backdrop(bpx, bw, bh);
        else fprintf(stderr, "cannot read the backdrop %s\n", argv[4]);
    }
    if (argc > 5) load_tsv(argv[5]);
    const char *only = argc > 6 && strcmp(argv[6], "all") ? argv[6] : NULL;
    if (argc > 7) overlay_set_game_dir(argv[7]);
    static ov_scene sc;
    for (int lang = 0; lang < 12; lang++) {
        if (only && strcmp(only, codes[lang])) continue;
        for (int k = 0; k < 5; k++) {
            if (k > 0 && lang != 0 && lang != 5 && lang != 8) continue;   /* the other screens in en / ru / ja only */
            memset(&sc, 0, sizeof sc);
            sc.lang = lang, sc.tick_ms = 600;
            const char *name = k == 0 ? "room" : k == 1 ? "top" : k == 2 ? "ip" : k == 3 ? "friends" : "kickask";
            if (k == 0) room(&sc.v, lang);
            else if (k == 4) kickask(&sc.v, lang);
            else if (k == 1) top(&sc.v, lang);
            else if (k == 2) ipin(&sc.v, lang);
            else friends(&sc.v, lang);
            unsigned *px = NULL;
            if (overlay_render_to_bitmap(&sc, w, h, &px)) {
                char path[512];
                snprintf(path, sizeof path, "%s/%s_%s.bmp", dir, name, codes[lang]);
                write_bmp(path, px, w, h);
            }
        }
    }
    printf("rendered to %s\n", dir);
    return 0;
}
