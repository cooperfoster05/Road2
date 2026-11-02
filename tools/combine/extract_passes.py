# Real passes from match replays, for the Combine's First touches event.
# A pass: a touch by one player, then the next touch is by a teammate (the receiver), 0.25-3.5 s later, with no one
# else touching it in between. Each row is the moment just after the pass: the ball, and the receiver (position, speed,
# spin, facing, boost, jumping), plus where the ball was when they first touched it.
#
#   python extract_passes.py out.csv --pro DIR --clips DIR --ranked RANKED_ROOT [--workers 2]
#
# Safe to stop and run again: replays already done are kept in out.csv.done and skipped.
import sys, os, glob, csv, argparse
import numpy as np
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_saves import load, RANKS

HEADER = ['replay', 'frame', 'dt', 'bx', 'by', 'bz', 'bvx', 'bvy', 'bvz', 'bwx', 'bwy', 'bwz', 'cx', 'cy', 'cz', 'cvx', 'cvy', 'cvz',
          'cwx', 'cwy', 'cwz', 'qw', 'qx', 'qy', 'qz', 'boost', 'rbx', 'rby', 'rbz', 'rbvx', 'rbvy', 'rbvz', 'rcx', 'rcy', 'rcz', 'att', 'jump', 'label']


def passes_in(job):
    path, label = job
    rid = os.path.basename(path)[:8] + label
    try:
        G, tl = load(path)
    except Exception as e:
        return path, None, str(e)
    T, bp, bv, bw, Ps = G['T'], G['bp'], G['bv'], G['bw'], G['players']
    rows = []
    for (f, a), (g, b) in zip(tl, tl[1:]):
        A, B = Ps[a], Ps[b]
        if a == b or A['team0'] is None or A['team0'] != B['team0']: continue
        att = 1 if A['team0'] else -1            # the way the passing team attacks (team 0 at +y)
        f1 = f + 1
        if f1 >= g or np.isnan(bp[f1]).any() or np.isnan(bp[g]).any() or np.isnan(B['pos'][f1]).any(): continue
        dt = T[g] - T[f1]
        if dt < 0.25 or dt > 3.5: continue
        # a pass goes somewhere: the ball moved at least 600 before the receiver got it
        if np.linalg.norm(bp[g] - bp[f1]) < 600: continue
        jumping = int(B['jump'][max(0, f1 - 3):f1 + 1].any())
        rows.append([rid, f1, round(dt, 3), *bp[f1].round(1), *bv[f1].round(1), *bw[f1].round(3),
                     *B['pos'][f1].round(1), *B['vel'][f1].round(1), *B['w'][f1].round(3), *B['q'][f1].round(4), round(B['boost'][f1], 1),
                     *bp[g].round(1), *bv[g - 1].round(1), *B['pos'][g].round(1), att, jumping, label])
    return path, rows, None


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
    new = not os.path.exists(a.out)
    with open(a.out, 'a', newline='') as fo, open(donef, 'a') as fd, Pool(a.workers) as pool:
        w = csv.writer(fo)
        if new: w.writerow(HEADER)
        n = 0
        for path, rows, err in pool.imap_unordered(passes_in, jobs):
            n += 1
            if err: print('skip', os.path.basename(path), err[:80], file=sys.stderr, flush=True)
            else: w.writerows(rows)
            fo.flush(); fd.write(path + '\n'); fd.flush()
            if n % 20 == 0: print(f'{n}/{len(jobs)} replays', file=sys.stderr, flush=True)
    print('done', n, 'replays')


if __name__ == '__main__':
    main()
