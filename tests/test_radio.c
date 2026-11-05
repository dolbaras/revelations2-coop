/* test_radio.c: offline model of the end-of-half trigger (native/radio_logic.h, stage 6m). A small "world" of two humans walks a frame loop at 10 Hz (the cadence of radio_tick); the host's radio_step decides,
 * a "flag" byte plays the story's scenario flag (set by the trigger, cleared by a retry), a mirror copies it to the client like the flag sync does. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../native/radio_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct {
    radio_state_t st;
    radio_human_t h[2];
    int flag_host, flag_client;      /* the scenario flag on each PC */
    int stage;
    unsigned now, epoch;
    int enabled, is_host;
    int set_calls;
} world_t;

static void w_init(world_t *w)
{
    memset(w, 0, sizeof *w);
    radio_init(&w->st);
    w->stage = 102, w->enabled = 1, w->is_host = 1, w->now = 1000, w->epoch = 1;
    for (int i = 0; i < 2; i++) w->h[i].present = 1, w->h[i].alive = 1, w->h[i].ai = 0, w->h[i].y = 3060.1f;
}

/* one 100 ms frame; returns the fired flag or -1; a fired flag is set on the host and mirrored to the client */
static int w_frame(world_t *w)
{
    radio_in_t in;
    memset(&in, 0, sizeof in);
    in.enabled = w->enabled, in.is_host = w->is_host, in.stage = w->stage;
    in.flag_value = w->flag_host;
    in.epoch = w->epoch, in.now = w->now, in.h = w->h, in.n = 2;
    int f = radio_step(&w->st, &in);
    if (f >= 0) {
        w->flag_host = 1, w->flag_client = 1;   /* the setter + the flag sync */
        w->set_calls++;
    }
    w->now += 100;
    return f;
}

static int w_run(world_t *w, int frames)
{
    int fired = 0;
    for (int i = 0; i < frames; i++) fired += w_frame(w) >= 0;
    return fired;
}

static void put(radio_human_t *h, float x, float y, float z) { h->x = x, h->y = y, h->z = z; }

int main(void)
{
    const radio_zone_t *z = radio_zone_for(102);
    /* 1. table */
    CHECK(z && z->flag == 11 && z->nv == 4 && z->stage == 102);
    CHECK(!radio_zone_for(100) && !radio_zone_for(101) && !radio_zone_for(151) && !radio_zone_for(153) && !radio_zone_for(0));
    /* 1b. Barry's half (6n): stage 152, flag 32, four corners, the same height band as Claire's zone */
    const radio_zone_t *zb = radio_zone_for(152);
    CHECK(zb && zb->flag == 32 && zb->nv == 4 && zb->stage == 152 && zb->ymin == 3060.11f && zb->ymax == 3160.11f);
    CHECK(zb && radio_in_poly(zb, 26524.6f, -13035.3f, 0.0f));              /* the centre of the corridor */
    CHECK(zb && radio_inside(zb, 26524.6f, 3060.1f, -13035.3f));
    CHECK(zb && !radio_inside(zb, 26524.6f, 3060.1f, -12000.0f));           /* far north */
    CHECK(zb && !radio_inside(z, 26524.6f, 3060.1f, -13035.3f));            /* the stage 102 zone does not see Barry's corridor */
    CHECK(!radio_inside(zb, 26260.0f, 3060.1f, -14065.0f));                 /* nor Claire's positions */

    /* 2. footprint: the hardware positions of 6j2 (the zonetest put slot 0 inside, slot 1 80 units aside outside the bare polygon), the polygon's own corners and centre */
    CHECK(radio_in_poly(z, 26254.0f, -14065.0f, 0.0f));                 /* inside, bare polygon */
    CHECK(!radio_in_poly(z, 26334.0f, -14065.0f, 0.0f));                /* the second human of the hop: just outside the bare polygon */
    CHECK(radio_in_poly(z, 26334.0f, -14065.0f, RADIO_MARGIN));         /* ... inside with the margin */
    CHECK(radio_in_poly(z, 26260.0f, -14065.0f, 0.0f) && radio_inside(z, 26260.0f, 3060.1f, -14065.0f));
    CHECK(radio_in_poly(z, 26236.0f, -13803.0f, 0.0f) || radio_in_poly(z, 26236.0f, -13803.0f, 1.0f));   /* a vertex (border): within a unit */
    CHECK(radio_in_poly(z, 26270.0f, -14080.0f, 0.0f));                 /* centre-ish */
    /* the points of the other stage 102 zones are far away (SF 4 / 6 / 2 / 8): never inside */
    CHECK(!radio_inside(z, 16255.0f, 2906.0f, -14287.0f));
    CHECK(!radio_inside(z, 19968.0f, 3127.0f, -14908.0f));
    CHECK(!radio_inside(z, 21600.0f, 3057.0f, -14343.0f));
    CHECK(!radio_inside(z, 23033.0f, 3000.0f, -14094.0f));
    /* the margin is a margin: 400 units off the corridor is out, the stage start is out */
    CHECK(!radio_inside(z, 26700.0f, 3060.1f, -14065.0f));
    CHECK(!radio_inside(z, 25800.0f, 3060.1f, -14065.0f));
    CHECK(!radio_inside(z, 26260.0f, 3060.1f, -13300.0f));              /* north of the corridor: far */
    CHECK(!radio_inside(z, 26260.0f, 3060.1f, -14700.0f));              /* south: far */
    CHECK(radio_inside(z, 26260.0f, 3060.1f, -13700.0f));               /* within the margin of the northern end */
    /* height: the floor level in, a floor a storey above or below out */
    CHECK(radio_inside(z, 26260.0f, 3060.1f, -14065.0f));
    CHECK(radio_inside(z, 26260.0f, 3140.0f, -14065.0f));
    CHECK(!radio_inside(z, 26260.0f, 2900.0f, -14065.0f));
    CHECK(!radio_inside(z, 26260.0f, 3300.0f, -14065.0f));
    CHECK(!radio_inside(z, 26260.0f, 1600.0f, -14065.0f));              /* the stage 100 level */
    /* an off-by-ordering check: the polygon is convex and the even-odd rule agrees with a reference on a grid */
    {
        int disagree = 0, in_n = 0;
        for (float x = 26000.0f; x <= 26500.0f; x += 25.0f)
            for (float zz = -14500.0f; zz <= -13600.0f; zz += 25.0f) {
                int a = radio_in_poly(z, x, zz, 0.0f), b = 1;
                for (int i = 0, j = 3; i < 4; j = i++) {   /* convex test: the point is on the same side of every edge */
                    float cr = (z->vx[i] - z->vx[j]) * (zz - z->vz[j]) - (z->vz[i] - z->vz[j]) * (x - z->vx[j]);
                    static float sign;
                    if (i == 0) sign = cr >= 0 ? 1.0f : -1.0f;
                    if (cr * sign < 0.0f) b = 0;
                }
                if (a != b) disagree++;
                in_n += a;
            }
        CHECK(disagree <= 4 && in_n > 20 && in_n < 120);   /* border points may differ by rounding, the area is that of a ~100 x 490 corridor */
    }

    /* 3. the normal case: both humans walk in, dwell, fire once on the host, mirrored to the client */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 25000, 3060.1f, -14100), put(&w.h[1], 24900, 3060.1f, -14100);
        CHECK(w_run(&w, 20) == 0);                                       /* both far away */
        put(&w.h[0], 26260, 3060.1f, -14065);
        CHECK(w_run(&w, 20) == 0);                                       /* only one inside */
        put(&w.h[1], 26320, 3060.1f, -14065);                            /* the partner arrives (inside with the margin) */
        int fired = 0, at = -1;
        for (int i = 0; i < 30; i++) {
            if (w_frame(&w) >= 0) fired++, at = i;
        }
        CHECK(fired == 1 && at >= 6 && at <= 8);                         /* about 0.7 s of dwell: first frame arms, 7th frame after it fires */
        CHECK(w.flag_host == 1 && w.flag_client == 1 && w.set_calls == 1);
        CHECK(w_run(&w, 200) == 0);                                      /* once only, however long they stand there */
        CHECK(w.st.fired == 1);
    }

    /* 4. one human leaves during the dwell: the dwell starts again */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 26280, 3060.1f, -14090);
        w_run(&w, 4);                                                    /* 0.4 s inside */
        put(&w.h[1], 26280, 3060.1f, -15500);                            /* the partner steps out */
        CHECK(w_run(&w, 3) == 0 && !w.st.inside);
        put(&w.h[1], 26280, 3060.1f, -14090);
        CHECK(w_run(&w, 5) == 0);                                        /* 0.5 s: not yet (the dwell restarted) */
        CHECK(w_run(&w, 5) == 1);
    }

    /* 5. who counts */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 20000, 3060.1f, -14065);
        w.h[1].ai = 1;                                                   /* the stock AI partner far away: not a human of the session */
        CHECK(w_run(&w, 30) == 0);                                       /* one human only: nothing fires (RADIO_MIN_HUMANS) */
        w.h[1].ai = 0, w.h[1].alive = 0;                                 /* the partner is dead: the game-over coupling owns that case */
        CHECK(w_run(&w, 30) == 0);
        w.h[1].alive = 1, w.h[1].present = 0;                            /* the partner is not known */
        CHECK(w_run(&w, 30) == 0);
        w.h[1].present = 1;
        put(&w.h[1], 26300, 3060.1f, -14080);
        CHECK(w_run(&w, 15) == 1);
    }
    /* a dead human far away does not block ... but then fewer than two humans live, so nothing fires either */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 0, 0, 0);
        w.h[1].alive = 0;
        CHECK(w_run(&w, 40) == 0);
    }
    /* a human on another level (the stage 100 floor) holds the trigger */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 26260, 1600.1f, -14065);
        CHECK(w_run(&w, 40) == 0);
    }

    /* 6. never on the client, never off, never in another stage, never without the session switches */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 26300, 3060.1f, -14080);
        w.is_host = 0;
        CHECK(w_run(&w, 50) == 0 && w.flag_host == 0);
        w.is_host = 1, w.enabled = 0;                                    /* switch off / world sync off / flag sync off / no session (single player, Raid) */
        CHECK(w_run(&w, 50) == 0 && w.flag_host == 0);
        w.enabled = 1, w.stage = 101;
        CHECK(w_run(&w, 50) == 0 && w.flag_host == 0);
        w.stage = 100;
        CHECK(w_run(&w, 50) == 0);
        w.stage = 102;
        CHECK(w_run(&w, 15) == 1);                                       /* the same positions fire once everything is on */
    }

    /* 7. the zone fired by itself (a press) or the flag is already set: the trigger does not fire */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 26300, 3060.1f, -14080);
        w.flag_host = 1;
        CHECK(w_run(&w, 50) == 0 && w.set_calls == 0 && w.st.latched_stage == 102);
    }

    /* 8. retry before the radio: the flag is clear again 4 s after the latch and the humans stand in the zone again: it fires again; a retry that puts them elsewhere does not */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 26300, 3060.1f, -14080);
        CHECK(w_run(&w, 20) == 1);
        w.flag_host = w.flag_client = 0;                                 /* the stage was reloaded (retry): flags back to the checkpoint's */
        w.epoch++;
        put(&w.h[0], 25000, 3060.1f, -14100), put(&w.h[1], 24900, 3060.1f, -14100);   /* back at the checkpoint, far from the radio */
        CHECK(w_run(&w, 80) == 0);
        CHECK(w.st.rearmed == 1);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 26300, 3060.1f, -14080);
        CHECK(w_run(&w, 20) == 1 && w.st.fired == 2);
    }
    /* the set did not take (flag still clear) 4 s later: tried again, but not every frame */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 26300, 3060.1f, -14080);
        int fired = 0;
        for (int i = 0; i < 300; i++) {
            if (w_frame(&w) >= 0) fired++;
            w.flag_host = 0;                                             /* the setter never takes */
        }
        CHECK(fired >= 3 && fired <= 8);                                 /* 30 s: every ~5 s, not 300 times */
    }

    /* 9. after the order: the flag stays set through the next loads (barrier / cutscene stage): no second fire; the next stage arms nothing */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 26300, 3060.1f, -14080);
        CHECK(w_run(&w, 20) == 1);
        w.epoch++;                                                       /* the cutscene load */
        CHECK(w_run(&w, 100) == 0);
        w.epoch++;
        w.stage = 103;                                                   /* the next stage */
        CHECK(w_run(&w, 100) == 0 && w.set_calls == 1);
        w.stage = 102;                                                   /* a replay of the half in the same session arms it again */
        w.flag_host = w.flag_client = 0;
        CHECK(w_run(&w, 20) == 1 && w.set_calls == 2);
    }

    /* 10. a load in the middle of the dwell restarts it */
    {
        world_t w;
        w_init(&w);
        put(&w.h[0], 26260, 3060.1f, -14065), put(&w.h[1], 26300, 3060.1f, -14080);
        w_run(&w, 5);
        w.epoch++;
        CHECK(w_run(&w, 6) == 0);
        CHECK(w_run(&w, 4) == 1);
    }

    /* 11. the host's own view is all that counts: the client's PC never decides; two worlds side by side, only the host fires and the client's flag follows */
    {
        world_t host, client;
        w_init(&host), w_init(&client);
        client.is_host = 0;
        put(&host.h[0], 26260, 3060.1f, -14065), put(&host.h[1], 26300, 3060.1f, -14080);
        put(&client.h[0], 26260, 3060.1f, -14065), put(&client.h[1], 26300, 3060.1f, -14080);
        for (int i = 0; i < 30; i++) {
            w_frame(&host), w_frame(&client);
            if (host.flag_client) client.flag_host = 1;                  /* the flag sync event reached the client */
        }
        CHECK(host.set_calls == 1 && client.set_calls == 0 && client.flag_host == 1);
    }

    /* 12. Barry's half (6n): two humans in the stage 152 corridor fire SF 32 (not 11) and latch stage 152; the same two in Claire's corridor fire nothing on 152 */
    {
        world_t w;
        w_init(&w);
        w.stage = 152;
        put(&w.h[0], 26524.6f, 3060.1f, -13035.3f), put(&w.h[1], 26500.0f, 3060.1f, -13000.0f);
        int got = -1;
        for (int i = 0; i < 20; i++) { int f = w_frame(&w); if (f >= 0) got = f; }
        CHECK(got == 32 && w.st.latched_stage == 152 && w.set_calls == 1);
        world_t v;
        w_init(&v);
        v.stage = 152;
        put(&v.h[0], 26260.0f, 3060.1f, -14065.0f), put(&v.h[1], 26300.0f, 3060.1f, -14080.0f);
        CHECK(w_run(&v, 30) == 0 && v.set_calls == 0);
    }

    printf("test_radio: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
