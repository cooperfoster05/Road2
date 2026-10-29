# Real saves from match replays, for testing the Combine's "is this save possible" check.
# A save: a shot by one team heading on target, then a touch by a defender before the ball crosses the line.
# Writes the ball and the saver's car at the moment of the shot (just after the attacker's touch).
import sys, glob, json, csv, numpy as np
sys.path.insert(0, '/home/claude/pro')
import subtr_actor
from touches import phys_touches

def V(d): return np.array([d['x'], d['y'], d['z']]) if d else np.zeros(3)

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
        P = dict(pos=np.full((n, 3), np.nan), vel=np.zeros((n, 3)), w=np.zeros((n, 3)), q=np.zeros((n, 4)), boost=np.zeros(n), team0=None)
        for i, fr in enumerate(pdat['frames']):
            dd = fr.get('Data') if isinstance(fr, dict) else None
            if not dd: continue
            rb = dd['rigid_body']; P['pos'][i] = V(rb['location']); P['vel'][i] = V(rb['linear_velocity']); P['w'][i] = V(rb['angular_velocity']) / 100.0
            r = rb['rotation']; P['q'][i] = [r['w'], r['x'], r['y'], r['z']]
            P['boost'][i] = (dd.get('boost_amount') or 0) * 100.0 / 255.0; P['team0'] = dd['is_team_0']
        players.append(P)
    G = dict(T=T, bp=bp, bv=bv, bw=bw, players=players, n=n)
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

rows = []
files = sorted(glob.glob('/mnt/user-data/uploads/data--RLCoach/pro_replays/*.replay')) + sorted(glob.glob('/mnt/user-data/uploads/data--RLCoach/clip_replays/*.replay'))
for path in files:
    rid = path.split('/')[-1][:8] + ('P' if 'pro_' in path else 'C')
    try: G, tl = load(path)
    except Exception as e: print('fail', rid, e, file=sys.stderr); continue
    T, bp, bv, bw, Ps = G['T'], G['bp'], G['bv'], G['bw'], G['players']
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
        rows.append([rid, f1, round(dt, 3), *bp[f1].round(1), *bv[f1].round(1), *bw[f1].round(3),
                     *S['pos'][f1].round(1), *S['vel'][f1].round(1), *S['w'][f1].round(3), *S['q'][f1].round(4), round(S['boost'][f1], 1),
                     *bp[g].round(1), *S['pos'][g].round(1), int(gs)])
    print(rid, len(rows), file=sys.stderr, flush=True)
with open('/home/claude/combine/real_saves.csv', 'w', newline='') as fo:
    w = csv.writer(fo)
    w.writerow(['replay', 'frame', 'dt', 'bx', 'by', 'bz', 'bvx', 'bvy', 'bvz', 'bwx', 'bwy', 'bwz', 'cx', 'cy', 'cz', 'cvx', 'cvy', 'cvz', 'cwx', 'cwy', 'cwz', 'qw', 'qx', 'qy', 'qz', 'boost',
                'sbx', 'sby', 'sbz', 'scx', 'scy', 'scz', 'gs'])
    w.writerows(rows)
print('saves', len(rows))
