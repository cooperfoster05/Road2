# Every kickoff in match replays: for an openings book (which kickoffs people take, how fast, how often they win, by
# rank and spawn) and for the Combine's kickoff battles (you take one taker's spawn; the bot plays the other taker's
# real kickoff).
#
# A kickoff: the ball sits still at centre; "go" is the first frame a car moves; contact is when the ball first moves.
# Each team's taker is its car nearest the ball at contact. For each taker:
#   spot   D diagonal, O off-centre, B back centre (from where they started)
#   type   speedflip (a diagonal dodge in the first 0.9 s), flip (dodged, not that), noflip (never dodged before the
#          ball), fake (moved under 400 in the first 1.2 s)
#   arrive seconds from go until they were within 200 of the ball
# Outcome for the taker's team: where the ball is 1.5 s after contact (+1 their opponents' half, -1 their own, 0 the
# middle 800), or a goal within 3 s (+2 / -2).
#
#   python extract_kickoffs.py book.csv battles.csv --pro DIR --ranked RANKED_ROOT [--workers 2] [--limit N]
# book.csv: one line per taker. battles.csv: challenge lines (kind K) for both takers of each kickoff.
import sys, os, glob, argparse
import numpy as np
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_saves import load, RANKS
from extract_challenges import car_line, ball_line

BOOK_HEADER = 'replay,label,team0,spot,type,arrive,first_dodge,dodge_tx,dodge_ty,boost_used,opp_type,opp_arrive,outcome'


def filled(P, a, b):
    """A copy of a player whose frames a..b have no gaps: cars waiting at their kickoff spawn often send no position
    until they move, so the start is filled back from the first one they send (and any gap after from the last)."""
    Q = dict(P)
    for key in ('pos', 'vel', 'w', 'q'):
        arr = P[key].copy()
        ok = ~np.isnan(arr[a:b + 1]).any(axis=1) if key == 'pos' else ~np.isnan(P['pos'][a:b + 1]).any(axis=1)
        idx = np.nonzero(ok)[0]
        if len(idx) == 0: return None
        first = a + idx[0]
        arr[a:first] = arr[first]
        if key == 'vel' or key == 'w': arr[a:first] = 0
        last = first
        for f in range(first, b + 1):
            if ok[f - a]: last = f
            else: arr[f] = arr[last]
        Q[key] = arr
    return Q


def spot_of(p):
    x, y = abs(p[0]), abs(p[1])
    if x > 1500 and 2300 < y < 2800: return 'D'
    if 100 < x < 500 and 3600 < y < 4100: return 'O'
    if x < 100 and 4400 < y < 4800: return 'B'
    return None


def kickoffs_in(job):
    path, label = job
    rid = os.path.basename(path)[:8] + label
    try:
        G, tl = load(path)
    except Exception as e:
        return path, None, None, str(e)
    T, bp, bv, Ps, n = G['T'], G['bp'], G['bv'], G['players'], G['n']
    at_centre = (np.abs(bp[:, 0]) < 5) & (np.abs(bp[:, 1]) < 5) & (np.abs(bp[:, 2] - 92.75) < 8) & (np.linalg.norm(bv, axis=1) < 1)
    book, battles = [], []
    i = 0
    while i < n:
        if not at_centre[i]: i += 1; continue
        j = i
        while j < n and at_centre[j]: j += 1
        fc = j                                  # the ball's first frame moving
        i = j
        if fc >= n - 10: continue
        # go: the first frame in the run a car moves
        go = None
        for f in range(i - 1, -1, -1):
            if not at_centre[f]: break
            go = f
        if go is None: continue
        start = go
        # go: a car really accelerating (over 300), traced back to where it started; cars settling onto the ground
        # just after the respawn move a little and don't count
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
        # each team's taker: nearest the ball at contact (gaps at the spawn filled in)
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
        info = {}
        for team, (k, _) in takers.items():
            P = FP[k]
            sp = spot_of(P['pos'][go])
            if sp is None: break
            # arrival
            arr = None
            for f in range(go, min(n, fc + 30)):
                if not np.isnan(P['pos'][f]).any() and np.linalg.norm(P['pos'][f] - bp[f]) < 200: arr = T[f] - T[go]; break
            if arr is None: arr = T[fc] - T[go] + 0.5
            # dodges before arrival
            first_dodge, tq = None, (0.0, 0.0)
            for f in range(go + 1, min(n, int(np.searchsorted(T, T[go] + arr)) + 1)):
                if P['dodge'][f] and not P['dodge'][f - 1]:
                    first_dodge = T[f] - T[go]; tq = (float(P['dtq'][f][0]), float(P['dtq'][f][1])); break
            f12 = int(np.searchsorted(T, T[go] + 1.2))
            moved12 = np.linalg.norm(P['pos'][min(f12, n - 1)][:2] - P['pos'][go][:2])
            m = max(abs(tq[0]), abs(tq[1]), 1e-6)
            diagonal = abs(tq[0]) > 0.35 * m and abs(tq[1]) > 0.35 * m
            if moved12 < 400: kind = 'fake'
            elif first_dodge is not None and first_dodge < 0.9 and diagonal: kind = 'speedflip'
            elif first_dodge is not None: kind = 'flip'
            else: kind = 'noflip'
            fa = min(n - 1, int(np.searchsorted(T, T[go] + arr)))
            used = max(0.0, P['boost'][go] - P['boost'][fa])
            info[team] = dict(k=k, spot=sp, kind=kind, arr=arr, fd=first_dodge, tq=tq, used=used)
        if len(info) != 2: continue
        # outcome: a goal within 3 s, else where the ball is 1.5 s after contact
        f15 = min(n - 1, int(np.searchsorted(T, T[fc] + 1.5)))
        goal = None
        for gf, gt in G.get('goals', []):
            if fc <= gf and T[gf] - T[fc] <= 3.0: goal = gt; break
        for team in (True, False):
            me, op = info[team], info[not team]
            gs = 1.0 if team else -1.0
            if goal is not None: out = 2 if goal == team else -2
            elif np.isnan(bp[f15]).any(): out = 0
            else:
                y = gs * bp[f15][1]
                out = 1 if y > 400 else -1 if y < -400 else 0
            fd_s = '' if me['fd'] is None else f"{me['fd']:.3f}"
            book.append(f"{rid},{label},{int(team)},{me['spot']},{me['kind']},{me['arr']:.3f},{fd_s},"
                        f"{me['tq'][0]:.2f},{me['tq'][1]:.2f},{me['used']:.0f},{op['kind']},{op['arr']:.3f},{out}")
        # battles: you are one taker, the bot the other (both written)
        for team in (True, False):
            you, bot = FP[info[team]['k']], FP[info[not team]['k']]
            if np.isnan(bot['pos'][go:fe + 1]).any(): continue
            flip = not you['team0']
            lines = [f'S,K,{label},33,{T[fc] - T[go]:.3f},{T[fc - 1] - T[go]:.3f},{T[fe] - T[go]:.3f},' + car_line(you, go, T[go], T, flip, '')]
            lines += [car_line(bot, f, T[go], T, flip, 'B') for f in range(go, fe + 1)]
            lines += [ball_line(G, f, T[go], flip) for f in range(go, fc)]
            battles.append('\n'.join(lines))
    return path, book, battles, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('book'); ap.add_argument('battles')
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
    with open(a.book, 'a') as fb, open(a.battles, 'a') as fo, open(donef, 'a') as fd, Pool(a.workers) as pool:
        if new: fb.write(BOOK_HEADER + '\n')
        n = 0
        for path, book, battles, err in pool.imap_unordered(kickoffs_in, jobs):
            n += 1
            if err: print('skip', os.path.basename(path), err[:80], file=sys.stderr, flush=True)
            else:
                for r in book: fb.write(r + '\n')
                for r in battles: fo.write(r + '\n')
            fb.flush(); fo.flush(); fd.write(path + '\n'); fd.flush()
            if n % 20 == 0: print(f'{n}/{len(jobs)} replays', file=sys.stderr, flush=True)
    print('done', n, 'replays')


if __name__ == '__main__':
    main()
