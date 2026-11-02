# Real saves from match replays, for the Combine's Saves event (and checking its "is this save possible" estimate).
# A save: a shot by one team heading on target, then a touch by a defender before the ball crosses the line.
# Each row is the moment just after the shot: the ball, and the defender who saved it (position, speed, spin, facing,
# boost), plus whether that defender was already jumping or dodging then (those make bad spawns), and a label for
# where the replay came from: P pro, C clip, 1-6 ranked (gold, platinum, diamond, champion, grand champion, SSL).
#
#   python extract_saves.py out.csv --pro DIR --clips DIR --ranked RANKED_ROOT [--workers 2]
#
# Safe to stop and run again: replays already done are kept in out.csv.done and skipped.
import sys, os, glob, csv, argparse
import numpy as np
import subtr_actor
from multiprocessing import Pool

RANKS = {'gold': '1', 'platinum': '2', 'diamond': '3', 'champion': '4', 'grand-champion': '5', 'supersonic-legend': '6'}
HEADER = ['replay', 'frame', 'dt', 'bx', 'by', 'bz', 'bvx', 'bvy', 'bvz', 'bwx', 'bwy', 'bwz', 'cx', 'cy', 'cz', 'cvx', 'cvy', 'cvz',
          'cwx', 'cwy', 'cwz', 'qw', 'qx', 'qy', 'qz', 'boost', 'sbx', 'sby', 'sbz', 'scx', 'scy', 'scz', 'gs', 'jump', 'label']


def V(d): return np.array([d['x'], d['y'], d['z']]) if d else np.zeros(3)


def surf_near(p):
    x, y, z = abs(p[0]), abs(p[1]), p[2]
    return z < 140 or z > 1900 or x > 3930 or (y > 5000 and not (x < 893 and z < 642)) or x + y > 7850


def phys_touches(G, thr=80.0, reach=330.0):
    """Touches from the ball's velocity jumps with a car close by (replay touch events only fire when the touching team changes)."""
    T, bp, bv = G['T'], G['bp'], G['bv']
    out = []
    for i in range(1, G['n']):
        if np.isnan(bp[i]).any() or np.isnan(bp[i - 1]).any(): continue
        dt = T[i] - T[i - 1]
        if dt <= 0 or dt > 0.1: continue
        dv = bv[i] - bv[i - 1]; dv[2] += 650 * dt
        if np.linalg.norm(dv) < thr: continue
        nearS = surf_near(bp[i]) or surf_near(bp[i - 1])
        best, bd = None, 1e9
        for k, P in enumerate(G['players']):
            for q in (i - 1, i):
                if np.isnan(P['pos'][q]).any(): continue
                d = np.linalg.norm(P['pos'][q] - bp[q])
                if d < bd: best, bd = k, d
        if best is not None and bd < (250 if nearS else reach) and (not nearS or np.linalg.norm(dv) > 250): out.append((i, best))
    return out


def load(path):
    d = subtr_actor.get_replay_frames_data(path)
    fd = d['frame_data']; T = np.array([m['time'] for m in fd['metadata_frames']]); n = len(T)
    bp = np.full((n, 3), np.nan); bv = np.zeros((n, 3)); bw = np.zeros((n, 3))
    for i, fr in enumerate(fd['ball_data']['frames']):
        rb = fr.get('Data', {}).get('rigid_body') if isinstance(fr, dict) else None
        if rb:
            bp[i] = V(rb['location']); bv[i] = V(rb['linear_velocity']); bw[i] = V(rb['angular_velocity']) / 100.0
    players = []
    for pid, pdat in fd['players']:
        P = dict(pos=np.full((n, 3), np.nan), vel=np.zeros((n, 3)), w=np.zeros((n, 3)), q=np.zeros((n, 4)), boost=np.zeros(n),
                 jump=np.zeros(n, bool), dodge=np.zeros(n, bool), dtq=np.zeros((n, 3)), boosting=np.zeros(n, bool), team0=None)
        for i, fr in enumerate(pdat['frames']):
            dd = fr.get('Data') if isinstance(fr, dict) else None
            if not dd: continue
            rb = dd['rigid_body']; P['pos'][i] = V(rb['location']); P['vel'][i] = V(rb['linear_velocity']); P['w'][i] = V(rb['angular_velocity']) / 100.0
            r = rb['rotation']; P['q'][i] = [r['w'], r['x'], r['y'], r['z']]
            P['boost'][i] = (dd.get('boost_amount') or 0) * 100.0 / 255.0; P['team0'] = dd['is_team_0']
            P['jump'][i] = bool(dd.get('jump_active') or dd.get('double_jump_active') or dd.get('dodge_active'))
            P['dodge'][i] = bool(dd.get('dodge_active'))
            P['boosting'][i] = bool(dd.get('boost_active'))
            tq = (dd.get('input') or {}).get('dodge_torque')
            if tq: P['dtq'][i] = tq[:3]
        players.append(P)
    G = dict(T=T, bp=bp, bv=bv, bw=bw, players=players, n=n,
             goals=[(g['frame'], g['scoring_team_is_team_0']) for g in d.get('goal_events', [])])
    touches = []
    for t in d['touch_events']:
        f = t['frame']
        if f >= n or np.isnan(bp[f]).any(): continue
        best, bd = None, 1e9
        for k, P in enumerate(players):
            if P['team0'] != t['team_is_team_0'] or np.isnan(P['pos'][f]).any(): continue
            dist = np.linalg.norm(P['pos'][f] - bp[f])
            if dist < bd: best, bd = k, dist
        if best is not None and bd < 400: touches.append((f, best))
    allt = sorted(set(touches) | set(phys_touches(G)))
    ded = []
    for f, k in allt:
        if ded and f - ded[-1][0] <= 2 and ded[-1][1] == k: continue
        ded.append((f, k))
    return G, ded


def on_target(p, v, gs):
    """Ballistic path (gravity, floor bounces) crosses the goal line in the goal mouth within 3 s."""
    p = p.copy(); v = v.copy()
    for _ in range(360):
        if gs * p[1] >= 5120: return abs(p[0]) < 893 and p[2] < 642
        if p[2] < 93 and v[2] < 0: v[2] = -0.6 * v[2]
        if abs(p[0]) > 4000: v[0] = -v[0]
        p = p + v / 120.0; v[2] -= 650 / 120.0
    return False


def saves_in(job):
    path, label = job
    rid = os.path.basename(path)[:8] + label
    try:
        G, tl = load(path)
    except Exception as e:
        return path, None, str(e)
    T, bp, bv, bw, Ps = G['T'], G['bp'], G['bv'], G['bw'], G['players']
    rows = []
    for (f, a), (g, s) in zip(tl, tl[1:]):
        A, S = Ps[a], Ps[s]
        if A['team0'] is None or S['team0'] is None or A['team0'] == S['team0']: continue
        gs = 1.0 if A['team0'] else -1.0          # team 0 (blue) shoots at +y
        f1 = f + 1
        if f1 >= g or g - 1 <= f1 or np.isnan(bp[f1]).any() or np.isnan(bp[g - 1]).any(): continue
        dt = T[g] - T[f1]
        if dt < 0.1 or dt > 4.0: continue
        if gs * bv[g - 1][1] < 300 or gs * bp[g][1] > 5120: continue
        if not on_target(bp[g - 1], bv[g - 1], gs): continue
        if np.isnan(S['pos'][f1]).any(): continue
        # already jumping or dodging at the shot (or in the few frames before it)
        jumping = int(S['jump'][max(0, f1 - 3):f1 + 1].any())
        rows.append([rid, f1, round(dt, 3), *bp[f1].round(1), *bv[f1].round(1), *bw[f1].round(3),
                     *S['pos'][f1].round(1), *S['vel'][f1].round(1), *S['w'][f1].round(3), *S['q'][f1].round(4), round(S['boost'][f1], 1),
                     *bp[g].round(1), *S['pos'][g].round(1), int(gs), jumping, label])
    return path, rows, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--pro'); ap.add_argument('--clips'); ap.add_argument('--ranked')
    ap.add_argument('--workers', type=int, default=2)
    ap.add_argument('--limit', type=int, default=0, help='stop after this many new replays (for short runs)')
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
    new = not os.path.exists(a.out)
    with open(a.out, 'a', newline='') as fo, open(donef, 'a') as fd, Pool(a.workers) as pool:
        w = csv.writer(fo)
        if new: w.writerow(HEADER)
        n = 0
        for path, rows, err in pool.imap_unordered(saves_in, jobs):
            n += 1
            if err: print('skip', os.path.basename(path), err[:80], file=sys.stderr, flush=True)
            else: w.writerows(rows)
            fo.flush(); fd.write(path + '\n'); fd.flush()
            if n % 20 == 0: print(f'{n}/{len(jobs)} replays', file=sys.stderr, flush=True)
    print('done', n, 'replays')


if __name__ == '__main__':
    main()
