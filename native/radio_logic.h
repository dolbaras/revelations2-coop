/* radio_logic.h: the pure part of the end-of-half trigger (stage 6m), so tests/test_radio.c can replay it without a game.
 *
 * Every half of Episode 1 ends at a radio: the story's flag zone "radio gaze" (stage 102: scenario flag 11, Barry's half: stage 152, flag 32) is an AreaHit zone of the action-button system with the boot type 6
 * and the button type 3 (the ones that wait for a press, unlike the enter-triggered zones of boot type 3) and it reacts only to the local human of the PC it runs on. On the pair that means: nobody has to
 * press a key for the story to end by walking in a two-player session, and the partner's puppet never fires it. The flag it would set starts the stock end-of-half order (the FSM `fix__Acdemo_radioINT`:
 * flag -> cutscene 1010 -> flag -> 17.5 s timer -> StageClear), which already runs on both PCs from the flag alone (shown on hardware, 6j2: `net w sf set 11 1` on both PCs).
 * The trigger: the HOST looks at both humans (its own human and the partner's puppet, whose coordinates it has exactly) and sets the flag when every living human that is not the AI partner has stood inside the
 * zone's footprint (the zone's polygon grown by a margin, the zone's own height range grown too) for a short dwell. The flag sync then mirrors it to the client like any other flag. Once per stage and session
 * (the latch is cleared by a change of stage, and when the flag reads clear again 4 s after the latch: a retry from a checkpoint before the radio, or a set that did not take).
 * The data are the zones' own (extracted from the stage archives, `mVertex` / `mBottom` / `mHeight` of the AreaHitShape). */
#ifndef RADIO_LOGIC_H
#define RADIO_LOGIC_H

#define RADIO_MAX_V 8
#define RADIO_MARGIN 150.0f        /* grown polygon (xz): the zone is a narrow corridor of about 100 units; a human's body is about 40 wide, the puppet may lag a few frames */
#define RADIO_Y_BELOW 80.0f        /* feet may sit a little below the zone's bottom plane (the floor is at 3060.1, the zone's bottom is 3060.11) */
#define RADIO_Y_ABOVE 120.0f
#define RADIO_DWELL_MS 700u        /* every human inside for this long without a break: no fire on a one-frame teleport or on somebody running through */
#define RADIO_MIN_HUMANS 2         /* a session has two humans: with fewer known (the partner is loading, dead, gone) nothing fires */
#define RADIO_REARM_MS 4000u       /* the flag still reads clear this long after the latch: a retry reloaded the stage (or the set did not take): armed again */

typedef struct {
    int stage;                      /* story area id (the stage the zone lives in) */
    int flag;                       /* scenario flag index the zone sets */
    int nv;
    float vx[RADIO_MAX_V], vz[RADIO_MAX_V];
    float ymin, ymax;               /* the zone's own vertical range */
    const char *name;
} radio_zone_t;

/* Claire's half, stage 102: zone `radio gaze`, flag 11. Barry's half, stage 152: the same radio tower in the present, flag 32, polygon (26658.5,-12884.6) (26710.0,-13090.1)
 * (26398.6,-13184.9) (26331.3,-12981.5), y 3060.11 .. 3160.11 (added in 6n; offline tests only until the Barry half is shown on hardware). */
static const radio_zone_t g_radio_zones[] = {
    {102, 11, 4, {26236.0f, 26376.0f, 26280.0f, 26146.0f}, {-13803.0f, -14291.0f, -14318.0f, -13849.0f}, 3060.11f, 3160.11f, "radio gaze (stage 102, SF 11)"},
    {152, 32, 4, {26658.5f, 26710.0f, 26398.6f, 26331.3f}, {-12884.6f, -13090.1f, -13184.9f, -12981.5f}, 3060.11f, 3160.11f, "radio (Barry's half, stage 152, SF 32)"},
};
#define RADIO_ZONES ((int)(sizeof g_radio_zones / sizeof g_radio_zones[0]))

typedef struct { int present, alive, ai; float x, y, z; } radio_human_t;   /* ai: the stock AI partner (slot type 1): not a human of the session, never counted */

static inline const radio_zone_t *radio_zone_for(int stage)
{
    for (int i = 0; i < RADIO_ZONES; i++)
        if (g_radio_zones[i].stage == stage) return &g_radio_zones[i];
    return 0;
}

/* squared distance of a point to a segment (xz) */
static inline float radio_seg_d2(float px, float pz, float ax, float az, float bx, float bz)
{
    float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz, t = 0.0f;
    if (l2 > 0.0f) {
        t = ((px - ax) * dx + (pz - az) * dz) / l2;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    float qx = ax + t * dx - px, qz = az + t * dz - pz;
    return qx * qx + qz * qz;
}

/* inside the polygon (even-odd rule) or within `margin` of its border */
static inline int radio_in_poly(const radio_zone_t *z, float x, float zz, float margin)
{
    int in = 0;
    for (int i = 0, j = z->nv - 1; i < z->nv; j = i++) {
        if (((z->vz[i] > zz) != (z->vz[j] > zz)) && (x < (z->vx[j] - z->vx[i]) * (zz - z->vz[i]) / (z->vz[j] - z->vz[i]) + z->vx[i])) in = !in;
    }
    if (in) return 1;
    float m2 = margin * margin;
    for (int i = 0, j = z->nv - 1; i < z->nv; j = i++)
        if (radio_seg_d2(x, zz, z->vx[j], z->vz[j], z->vx[i], z->vz[i]) <= m2) return 1;
    return 0;
}

static inline int radio_inside(const radio_zone_t *z, float x, float y, float zz)
{
    if (y < z->ymin - RADIO_Y_BELOW || y > z->ymax + RADIO_Y_ABOVE) return 0;
    return radio_in_poly(z, x, zz, RADIO_MARGIN);
}

typedef struct {
    int latched_stage;              /* the stage whose zone has been fired (or found fired): nothing more there; -1 none */
    int last_stage;
    unsigned last_epoch;
    int inside;                     /* all humans inside since `since` */
    unsigned since, latch_t;
    unsigned fired, checks, rearmed;
} radio_state_t;

static inline void radio_init(radio_state_t *s)
{
    s->latched_stage = s->last_stage = -1;
    s->last_epoch = 0, s->inside = 0, s->since = s->latch_t = 0, s->fired = s->checks = s->rearmed = 0;
}

typedef struct {
    int enabled;                    /* switch on, world sync on, flag sync on, session running (the caller's decision) */
    int is_host;
    int stage;                      /* story area id */
    int flag_value;                 /* the zone's flag as the game reads it now (-1: not asked) */
    unsigned epoch;                 /* changes at every level (re)load of the session */
    unsigned now;                   /* ms */
    const radio_human_t *h;
    int n;
} radio_in_t;

/* One call per frame. Returns the flag index to set (>= 0) or -1. */
static inline int radio_step(radio_state_t *s, const radio_in_t *in)
{
    const radio_zone_t *z = radio_zone_for(in->stage);
    if (in->stage != s->last_stage) {   /* another stage: the latch of the old one does not apply (a replay of the half arms it again) */
        s->last_stage = in->stage;
        s->latched_stage = -1;
        s->inside = 0;
    }
    if (in->epoch != s->last_epoch) {   /* a (re)load: the dwell starts again */
        s->last_epoch = in->epoch;
        s->inside = 0;
    }
    if (!z || !in->enabled || !in->is_host) {
        s->inside = 0;
        return -1;
    }
    if (in->flag_value > 0) {   /* the zone fired by itself (a press) or the order is under way: once only */
        if (s->latched_stage != in->stage) s->latch_t = in->now;
        s->latched_stage = in->stage;
        s->inside = 0;
        return -1;
    }
    if (s->latched_stage == in->stage) {
        if (in->flag_value == 0 && in->now - s->latch_t > RADIO_REARM_MS) s->latched_stage = -1, s->rearmed++;   /* the flag was cleared (retry) or never took: armed again */
        else return -1;
    }
    s->checks++;
    int humans = 0, inside = 0;
    for (int i = 0; i < in->n; i++) {
        const radio_human_t *p = &in->h[i];
        if (!p->present || !p->alive || p->ai) continue;
        humans++;
        if (radio_inside(z, p->x, p->y, p->z)) inside++;
    }
    if (humans < RADIO_MIN_HUMANS || inside != humans) {
        s->inside = 0;
        return -1;
    }
    if (!s->inside) {
        s->inside = 1;
        s->since = in->now;
        return -1;
    }
    if (in->now - s->since < RADIO_DWELL_MS) return -1;
    s->latched_stage = in->stage;
    s->latch_t = in->now;
    s->inside = 0;
    s->fired++;
    return z->flag;
}

#endif
