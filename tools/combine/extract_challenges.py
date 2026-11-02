# Real 50/50s and shadow-defense attacks from match replays, for the Combine's bot events. In both, you play one real
# player and the bot plays back the other one's real path (and the ball follows its real path until you get close).
#
# 50s: two players from opposite teams touch the ball within 0.25 s of each other, both right at it. The situation
#   starts 1.5 s before; you are either of the two (both are written), the bot is the other.
# Shadow: an attacker has the ball (touches it again within 1.5 s with nobody else touching it), moving toward the
#   other goal, with a defender goal-side of the ball 600-3500 away; and the attack ends dangerous (a goal, or the ball
#   on target when someone else first touches it). You are that defender, the bot is the attacker.
#
# Turned so you defend the blue goal (-y) and attack orange (+y). Only spawns that make sense: you on a surface, not
# jumping, not spinning. Written as lines:
#   S,kind,label,boost,tc,ballEnd,botEnd, your car: px,py,pz,vx,vy,vz,wx,wy,wz,qw,qx,qy,qz
#   B,t, the bot's car: px,py,pz,vx,vy,vz,wx,wy,wz,qw,qx,qy,qz
#   L,t, the ball: px,py,pz,vx,vy,vz,wx,wy,wz
# kind: F 50/50, D shadow defense. Times are from the start. tc: the 50's contact (shadow: 0). ballEnd: when the ball
# stops following its path. label: P pro, C clip, 1-6 ranked gold to SSL.
#
#   python extract_challenges.py out.csv --pro DIR --clips DIR --ranked RANKED_ROOT [--workers 2] [--limit N]
import sys, os, glob, argparse
import numpy as np
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_saves import load, RANKS, on_target


def on_surface(p):
    x, y, z = abs(p[0]), abs(p[1]), p[2]
    return z < 60 or z > 1940 or x > 3980 or (y > 5000 and not (x < 893 and z < 642)) or x + y > 7900


def ok_spawn(P, f):
    if np.isnan(P['pos'][f]).any(): return False
    if not on_surface(P['pos'][f]) and P['pos'][f][2] > 40: return False
    if P['jump'][max(0, f - 3):f + 1].any(): return False
    return np.linalg.norm(P['w'][f]) <= 3.5


def flip_y(p): return [p[0], -p[1], p[2]]
def flip_w(w): return [-w[0], w[1], -w[2]]
def flip_q(q): return [q[0], -q[1], q[2], -q[3]]


def fmt(xs, d=1): return ','.join(f'{x:.{d}f}' for x in xs)


def car_line(P, i, t0, T, flip, prefix):
    p, v, w, q = list(P['pos'][i]), list(P['vel'][i]), list(P['w'][i]), list(P['q'][i])
    if flip: p, v, w, q = flip_y(p), flip_y(v), flip_w(w), flip_q(q)
    head = f'{prefix},{T[i] - t0:.3f},' if prefix else ''
    return head + fmt(p) + ',' + fmt(v) + ',' + fmt(w, 3) + ',' + fmt(q, 4)


def ball_line(G, i, t0, flip):
    p, v, w = list(G['bp'][i]), list(G['bv'][i]), list(G['bw'][i])
    if flip: p, v, w = flip_y(p), flip_y(v), flip_w(w)
    return f'L,{G["T"][i] - t0:.3f},' + fmt(p) + ',' + fmt(v) + ',' + fmt(w, 3)


def challenges_in(job):
    path, label = job
    try:
        G, tl = load(path)
    except Exception as e:
        return path, None, str(e)
    T, bp, bv, Ps, n = G['T'], G['bp'], G['bv'], G['players'], G['n']
    out = []
    tframes = [f for f, _ in tl]

    def touches_between(a, b):   # touches with frame in [a, b)
        return [tl[k] for k in range(len(tl)) if a <= tl[k][0] < b]

    def path_ok(P, a, b):
        return not np.isnan(P['pos'][a:b + 1]).any() and not np.isnan(bp[a:b + 1]).any()

    # ---- 50/50s (at most 2 per replay, spread through the game: both sides of each) ----
    fifties = []
    for k in range(len(tl) - 1):
        (f, a), (g, b) = tl[k], tl[k + 1]
        A, B = Ps[a], Ps[b]
        if A['team0'] is None or B['team0'] is None or A['team0'] == B['team0']: continue
        if T[g] - T[f] > 0.25: continue
        if np.isnan(A['pos'][f]).any() or np.isnan(B['pos'][g]).any() or np.isnan(bp[f]).any(): continue
        if np.linalg.norm(A['pos'][f] - bp[f]) > 350 or np.linalg.norm(B['pos'][g] - bp[g]) > 350: continue
        tc = T[f]
        f0 = int(np.searchsorted(T, tc - 1.5))
        if f0 <= 0 or tc - T[f0] < 1.2: continue
        if touches_between(f0, f): continue                               # a clean run-in, nobody touching it
        if np.linalg.norm(bv[f0]) < 50 and np.hypot(bp[f0][0], bp[f0][1]) < 100: continue   # a kickoff
        fe = min(n - 1, int(np.searchsorted(T, tc + 1.2)))
        recs = []
        for you, bot in ((A, B), (B, A)):
            if not ok_spawn(you, f0) or not path_ok(bot, f0, fe): continue
            flip = not you['team0']          # team 0 attacks +y already
            lines = [f'S,F,{label},{you["boost"][f0]:.0f},{tc - T[f0]:.3f},{T[f - 1] - T[f0]:.3f},{T[fe] - T[f0]:.3f},' + car_line(you, f0, T[f0], T, flip, '')]
            lines += [car_line(bot, i, T[f0], T, flip, 'B') for i in range(f0, fe + 1)]
            lines += [ball_line(G, i, T[f0], flip) for i in range(f0, f)]
            recs.append('\n'.join(lines))
        if recs: fifties.append(recs)
    if fifties:
        picks = sorted(set(int(round(x)) for x in np.linspace(0, len(fifties) - 1, min(2, len(fifties)))))
        for p_ in picks: out += fifties[p_]

    # ---- shadow defense ----
    k = 0
    while k < len(tl) - 1:
        (f, a), (g, a2) = tl[k], tl[k + 1]
        k += 1
        A = Ps[a]
        if a2 != a or T[g] - T[f] > 1.5 or A['team0'] is None or np.isnan(bp[f]).any(): continue
        gs = 1.0 if A['team0'] else -1.0                  # A attacks +y if team 0
        if gs * bv[f][1] < 200 or gs * bp[f][1] < -2000: continue
        # the defender: goal-side of the ball, 600-3500 from it, a sensible spawn
        best, bd = None, 1e9
        for d, D in enumerate(Ps):
            if D['team0'] is None or D['team0'] == A['team0'] or np.isnan(D['pos'][f]).any(): continue
            dist = np.linalg.norm(D['pos'][f] - bp[f])
            if gs * (D['pos'][f][1] - bp[f][1]) < 200 or dist < 600 or dist > 3500: continue
            if dist < bd: best, bd = d, dist
        if best is None or not ok_spawn(Ps[best], f): continue
        D = Ps[best]
        # the attack: until someone other than A touches it, or 4.5 s
        h, kk = None, k
        while kk < len(tl) and T[tl[kk][0]] - T[f] <= 4.5:
            if tl[kk][1] != a: h = tl[kk][0]; break
            kk += 1
        tend = T[h] if h is not None else T[f] + 4.5
        fe = min(n - 1, int(np.searchsorted(T, tend)))
        if fe - f < 20: continue
        # dangerous: a goal by A's team in the window, or the ball on target just before the other touch
        scored = any(f <= gf <= fe and gt == A['team0'] for gf, gt in G.get('goals', []))
        danger = scored or on_target(bp[fe - 1], bv[fe - 1], gs)
        if not danger: continue
        fb = min(n - 1, fe + 18)
        if not path_ok(A, f, fb): continue
        flip = gs > 0                                     # D defends +y: turn it so D defends -y
        lines = [f'S,D,{label},{D["boost"][f]:.0f},0,{T[fe - 1] - T[f]:.3f},{T[fb] - T[f]:.3f},' + car_line(D, f, T[f], T, flip, '')]
        lines += [car_line(A, i, T[f], T, flip, 'B') for i in range(f, fb + 1)]
        lines += [ball_line(G, i, T[f], flip) for i in range(f, fe)]
        out.append('\n'.join(lines))
        while k < len(tl) - 1 and tl[k][0] < fe: k += 1     # no overlapping attacks
    return path, out, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--pro'); ap.add_argument('--clips'); ap.add_argument('--ranked')
    ap.add_argument('--workers', type=int, default=2)
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    jobs = []
    if a.pro: jobs += [(p, 'P') for p in sorted(glob.glob(os.path.join(a.pro, '*.replay')))]
    if a.clips: jobs += [(p, 'C') for p in sorted(glob.glob(os.path.join(a.clips, '*.replay')))]
    if a.ranked:
        for rank, lab in RANKS.items():
            jobs += [(p, lab) for p in sorted(glob.glob(os.path.join(a.ranked, rank, '*.replay')))]
    donef = a.out + '.done'
    done = set(open(donef).read().split('\n')) if os.path.exists(donef) else set()
    jobs = [j for j in jobs if j[0] not in done]
    if a.limit: jobs = jobs[:a.limit]
    with open(a.out, 'a') as fo, open(donef, 'a') as fd, Pool(a.workers) as pool:
        n = 0
        for path, recs, err in pool.imap_unordered(challenges_in, jobs):
            n += 1
            if err: print('skip', os.path.basename(path), err[:80], file=sys.stderr, flush=True)
            else:
                for r in recs: fo.write(r + '\n')
            fo.flush(); fd.write(path + '\n'); fd.flush()
            if n % 20 == 0: print(f'{n}/{len(jobs)} replays', file=sys.stderr, flush=True)
    print('done', n, 'replays')


if __name__ == '__main__':
    main()
