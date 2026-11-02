# Kickoff openings book, version 2 (Nov 1): every kickoff taker's raw moves, so the openings can be told apart in the
# analysis (speedflip, standard flip-in, double flip, wavedash, jump-in, drive-in, delayed, fake, hook), plus what
# their teammates did (cheat up, boost, stay back) and what came of it.
#
# One line per taker:
#   kick      replay id + contact frame (pairs the two takers of a kickoff)
#   label     P pro, 1-6 ranked gold to SSL;  team0;  size (players per team)
#   spot      D diagonal, O off-centre, B back centre;  side (+1 right, -1 left, as the taker sees it; 0 back centre)
#   arrive    s from go until within 200 of the ball;  moved12: distance moved in the first 1.2 s
#   dodges    each dodge before arriving: t:torqueX:torqueY:carZ:airTime  ("|" between)
#   jumps     each jump press before arriving: t  ("|" between)
#   pathdev   most the path strayed sideways from the straight line spawn -> ball
#   speed     speed when arriving;  boost: boost used getting there
#   hitdir    where the ball went off the hit (degrees, 0 = straight up field for the taker's team, + = to their right)
#   mates     each teammate 1 s after contact: their x:y (turned so the team attacks +y)  ("|" between)
#   poss      +1 / -1: which team had the first touch after the kickoff hits (from 0.35 s after contact, within 3.5 s); 0 none
#   half      +1 / 0 / -1: the ball's half 2 s after contact (+-800)
#   goal      +1 / -1: a goal within 5 s of contact; 0 none
#
#   python extract_kickoffs2.py book2.csv --pro DIR --ranked RANKED_ROOT [--workers 2] [--limit N]
import sys, os, glob, argparse
import numpy as np
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_saves import load, RANKS
from extract_kickoffs import filled, spot_of

HEADER = 'kick,label,team0,size,spot,side,arrive,moved12,dodges,jumps,pathdev,speed,boost,hitdir,mates,poss,half,goal'


def book_in(job):
    path, label = job
    rid = os.path.basename(path)[:8] + label
    try:
        G, tl = load(path)
    except Exception as e:
        return path, None, str(e)
    T, bp, bv, Ps, n = G['T'], G['bp'], G['bv'], G['players'], G['n']
    at_centre = (np.abs(bp[:, 0]) < 5) & (np.abs(bp[:, 1]) < 5) & (np.abs(bp[:, 2] - 92.75) < 8) & (np.linalg.norm(bv, axis=1) < 1)
    sizes = {True: sum(1 for P in Ps if P['team0'] is True), False: sum(1 for P in Ps if P['team0'] is False)}
    out = []
    i = 0
    while i < n:
        if not at_centre[i]: i += 1; continue
        j = i
        while j < n and at_centre[j]: j += 1
        fc, start = j, i
        i = j
        if fc >= n - 10: continue
        moved, who = None, None
        for f in range(start, fc):
            for P in Ps:
                if P['team0'] is None or np.isnan(P['vel'][f]).any(): continue
                if np.linalg.norm(P['vel'][f]) > 300: moved, who = f, P; break
            if moved is not None: break
        if moved is None: continue
        while moved > start and not np.isnan(who['vel'][moved - 1]).any() and np.linalg.norm(who['vel'][moved - 1]) > 15: moved -= 1
        go = moved
        if not 1.4 <= T[fc] - T[go] <= 4.5: continue
        fe = min(n - 1, int(np.searchsorted(T, T[fc] + 1.2)))
        FP = {}
        for k, P in enumerate(Ps):
            if P['team0'] is None: continue
            Q = filled(P, go, fe)
            if Q is not None and not np.isnan(Q['pos'][fc]).any(): FP[k] = Q
        takers = {}
        for k, P in FP.items():
            d = np.linalg.norm(P['pos'][fc] - bp[fc])
            if P['team0'] not in takers or d < takers[P['team0']][1]: takers[P['team0']] = (k, d)
        if len(takers) != 2: continue
        # outcomes (team 0's view; flipped per taker below)
        f2 = min(n - 1, int(np.searchsorted(T, T[fc] + 2.0)))
        half0 = 0
        if not np.isnan(bp[f2]).any(): half0 = 1 if bp[f2][1] > 800 else -1 if bp[f2][1] < -800 else 0
        poss0 = 0
        for tf, tk in tl:
            if T[tf] < T[fc] + 0.35: continue
            if T[tf] > T[fc] + 3.5: break
            poss0 = 1 if Ps[tk]['team0'] else -1
            break
        goal0 = 0
        for gf, gt in G.get('goals', []):
            if fc <= gf and T[gf] - T[fc] <= 5.0: goal0 = 1 if gt else -1; break
        fh = min(n - 1, fc + 6)
        rows = []
        for team, (k, _) in takers.items():
            P = FP[k]
            gs = 1.0 if team else -1.0
            sp = spot_of(P['pos'][go])
            if sp is None: rows = []; break
            # side as the taker sees it (attacking +y for team 0): right is +x for team 0, -x for team 1
            side = 0 if sp == 'B' else int(np.sign(P['pos'][go][0] * gs))
            arr = None
            for f in range(go, min(n, fc + 30)):
                if np.linalg.norm(P['pos'][f] - bp[f]) < 200: arr = T[f] - T[go]; break
            if arr is None: arr = T[fc] - T[go] + 0.5
            fa = min(n - 1, int(np.searchsorted(T, T[go] + arr)))
            # dodges and jumps before arriving
            dodges, jumps = [], []
            lastground = T[go]
            for f in range(go + 1, fa + 1):
                if P['pos'][f][2] < 25: lastground = T[f]
                if P['dodge'][f] and not P['dodge'][f - 1]:
                    dodges.append(f"{T[f] - T[go]:.3f}:{P['dtq'][f][0]:.2f}:{P['dtq'][f][1]:.2f}:{P['pos'][f][2]:.0f}:{T[f] - lastground:.3f}")
                if P['jump'][f] and not P['jump'][f - 1] and not P['dodge'][f]:
                    jumps.append(f"{T[f] - T[go]:.3f}")
            f12 = min(n - 1, int(np.searchsorted(T, T[go] + 1.2)))
            moved12 = float(np.linalg.norm(P['pos'][f12][:2] - P['pos'][go][:2]))
            a0 = P['pos'][go][:2]; d0 = -a0 / max(1.0, np.linalg.norm(a0))
            dev = 0.0
            for f in range(go, fa + 1):
                v = P['pos'][f][:2] - a0
                dev = max(dev, abs(v[0] * d0[1] - v[1] * d0[0]))
            speed = float(np.linalg.norm(P['vel'][fa]))
            used = max(0.0, P['boost'][go] - P['boost'][fa])
            hit = ''
            if not np.isnan(bv[fh]).any() and np.linalg.norm(bv[fh][:2]) > 100:
                hit = f"{np.degrees(np.arctan2(bv[fh][0] * gs, bv[fh][1] * gs)):.0f}"
            mates = []
            f1 = min(n - 1, int(np.searchsorted(T, T[fc] + 1.0)))
            for kk, Q in FP.items():
                if kk == k or Q['team0'] != team or np.isnan(Q['pos'][f1]).any(): continue
                mates.append(f"{Q['pos'][f1][0] * gs:.0f}:{Q['pos'][f1][1] * gs:.0f}")
            rows.append(f"{rid}{fc},{label},{int(team)},{sizes[team]},{sp},{side},{arr:.3f},{moved12:.0f},{'|'.join(dodges)},{'|'.join(jumps)},"
                        f"{dev:.0f},{speed:.0f},{used:.0f},{hit},{'|'.join(mates)},{int(poss0 * gs)},{int(half0 * gs)},{int(goal0 * gs)}")
        out += rows
    return path, out, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('book')
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
    donef = a.book + '.done'
    done = set(open(donef).read().split('\n')) if os.path.exists(donef) else set()
    jobs = [j for j in jobs if j[0] not in done]
    if a.limit: jobs = jobs[:a.limit]
    new = not os.path.exists(a.book)
    with open(a.book, 'a') as fb, open(donef, 'a') as fd, Pool(a.workers) as pool:
        if new: fb.write(HEADER + '\n')
        n = 0
        for path, rows, err in pool.imap_unordered(book_in, jobs):
            n += 1
            if err: print('skip', os.path.basename(path), err[:80], file=sys.stderr, flush=True)
            else:
                for r in rows: fb.write(r + '\n')
            fb.flush(); fd.write(path + '\n'); fd.flush()
            if n % 20 == 0: print(f'{n}/{len(jobs)} replays', file=sys.stderr, flush=True)
    print('done', n, 'replays')


if __name__ == '__main__':
    main()
