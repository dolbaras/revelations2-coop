/* puppet.inc: record the state of one character at 20 Hz, and replay it into another character ("network puppet" proof).
 * Included at the end of coop3.c (runs on the game thread from the per-pass hook).
 *
 * State per sample (what a network packet has to carry at minimum, see notes\stage3.md for the game's own Raid packet):
 *   t, position (+0x40 x,y,z), orientation (quaternion at +0x50 x,y,z,w), action id (+0x1A54), HP (+0x1A08), aim angle (+0x7950)
 *
 *   rec <slot> <seconds> <file>          sample the character of that slot at 20 Hz, write the file when the time is over (or stoprec)
 *   play <slot> <opts snap|anim,loop,rel or -> <file>
 *                                        replay a file into the character of <slot> (should be slot type 2): snap = position/orientation only,
 *                                        anim = also the action id through the player's ChangeAction (vf+0xB8); rel = relative to the first sample
 *   stopplay | pupinfo | pset <slot> x y z yawdeg
 */
typedef struct { float t, x, y, z, qx, qy, qz, qw, aim; int act, hp; } ps_t;

static ps_t *rec_buf;
static int rec_n, rec_cap, rec_slot = -1;
static double rec_t0, rec_len, rec_next;
static char rec_file[260];

static ps_t *pl_buf;
static int pl_raw, pl_n, pl_slot = -1, pl_snap_anim, pl_loop, pl_rel, pl_last_act = -1;
static double pl_t0;
static float pl_off[3], pl_start_xyz[3];
static unsigned pl_frames;
static float pl_maxerr, pl_sumerr, pl_last[3];
static int pl_have_last;

static double now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static int rec_save(void)
{
    FILE *fp = fopen(rec_file, "w");
    if (!fp)
        return 0;
    fprintf(fp, "# t x y z qx qy qz qw aim act hp (20 Hz)\n");
    for (int i = 0; i < rec_n; i++)
        fprintf(fp, "%.4f %.3f %.3f %.3f %.6f %.6f %.6f %.6f %.5f %d %d\n", rec_buf[i].t, rec_buf[i].x, rec_buf[i].y, rec_buf[i].z, rec_buf[i].qx, rec_buf[i].qy,
                rec_buf[i].qz, rec_buf[i].qw, rec_buf[i].aim, rec_buf[i].act, rec_buf[i].hp);
    fclose(fp);
    return 1;
}

static void sample_player(unsigned p, ps_t *s)
{
    s->x = rdf(p + 0x40), s->y = rdf(p + 0x44), s->z = rdf(p + 0x48);
    s->qx = rdf(p + 0x50), s->qy = rdf(p + 0x54), s->qz = rdf(p + 0x58), s->qw = rdf(p + 0x5C);
    s->aim = rdf(p + 0x7950);
    s->act = (int)rd32(p + 0x1A54);
    s->hp = (int)rd32(p + 0x1A08);
}

static void puppet_frame(void)
{
    if (rec_slot >= 0) {
        unsigned p = player_obj(rec_slot);
        double t = now_s() - rec_t0;
        if (!p || t >= rec_len) {
            int ok = rec_save();
            r2_log("coop3: recording of slot %d finished: %d samples -> %s (%s)", rec_slot, rec_n, rec_file, ok ? "saved" : "SAVE FAILED");
            rec_slot = -1;
        } else if (t >= rec_next) {
            rec_next += 0.05;
            if (rec_n < rec_cap) {
                ps_t *s = &rec_buf[rec_n++];
                s->t = (float)t;
                sample_player(p, s);
            }
        }
    }
    if (pl_slot >= 0 && pl_n > 1) {
        unsigned p = player_obj(pl_slot);
        if (!p)
            return;
        if (pl_have_last) {   /* how far did the game's own update move the body away from what we wrote last pass? */
            float dx = rdf(p + 0x40) - pl_last[0], dz = rdf(p + 0x48) - pl_last[2];
            float d = sqrtf(dx * dx + dz * dz);
            pl_sumerr += d;
            if (d > pl_maxerr)
                pl_maxerr = d;
        }
        double t = now_s() - pl_t0, dur = pl_buf[pl_n - 1].t;
        if (t > dur) {
            if (pl_loop)
                pl_t0 += dur, t -= dur;
            else {
                r2_log("coop3: replay finished after %u frames, drift between passes: max %.3f mean %.3f (world units)", pl_frames, pl_maxerr, pl_frames ? pl_sumerr / pl_frames : 0.0f);
                pl_slot = -1;
                return;
            }
        }
        int i = 0;
        while (i + 2 < pl_n && pl_buf[i + 1].t < t)
            i++;
        ps_t *a = &pl_buf[i], *b = &pl_buf[i + 1];
        float u = b->t > a->t ? (float)((t - a->t) / (b->t - a->t)) : 0;
        if (u < 0) u = 0;
        if (u > 1) u = 1;
        float x = a->x + (b->x - a->x) * u, y = a->y + (b->y - a->y) * u, z = a->z + (b->z - a->z) * u;
        float bq[4] = {b->qx, b->qy, b->qz, b->qw}, aq[4] = {a->qx, a->qy, a->qz, a->qw}, q[4];
        float dot = aq[0] * bq[0] + aq[1] * bq[1] + aq[2] * bq[2] + aq[3] * bq[3];
        for (int k = 0; k < 4; k++)   /* nlerp along the short arc */
            q[k] = aq[k] + ((dot < 0 ? -bq[k] : bq[k]) - aq[k]) * u;
        float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (n > 1e-6f)
            for (int k = 0; k < 4; k++)
                q[k] /= n;
        if (pl_rel)
            x += pl_off[0], y += pl_off[1], z += pl_off[2];
        if (pl_raw) {   /* raw: write the transform directly (collision pushes the body around in tight rooms) */
            wr(p + 0x40, &x, 4), wr(p + 0x44, &y, 4), wr(p + 0x48, &z, 4);
            wr(p + 0x50, q, 16);
        } else {   /* the game's own remote-player teleport (what the Raid net code does for a far target): target fields + FUN_00710fd0 */
            float yaw = 2.0f * atan2f(q[1], q[3]);
            wr(p + 0x39A0, &x, 4), wr(p + 0x39A4, &y, 4), wr(p + 0x39A8, &z, 4), wr(p + 0x39C4, &yaw, 4);
            unsigned a1[1] = {p};
            do_call(3, 0x710FD0, a1, 1);
        }
        wr(p + 0x39A0, &x, 4), wr(p + 0x39A4, &y, 4), wr(p + 0x39A8, &z, 4);
        if (pl_snap_anim && a->act != pl_last_act) {
            pl_last_act = a->act;
            unsigned vt = rd32(p), fn = rd32(vt + 0xB8);
            if (fn > 0x400000 && fn < 0x1400000) {
                unsigned args[6] = {p, (unsigned)a->act, 1, 0, 0xFFFFFFFFu, 0};
                do_call(2, fn, args, 6);
            }
        }
        pl_frames++;
        pl_last[0] = x, pl_last[1] = y, pl_last[2] = z, pl_have_last = 1;
    }
}

static int load_stream(const char *file)
{
    FILE *fp = fopen(file, "r");
    if (!fp)
        return 0;
    free(pl_buf);
    pl_buf = (ps_t *)calloc(20000, sizeof(ps_t));
    pl_n = 0;
    char line[256];
    while (fgets(line, sizeof line, fp) && pl_n < 20000) {
        if (line[0] == '#')
            continue;
        ps_t *s = &pl_buf[pl_n];
        if (sscanf(line, "%f %f %f %f %f %f %f %f %f %d %d", &s->t, &s->x, &s->y, &s->z, &s->qx, &s->qy, &s->qz, &s->qw, &s->aim, &s->act, &s->hp) == 11)
            pl_n++;
    }
    fclose(fp);
    return pl_n;
}

void puppet_cmd(const char *name, const char *arg, char *out, int size)
{
    if (!strcmp(name, "rec")) {
        int slot = 0;
        float secs = 5;
        char f[260] = "";
        if (sscanf(arg, "%d %f %259[^\r\n]", &slot, &secs, f) != 3) {
            snprintf(out, size, "usage: rec <slot> <seconds> <file>");
            return;
        }
        if (!rec_buf)
            rec_buf = (ps_t *)calloc(20000, sizeof(ps_t)), rec_cap = 20000;
        rec_n = 0, rec_slot = slot, rec_len = secs, rec_t0 = now_s(), rec_next = 0;
        snprintf(rec_file, sizeof rec_file, "%s", f);
        snprintf(out, size, "recording slot %d for %.1f s -> %s", slot, secs, f);
    } else if (!strcmp(name, "stoprec")) {
        if (rec_slot >= 0) {
            rec_len = 0;
            snprintf(out, size, "stopping (saved at the next pass)");
        } else
            snprintf(out, size, "not recording");
    } else if (!strcmp(name, "play")) {
        int slot = 0;
        char rest[100] = "";
        const char *f = NULL;
        int used = 0;
        if (sscanf(arg, "%d %99s%n", &slot, rest, &used) < 2 || used <= 0) {
            snprintf(out, size, "usage: play <slot> <opts: snap|anim,loop,rel (comma separated, or -)> <file (rest of the line, spaces allowed)>");
            return;
        }
        f = arg + used;
        while (*f == ' ')
            f++;
        if (!load_stream(f)) {
            snprintf(out, size, "cannot read samples from %s", f);
            return;
        }
        unsigned p = player_obj(slot);
        if (!p) {
            snprintf(out, size, "no player in slot %d", slot);
            return;
        }
        pl_snap_anim = strstr(rest, "anim") != NULL;
        pl_loop = strstr(rest, "loop") != NULL;
        pl_rel = strstr(rest, "rel") != NULL;
        pl_raw = strstr(rest, "raw") != NULL;
        pl_last_act = -1, pl_frames = 0, pl_maxerr = 0, pl_sumerr = 0, pl_have_last = 0;
        if (pl_rel) {
            pl_off[0] = rdf(p + 0x40) - pl_buf[0].x, pl_off[1] = rdf(p + 0x44) - pl_buf[0].y, pl_off[2] = rdf(p + 0x48) - pl_buf[0].z;
        }
        pl_t0 = now_s();
        pl_slot = slot;
        snprintf(out, size, "replaying %d samples (%.1f s) into slot %d: %s%s%s", pl_n, pl_buf[pl_n - 1].t, slot, pl_snap_anim ? "anim " : "snap ", pl_loop ? "loop " : "", pl_rel ? "rel" : "abs");
    } else if (!strcmp(name, "stopplay")) {
        pl_slot = -1;
        snprintf(out, size, "replay stopped");
    } else if (!strcmp(name, "pupinfo")) {
        snprintf(out, size, "rec slot %d n=%d ; play slot %d n=%d frames=%u drift max %.3f mean %.3f", rec_slot, rec_n, pl_slot, pl_n, pl_frames, pl_maxerr, pl_frames ? pl_sumerr / pl_frames : 0.0f);
    } else if (!strcmp(name, "pset")) {
        int slot = 0;
        float x = 0, y = 0, z = 0, yaw = 0;
        if (sscanf(arg, "%d %f %f %f %f", &slot, &x, &y, &z, &yaw) < 4) {
            snprintf(out, size, "usage: pset <slot> x y z [yawdeg]");
            return;
        }
        unsigned p = player_obj(slot);
        if (!p) {
            snprintf(out, size, "no player in slot %d", slot);
            return;
        }
        float q[4] = {0, sinf(yaw * 3.14159265f / 360.0f), 0, cosf(yaw * 3.14159265f / 360.0f)};
        wr(p + 0x40, &x, 4), wr(p + 0x44, &y, 4), wr(p + 0x48, &z, 4);
        wr(p + 0x50, q, 16);
        snprintf(out, size, "slot %d placed at %.1f %.1f %.1f yaw %.0f", slot, x, y, z, yaw);
    }
}
