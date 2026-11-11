# Clean takes.csv for Score on a goalie (Nov 4): the ball's real path must not show anyone else touching it before the
# take (a teammate's pass, another opponent: cars that aren't in the drill, so it looks like a ghost hits it), and it
# stops just before the take touch itself (that touch is yours to make).
import sys, math
src, dst = sys.argv[1], sys.argv[2]
recs, cur = [], None
for line in open(src):
    if line.startswith('S,'):
        if cur: recs.append(cur)
        cur = {'S': line, 'B': [], 'L': []}
    elif cur is not None:
        cur[line[0]].append(line)
if cur: recs.append(cur)

def touched(L):
    # a velocity change gravity, the floor or a wall can't explain
    pts = [list(map(float, l.split(',')[1:8])) for l in L]
    for (t0, x0, y0, z0, vx0, vy0, vz0), (t1, x1, y1, z1, vx1, vy1, vz1) in zip(pts, pts[1:]):
        dt = t1 - t0
        if dt <= 0 or dt > 0.12: continue
        nearWall = abs(x1) > 3800 or abs(y1) > 4900 or abs(x0) > 3800 or abs(y0) > 4900
        nearFloor = min(z0, z1) < 160
        nearCeil = max(z0, z1) > 1850
        dvh = math.hypot(vx1 - vx0, vy1 - vy0)
        dvz = (vz1 - vz0) + 650 * dt
        if not nearWall and dvh > 250: return True
        if not (nearFloor or nearCeil or nearWall) and abs(dvz) > 250: return True
    return False

kept, ghost = [], 0
for r in recs:
    tc = float(r['S'].split(',')[4])
    L = [l for l in r['L'] if float(l.split(',')[1]) < tc - 0.02]   # up to just before the take
    if len(L) < 5: continue
    if touched(L): ghost += 1; continue
    kept.append(r['S'] + ''.join(r['B']) + ''.join(L))
open(dst, 'w').write(''.join(kept))
from collections import Counter
print(len(recs), 'takes;', ghost, 'had someone else touch the ball first;', len(kept), 'kept')
print(Counter((r.split(',')[1], r.split(',')[2]) for r in kept))
