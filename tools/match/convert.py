# Turns replay files into Road2 match scans (the same rows the plugin writes when it scans a replay in game).
#   python convert.py subset.csv replays_root out_dir [seconds]
import sys, os, gzip, math, time
import numpy as np
from multiprocessing import Pool
import subtr_actor

def V(d): return (d['x'], d['y'], d['z']) if d else (0.0, 0.0, 0.0)

def rot(q):
    # quaternion to Unreal pitch, yaw, roll (65536 a turn)
    w, x, y, z = q['w'], q['x'], q['y'], q['z']
    fx, fy, fz = 1 - 2*(y*y + z*z), 2*(x*y + w*z), 2*(x*z - w*y)       # forward
    ux, uy, uz = 2*(x*z + w*y), 2*(y*z - w*x), 1 - 2*(x*x + y*y)       # up
    rx, ry, rz = 2*(x*y - w*z), 1 - 2*(x*x + z*z), 2*(y*z + w*x)       # left/right
    yaw = math.atan2(fy, fx); pitch = math.asin(max(-1.0, min(1.0, fz)))
    roll = math.atan2(-rz, uz) if abs(fz) < 0.999 else 0.0
    k = 32768.0 / math.pi
    return int(pitch * k), int(yaw * k), int(roll * k)

def on_surface(p):
    x, y, z = abs(p[0]), abs(p[1]), p[2]
    return z < 30 or z > 2010 or x > 4060 or (y > 5085 and not (x < 893 and z < 642))

def surf2(p):   # near a wall, the floor or the ceiling (the same test the touch finder was checked with)
    x, y, z = abs(p[0]), abs(p[1]), p[2]
    return z < 140 or z > 1900 or x > 3930 or (y > 5000 and not (x < 893 and z < 642)) or x + y > 7850

def convert(job):
    try: return convert1(job)
    except Exception as e: return job[0], 'convert: ' + repr(e)[:120]

def convert1(job):
    rid, rank, playlist, src, dst = job
    try:
        d = subtr_actor.get_replay_frames_data(src)
    except Exception as e:
        return rid, 'parse: ' + str(e)[:80]
    fd = d['frame_data']; meta = fd['metadata_frames']; n = len(meta)
    players = fd['players']
    if n < 600 or not players: return rid, 'short'
    ids = [str(pid) for pid, _ in players]
    idx = {s: i for i, s in enumerate(ids)}
    team = [None] * len(players); name = [''] * len(players)
    for i, (pid, pd) in enumerate(players):
        for fr in pd['frames']:
            dd = fr.get('Data') if isinstance(fr, dict) else None
            if dd: team[i] = 0 if dd['is_team_0'] else 1; name[i] = (dd.get('player_name') or 'player').replace(',', ' '); break
    # ball arrays for touches
    bp = np.full((n, 3), np.nan); bv = np.zeros((n, 3))
    for i, fr in enumerate(fd['ball_data']['frames']):
        rb = fr.get('Data', {}).get('rigid_body') if isinstance(fr, dict) else None
        if rb: bp[i] = V(rb['location']); bv[i] = V(rb['linear_velocity'])
    pos = np.full((len(players), n, 3), np.nan)
    for k, (pid, pd) in enumerate(players):
        for i, fr in enumerate(pd['frames']):
            dd = fr.get('Data') if isinstance(fr, dict) else None
            if dd: pos[k, i] = V(dd['rigid_body']['location'])
    T = np.array([m['time'] for m in meta])
    # Keep frames at most 30 a second (a few replay files are recorded at 120). The touch finder below was checked
    # against the game's own touch reports in an in-game scan: 97% of what it finds is real and it misses none. Its
    # count barely moves between 15, 30 and 120 a second, so 30 keeps the most detail.
    keep = []
    for i in range(n):
        if np.isnan(bp[i]).any() and keep and not np.isnan(bp[keep[-1]]).any(): pass
        if not keep or T[i] - T[keep[-1]] >= 0.030: keep.append(i)
    keepset = set(keep)
    import bisect
    def snap(f):   # a replay frame to the kept frame at or after it
        j = bisect.bisect_left(keep, f)
        return keep[min(j, len(keep) - 1)]
    # touches: the replay's own (team only, so the nearest player of that team) plus ball velocity jumps with a car close by
    touches = set()
    for t in d['touch_events']:
        f = t['frame']
        if f >= n or np.isnan(bp[f]).any(): continue
        best, bd = None, 1e9
        for k in range(len(players)):
            if team[k] != (0 if t['team_is_team_0'] else 1) or np.isnan(pos[k, f]).any(): continue
            dist = np.linalg.norm(pos[k, f] - bp[f])
            if dist < bd: best, bd = k, dist
        if best is not None and bd < 400: touches.add((snap(f), best))
    for a_, i in zip(keep, keep[1:]):
        if np.isnan(bp[i]).any() or np.isnan(bp[a_]).any(): continue
        dt = T[i] - T[a_]
        if dt <= 0 or dt > 0.1: continue
        dv = bv[i] - bv[a_]; dv[2] += 650 * dt
        m = np.linalg.norm(dv)
        if m < 80: continue
        p0 = bp[a_]
        near = surf2(bp[i]) or surf2(p0)
        best, bd = None, 1e9
        for k in range(len(players)):
            for q in (a_, i):
                if np.isnan(pos[k, q]).any(): continue
                dist = np.linalg.norm(pos[k, q] - bp[q])
                if dist < bd: best, bd = k, dist
        if best is not None and bd < (250 if near else 330) and (not near or m > 250): touches.add((i, best))
    tby = {}
    for f, k in sorted(touches): tby.setdefault(f, []).append(k)
    # counters: goals, shots, saves, assists as the replay marks them
    ev = {}
    for g in d.get('goal_events', []):
        k = idx.get(str(g.get('player')))
        if k is not None: ev.setdefault(snap(g['frame']), []).append((k, 'goal'))
    for s in d.get('player_stat_events', []):
        k = idx.get(str(s.get('player'))); kind = str(s.get('kind')).lower()
        if k is not None and kind in ('shot', 'save', 'assist'): ev.setdefault(snap(s['frame']), []).append((k, kind))
    goals = sorted((g['frame'], g['team_zero_score'], g['team_one_score']) for g in d.get('goal_events', []))
    cnt = [dict(touch=0, shot=0, save=0, goal=0, assist=0) for _ in players]
    L = ['# Road2 match scan v1', '# replay,' + rid, '# frames,%d' % n, '# fps,30', '# source,file', '# rank,' + rank, '# playlist,' + playlist]
    seen = [False] * len(players)
    gi = 0; sc = (0, 0)
    for i in keep:
        m = meta[i]
        while gi < len(goals) and goals[gi][0] <= i: sc = (goals[gi][1], goals[gi][2]); gi += 1
        L.append('G,%d,%.3f,%d,0,%d,%d' % (i, m['time'], m.get('seconds_remaining') or 0, sc[0], sc[1]))
        if not np.isnan(bp[i]).any():
            L.append('B,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,0,0,0' % (i, *bp[i], *bv[i]))
        for k, (pid, pd) in enumerate(players):
            fr = pd['frames'][i]
            dd = fr.get('Data') if isinstance(fr, dict) else None
            if not dd or team[k] is None: continue
            if not seen[k]: seen[k] = True; L.append('P,%d,%d,%s' % (k, team[k], name[k]))
            rb = dd['rigid_body']; p = V(rb['location']); v = V(rb['linear_velocity']); w = V(rb['angular_velocity'])
            pi, ya, ro = rot(rb['rotation'])
            air = dd.get('jump_active') or dd.get('double_jump_active') or dd.get('dodge_active')
            ground = 1 if (on_surface(p) and not air) else 0
            sup = 1 if v[0]*v[0] + v[1]*v[1] + v[2]*v[2] >= 2200.0*2200.0 else 0
            inp = dd.get('input') or {}
            L.append('C,%d,%d,%.1f,%.1f,%.1f,%d,%d,%d,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.0f,%d,%d,0,0,%d,%d,%d' % (
                i, k, *p, pi, ya, ro, *v, *w, (dd.get('boost_amount') or 0) * 100.0 / 255.0, ground, sup,
                inp.get('throttle') if inp.get('throttle') is not None else 128, inp.get('steer') if inp.get('steer') is not None else 128,
                1 if dd.get('powerslide_active') else 0))
        for k in tby.get(i, []): L.append('T,%d,%d' % (i, k))
        for k, kind in ev.get(i, []):
            cnt[k][kind] += 1; c = cnt[k]
            L.append('S,%d,%d,0,%d,%d,%d,%d,0' % (i, k, c['shot'], c['save'], c['goal'], c['assist']))
    with gzip.open(dst + '.tmp', 'wt', compresslevel=4) as f: f.write('\n'.join(L) + '\n')
    os.replace(dst + '.tmp', dst)
    return rid, None

if __name__ == '__main__':
    sub, root, out = sys.argv[1:4]; budget = float(sys.argv[4]) if len(sys.argv) > 4 else 150
    jobs = []
    for line in open(sub):
        rid, rank, pl = line.strip().split(',')
        dst = os.path.join(out, rid + '.csv.gz')
        if not os.path.exists(dst) and not os.path.exists(dst + '.bad'): jobs.append((rid, rank, pl, os.path.join(root, rank, rid + '.replay'), dst))
    t0 = time.time(); done = 0
    with Pool(2) as pool:
        for rid, err in pool.imap_unordered(convert, jobs):
            if err: open(os.path.join(out, rid + '.csv.gz.bad'), 'w').write(err); print('BAD', rid, err)
            else: done += 1
            if time.time() - t0 > budget: pool.terminate(); break
    print('done', done, 'left', len(jobs) - done, 'secs', round(time.time() - t0))
