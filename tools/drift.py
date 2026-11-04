"""drift.py <owner_log> <viewer_log> [--delay-ms N]: position error of a puppet against the machine that owns the character.

Both files come from `net drift <file>` (rows `O,t,x,y,z,yaw,action` = the machine's own character, `P,t,x,y,z,yaw,action` = the puppet as shown, t in the
SENDER's clock = the clock the O rows of the owner use). The owner's file supplies the truth (its O rows, linearly interpolated), the viewer's file the P rows:
error(t) = distance(P(t), O(t)) in the xz plane. Prints mean / median / 95th percentile / max over the moving stretches and over everything.
"""
import bisect, math, sys

def load(path, kind):
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        p = line.strip().split(',')
        if len(p) >= 7 and p[0] == kind:
            rows.append((int(p[1]), float(p[2]), float(p[3]), float(p[4]), float(p[5]), int(p[6])))
    rows.sort()
    return rows

def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    own, view = load(sys.argv[1], 'O'), load(sys.argv[2], 'P')
    if not own or not view:
        print('no rows: owner O=%d viewer P=%d' % (len(own), len(view))); return 1
    ts = [r[0] for r in own]
    errs, moving, teles = [], [], 0
    for t, x, y, z, yaw, act in view:
        i = bisect.bisect_left(ts, t)
        if i <= 0 or i >= len(own): continue
        a, b = own[i - 1], own[i]
        if b[0] == a[0] or b[0] - a[0] > 400: continue
        u = (t - a[0]) / (b[0] - a[0])
        tx, tz = a[1] + (b[1] - a[1]) * u, a[3] + (b[3] - a[3]) * u
        e = math.hypot(x - tx, z - tz)
        speed = math.hypot(b[1] - a[1], b[3] - a[3]) / max(1, (b[0] - a[0])) * 1000.0
        errs.append(e)
        if speed > 20: moving.append(e)
        if e > 300: teles += 1
    def stats(name, v):
        if not v: print('%-10s n=0' % name); return
        s = sorted(v)
        print('%-10s n=%5d mean %6.2f median %6.2f p95 %6.2f max %7.2f' % (name, len(v), sum(v) / len(v), s[len(s) // 2], s[int(len(s) * 0.95)], s[-1]))
    print('owner rows %d, viewer rows %d, paired %d, errors over 300 units: %d' % (len(own), len(view), len(errs), teles))
    stats('all', errs)
    stats('moving', moving)
    return 0

if __name__ == '__main__':
    sys.exit(main())
