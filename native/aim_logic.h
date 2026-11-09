/* aim_logic.h: the pure part of the LAB aim aid (`net aim`, native/aim.inc; notes\aim.md), tested offline by tests/test_aim.c.
 *
 * Why (static, from the image, 2026-11-08): a gunshot of a human is computed by FUN_006EF600 (thiscall on the player, stack: weapon, float start[4] (out),
 * float dir[4] (out), char flag; `ret 0x10`). It takes the player's root matrix (vtable +0x50 with -1), turns it by the aim yaw offset player+0x6FBC (degrees,
 * the aim controller's +0x834) and asks the weapon (uWeapon_vf129) for the muzzle offset and the shot vector, whose pitch comes from player+0x798C (the
 * normalised aim pitch, aim controller +0x828/+0x838). The fire action (cPlAction_pl_act_weapon_fire_vf7 0x6D3250) calls it at 0x6D43D3 and hands start/dir to the
 * weapon's fire call (vtable +0x210) and to the Raid shot sender FUN_009F7C70. The camera is not read there: the aim state is driven by the pad (cPadEx_vf5 ->
 * FUN_00A12380 -> FUN_00A13650) and the camera follows it (uCameraCtrl+0x23C0/+0x23C4 = uCameraQFPS+0x21F0/+0x21F4 deltas, flags +0x2200). A lab tool that only
 * turned the body (teleport yaw) left pitch and offset to the pad, so a lying or crouching enemy was never hit (melee bench gun1: 11 shots, 0 hits).
 *
 * The aid: while `net aim at ...` is on, the shot vector of THIS PC's human is replaced right after FUN_006EF600 by the line from the computed start to a target point
 * (a joint of an enemy or a fixed point), with the same length (the weapon's range / speed). Everything after it is the game's: the muzzle check (a blocked muzzle
 * starts the shot at the chest joint), the weapon's fire, the bullet collision, the pending DamageInfo of the hit part (meleehit_logic.h), MHIT on a client. */
#ifndef AIM_LOGIC_H
#define AIM_LOGIC_H
#include <math.h>

#define AIM_SITE 0x6D43D3u          /* call rel32 FUN_006EF600 inside the fire action's update */
#define AIM_FN 0x6EF600u
#define AIM_JOINT_ARR 0x484u        /* model + this = the joint array (0xA0 bytes each, +0x40 = world position): FUN_00478900 */
#define AIM_JOINT_MAP 0x488u        /* model + this = the joint id -> index byte map (0xFF = none) */
#define AIM_JOINT_SIZE 0xA0u
#define AIM_JOINT_POS 0x40u

/* Replace dir (xyz, w kept) by the direction start -> target with the same length. Returns 1 when changed, 0 when the inputs are degenerate (left as is).
 * *turn_deg (may be NULL) = the angle between the old and the new vector. */
static int aim_redirect(const float start[3], float dir[4], const float target[3], float *turn_deg)
{
#ifdef AIM_MODEL_BEFORE_FIX
    /* the lab tool before `net aim`: the shot vector stays the pad's (the body was only turned by the teleport yaw) */
    (void)start, (void)target;
    if (turn_deg) *turn_deg = 0.0f;
    (void)dir;
    return 0;
#else
    float v[3] = {target[0] - start[0], target[1] - start[1], target[2] - start[2]};
    float d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(d >= 1.0f) || !(len > 1e-6f) || !isfinite(d) || !isfinite(len)) {
        if (turn_deg) *turn_deg = 0.0f;
        return 0;
    }
    if (turn_deg) {
        float c = (v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2]) / (d * len);
        if (c > 1.0f) c = 1.0f;
        if (c < -1.0f) c = -1.0f;
        *turn_deg = acosf(c) * 57.2957795f;
    }
    float k = len / d;
    dir[0] = v[0] * k, dir[1] = v[1] * k, dir[2] = v[2] * k;
    return 1;
#endif
}

/* The distance of point p from the ray start + t * dir (t >= 0); -1 when dir is zero. Used by the test (does the shot pass through the target sphere). */
static float aim_ray_miss(const float start[3], const float dir[3], const float p[3])
{
    float len2 = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (!(len2 > 1e-12f)) return -1.0f;
    float w[3] = {p[0] - start[0], p[1] - start[1], p[2] - start[2]};
    float t = (w[0] * dir[0] + w[1] * dir[1] + w[2] * dir[2]) / len2;
    if (t < 0) t = 0;
    float q[3] = {start[0] + dir[0] * t - p[0], start[1] + dir[1] * t - p[1], start[2] + dir[2] * t - p[2]};
    return sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
}

/* "top" target of a model: the index of the highest joint (largest y) among n positions, -1 when n <= 0. Ties: the first. Non-finite rows are skipped. */
static int aim_pick_top(const float (*pos)[3], int n)
{
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!isfinite(pos[i][0]) || !isfinite(pos[i][1]) || !isfinite(pos[i][2])) continue;
        if (best < 0 || pos[i][1] > pos[best][1]) best = i;
    }
    return best;
}

/* ---- `net aim cam` (2026-11-08, main-PC bench g0): a handgun does NOT use FUN_006EF600 (0 calls through the patched site in 13 shots). Its shot goes to the gun's
 * aim point, which FUN_00656ED0 (from uCameraCtrl_vf9 -> FUN_004B2520) takes from the CAMERA's crosshair ray (FUN_004ACE20 unprojects the screen centre; the first
 * hit along it is written to weapon+0x43B0, flag +0x43AC bit 1). So a lab shot is aimed by turning the camera: uCameraCtrl (player+0x4780) +0x23A8 pitch (normalised,
 * up = positive), +0x23AC yaw (rad, atan2(dx, dz) convention), copies at +0x23B0/+0x23B4/+0x23B8/+0x23BC. The aiming camera sits AIM_CAM_RIGHT units to the right of
 * the human: bench g0 put the crosshair on the head of (151,0,3) 252 units away with yaw 1.87 for a body line of 1.658 (-> 51.4 units) and pitch 0.084. */
#define AIM_CAM_RIGHT 51.4f          /* units the aiming camera sits right of the human (calibrated, bench g0 03:36) */
#define AIM_CAM_EYE 140.0f           /* camera height over the human's feet (guess that fits pitch 0.084 at 247 units with the scale below) */
#define AIM_CAM_PITCH_RAD 1.0471976f /* radians per unit of normalised pitch (guess: 60 degrees) */

/* the camera yaw (rad) and normalised pitch that put the crosshair on t, for a human standing at h (feet); 0 when degenerate */
static int aim_cam_solve(const float h[3], const float t[3], float right, float eye, float pitch_rad, float *yaw_out, float *pitch_out)
{
    if (!isfinite(h[0]) || !isfinite(h[2]) || !isfinite(t[0]) || !isfinite(t[1]) || !isfinite(t[2]) || !(pitch_rad > 0)) return 0;
    float dx = t[0] - h[0], dz = t[2] - h[2];
    if (dx * dx + dz * dz < 1.0f) return 0;
    float yaw = atan2f(dx, dz), cx = h[0], cz = h[2];
    for (int i = 0; i < 4; i++) {   /* the camera point depends on the yaw: right of the facing = (-cos yaw, sin yaw) */
        cx = h[0] - right * cosf(yaw), cz = h[2] + right * sinf(yaw);
        float ex = t[0] - cx, ez = t[2] - cz;
        if (ex * ex + ez * ez < 1.0f) return 0;
        yaw = atan2f(ex, ez);
    }
    float hd = sqrtf((t[0] - cx) * (t[0] - cx) + (t[2] - cz) * (t[2] - cz));
    float p = atan2f(t[1] - (h[1] + eye), hd) / pitch_rad;
    if (p > 1.0f) p = 1.0f;
    if (p < -1.0f) p = -1.0f;
    *yaw_out = yaw, *pitch_out = p;
    return 1;
}

#endif
