# Real landings (Nov 4, to check the Combine Recovery event's par): every time a player comes down on their wheels
# after 0.5 s+ in the air, their landing speed and heading, then how long they took to get 1500, 2500 and 3500 away
# (straight line) and where that point was. The par's drive model (Combine::RealDriveTime) is then run on the same
# numbers to see how a good player's real times compare with it.
#   python landings.py out.csv --pro DIR [--ranked RANKED_ROOT] [--limit N]
# out: label,airtime,landSpeed,vx,vy,d,t,px,py  (one row per landing and distance; position relative to the landing)
import sys, os, glob, argparse
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_saves import load, RANKS

def up_z(q):
    w, x, y, z = q
    return 1 - 2 * (x * x + y * y)

def scan(path, label):
    try: G, _ = load(path)
    except Exception as e: return []
    T, n = G['T'], G['n']
    rows = []
    for P in G['players']:
        pos, vel, q = P['pos'], P['vel'], P['q']
        air_start = None
        i = 1
        while i < n:
            if np.isnan(pos[i]).any(): air_start = None; i += 1; continue
            z = pos[i][2]
            if z > 60:
                if air_start is None: air_start = i
            else:
                if air_start is not None and T[i] - T[air_start] >= 0.5 and up_z(q[i]) > 0.8:
                    # landed on the floor, wheels down: follow the next 4 s while they stay on the floor
                    land = pos[i].copy(); v = vel[i].copy()
                    ds = [1500.0, 2500.0, 3500.0]; k = 0
                    for j in range(i, n):
                        if np.isnan(pos[j]).any() or T[j] - T[i] > 4.0: break
                        if pos[j][2] > 200: break   # jumped or went up a wall: not a drive
                        d = np.hypot(pos[j][0] - land[0], pos[j][1] - land[1])
                        while k < len(ds) and d >= ds[k]:
                            rows.append(f"{label},{T[i] - T[air_start]:.2f},{np.hypot(v[0], v[1]):.0f},{v[0]:.0f},{v[1]:.0f},{ds[k]:.0f},{T[j] - T[i]:.3f},{pos[j][0] - land[0]:.0f},{pos[j][1] - land[1]:.0f}")
                            k += 1
                        if k >= len(ds): break
                air_start = None
            i += 1
    return rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out'); ap.add_argument('--pro'); ap.add_argument('--ranked'); ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    jobs = []
    if a.pro: jobs += [(p, 'P') for p in sorted(glob.glob(os.path.join(a.pro, '*.replay')))]
    if a.ranked:
        for rank, lab in RANKS.items():
            jobs += [(p, lab) for p in sorted(glob.glob(os.path.join(a.ranked, rank, '*.replay')))]
    if a.limit: jobs = jobs[:a.limit]
    with open(a.out, 'w') as f:
        f.write('label,airtime,landSpeed,vx,vy,d,t,px,py\n')
        for k, (p, lab) in enumerate(jobs):
            for r in scan(p, lab): f.write(r + '\n')
            if k % 20 == 0: print(f'{k}/{len(jobs)}', flush=True)
    print('done')

if __name__ == '__main__': main()
