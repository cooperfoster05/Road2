# Real takes for Score on a goalie (Nov 4): moments where a player wins the ball and keeps it in midfield or the
# other team's half, with a defender back and nobody else on them. The situation starts 1.25 s BEFORE the take, so
# you drive up to it the way the real player did and make the take yourself; then it's yours to turn into a goal
# (dribble, bounce dribble, power shot, ground to air, double tap, anything). Nexto plays the defender.
#
# Ground take: the ball low (rolling or bouncing) when they take it. Wall take: the ball on or by the side wall.
# A take: player A touches it, A touches it again within 1.5 s with nobody else touching in between, and A didn't
# have it already (no touch by A in the 1.5 s before). The defender D: the other team's player nearest their own goal,
# goal-side of the ball, within 3500 of that goal and 1500+ from the ball; no other opponent within 1300 of it.
#
# Turned so you attack orange (+y). Lines (the same as challenges.csv):
#   S,kind,label,boost,tc,ballEnd,botEnd, your car (A at the start): px,py,pz,vx,vy,vz,wx,wy,wz,qw,qx,qy,qz
#   B,t, the defender's car (its real path, for the hand-off)
#   L,t, the ball (its real path up to just before the take; nobody touches it in that time)
# kind: G ground take, W wall take. tc and ballEnd: the take, from the start.
#
#   python extract_takes.py out.csv --ranked RANKED_ROOT --pro DIR [--limit N]
import sys, os, glob, argparse, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_saves import load, RANKS
from extract_challenges import ok_spawn, car_line, ball_line

LEAD = 1.25


def takes_in(path, label):
    G, tl = load(path)
    T, bp, bv, Ps, n = G['T'], G['bp'], G['bv'], G['players'], G['n']
    out, last = [], -1e9
    for k in range(1, len(tl) - 1):
        f, a = tl[k]
        A = Ps[a]
        if A['team0'] is None or np.isnan(bp[f]).any() or np.isnan(A['pos'][f]).any(): continue
        if T[f] - last < 4.0: continue
        g, a2 = tl[k + 1]
        if a2 != a or T[g] - T[f] > 1.5: continue                      # kept it
        pf, pa = tl[k - 1]
        if pa == a and T[f] - T[pf] < 1.5: continue                     # already had it
        gs = 1.0 if A['team0'] else -1.0
        by = gs * bp[f][1]
        if by < -2000 or by > 3800: continue                            # midfield to their third, not on the goal line
        wall = abs(bp[f][0]) > 3550 or (abs(A['pos'][f][0]) > 3800 and A['pos'][f][2] > 150)
        ground = bp[f][2] < 200
        if not (wall or ground): continue                               # aerial takes are someone else's idea
        if wall and by < -1000: continue
        # the defender: back, goal-side, not on the ball; nobody else pressuring
        goal = np.array([0.0, gs * 5120.0, 0.0])
        best, bdist, crowd = None, 1e9, False
        for d, D in enumerate(Ps):
            if D['team0'] is None or D['team0'] == A['team0'] or np.isnan(D['pos'][f]).any(): continue
            toBall = np.linalg.norm(D['pos'][f] - bp[f])
            toGoal = np.linalg.norm(D['pos'][f] - goal)
            if gs * (D['pos'][f][1] - bp[f][1]) > 500 and toGoal < 3500 and toBall > 1500 and toGoal < bdist:
                if best is not None and np.linalg.norm(Ps[best]['pos'][f] - bp[f]) < 1300: crowd = True
                best, bdist = d, toGoal
            elif toBall < 1300: crowd = True
        if best is None or crowd: continue
        D = Ps[best]
        f0 = int(np.searchsorted(T, T[f] - LEAD))
        if f0 <= 0 or T[f] - T[f0] < 1.0: continue
        if not ok_spawn(A, f0) or not ok_spawn(D, f0): continue
        if np.linalg.norm(A['pos'][f0] - bp[f0]) < 400: continue         # not already on it at the start
        if any(f0 <= tf < f for tf, tp in tl): continue                   # nobody touched it before the take (no ghosts)
        fe = min(n - 1, int(np.searchsorted(T, T[f0] + 4.0)))
        if np.isnan(D['pos'][f0:fe + 1]).any() or np.isnan(bp[f0:f + 1]).any(): continue
        flip = not A['team0']
        tc = T[f] - T[f0]
        lines = [f'S,{"W" if wall else "G"},{label},{A["boost"][f0]:.0f},{tc:.3f},{tc:.3f},{T[fe] - T[f0]:.3f},' + car_line(A, f0, T[f0], T, flip, '')]
        lines += [car_line(D, i, T[f0], T, flip, 'B') for i in range(f0, fe + 1)]
        lines += [ball_line(G, i, T[f0], flip) for i in range(f0, f)]   # up to just before the take: that touch is yours
        out.append('\n'.join(lines))
        last = T[f]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--pro', action='append', default=[]); ap.add_argument('--ranked')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--seconds', type=float, default=0, help='stop starting new replays after this long')
    ap.add_argument('--part', default='0/1', help='i/n: this run takes every n-th replay from i (run n at once)')
    a = ap.parse_args()
    jobs = []
    for d in a.pro: jobs += [(p, 'P') for p in sorted(glob.glob(os.path.join(d, '*.replay')))]
    if a.ranked:
        for rank, lab in RANKS.items():
            jobs += [(p, lab) for p in sorted(glob.glob(os.path.join(a.ranked, rank, '*.replay')))]
    donef = a.out + '.done'
    done = set(open(donef).read().split('\n')) if os.path.exists(donef) else set()
    pi, pn = (int(x) for x in a.part.split('/'))
    jobs = [j for idx, j in enumerate(jobs) if idx % pn == pi]
    jobs = [j for j in jobs if j[0] not in done]
    if a.limit: jobs = jobs[:a.limit]
    t0, n, found = time.time(), 0, 0
    with open(a.out, 'a') as fo, open(donef, 'a') as fd:
        for path, label in jobs:
            if a.seconds and time.time() - t0 > a.seconds: break
            try:
                recs = takes_in(path, label)
                for r in recs: fo.write(r + '\n')
                found += len(recs)
            except Exception as e:
                print('skip', os.path.basename(path), str(e)[:80], file=sys.stderr, flush=True)
            fo.flush(); fd.write(path + '\n'); fd.flush()
            n += 1
    print(f'{n} replays this run, {found} takes, {len(jobs) - n} left')


if __name__ == '__main__':
    main()
