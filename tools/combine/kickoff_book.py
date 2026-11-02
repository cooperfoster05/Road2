# The kickoff openings book (Nov 1): names every taker's kickoff from its raw moves (book2.csv from
# extract_kickoffs2.py) and builds chess-style tables: how often each opening is used, how fast it gets there, and
# won / even / lost, by spawn and rank; plus a matchup grid (your opening against theirs).
#
# Openings (the first that fits):
#   fake       moved under 400 in the first 1.2 s
#   wavedash   a dodge that started coming back down to the ground (car under 50 high after 0.25 s+ in the air) before
#              the last 0.3 s (a speedflip's dodge comes 0.05-0.15 s after the jump, on the way up)
#   speedflip  a diagonal dodge in the first 0.9 s
#   double     two or more dodges before arriving (an early flip for speed, then a flip in)
#   flip-in    one dodge, in the last 0.5 s before the ball (boost, then flip into it)
#   early flip one dodge earlier than that (a flip for speed, then drive or jump in)
#   jump-in    no dodge, but a jump in the last 0.6 s
#   drive-in   no dodge, no jump
# Modifiers, reported on their own: delayed (arrived 0.3 s+ after the spawn's median for that rank), hook (the path
# strayed 350+ sideways from the straight line).
# Result for the taker's team: won = their team had the first touch after the kickoff hits; lost = the other team did;
# even = nobody touched it in 3.5 s. A goal within 5 s is counted on its own.
#
#   python kickoff_book.py book2.csv [more.csv ...] > report.txt   (also writes openings.csv: the named rows)
import sys, csv, collections, statistics as st

GROUP = {'1': 'Gold/Plat', '2': 'Gold/Plat', '3': 'Diamond', '4': 'Champ', '5': 'GC', '6': 'SSL', 'P': 'Pro'}
ORDER = ['Gold/Plat', 'Diamond', 'Champ', 'GC', 'SSL', 'Pro']
SPOTS = {'D': 'Diagonal', 'O': 'Off-centre', 'B': 'Back centre'}
OPENINGS = ['speedflip', 'flip-in', 'double', 'early flip', 'wavedash', 'jump-in', 'drive-in', 'fake']


def parse_dodges(s):
    out = []
    for d in s.split('|') if s else []:
        t, tx, ty, z, air = (float(x) for x in d.split(':'))
        out.append(dict(t=t, tx=tx, ty=ty, z=z, air=air))
    return out


def name(r):
    arrive = float(r['arrive'])
    if float(r['moved12']) < 400: return 'fake'
    dodges = [d for d in parse_dodges(r['dodges']) if d['t'] < arrive + 0.05]
    jumps = [float(x) for x in r['jumps'].split('|')] if r['jumps'] else []
    for d in dodges:
        if d['z'] < 50 and d['air'] >= 0.25 and d['t'] < arrive - 0.3: return 'wavedash'
    if dodges:
        d0 = dodges[0]
        m = max(abs(d0['tx']), abs(d0['ty']), 1e-6)
        if d0['t'] < 0.9 and abs(d0['tx']) > 0.35 * m and abs(d0['ty']) > 0.35 * m: return 'speedflip'
        if len(dodges) >= 2: return 'double'
        return 'flip-in' if d0['t'] >= arrive - 0.5 else 'early flip'
    if any(arrive - 0.6 <= j <= arrive + 0.05 for j in jumps): return 'jump-in'
    return 'drive-in'


def result(r):
    p = int(r['poss'])
    return 'won' if p > 0 else 'lost' if p < 0 else 'even'


def wnl(rows):
    n = len(rows)
    if not n: return '      -'
    w = sum(result(r) == 'won' for r in rows) / n; l = sum(result(r) == 'lost' for r in rows) / n
    return f'{100 * w:3.0f} / {100 * (1 - w - l):3.0f} / {100 * l:3.0f}'


def upfield(rows):
    """share of kickoffs with the ball in the other team's half 2 s later"""
    return f'{100 * sum(int(r["half"]) > 0 for r in rows) / max(1, len(rows)):3.0f}% upfield'


def main():
    rows = []
    for path in sys.argv[1:]:
        rows += list(csv.DictReader(open(path)))
    rows = [r for r in rows if r['label'] in GROUP]
    for r in rows:
        r['group'] = GROUP[r['label']]
        r['opening'] = name(r)
    # delayed: 0.3 s+ after the median arrival for the spawn and rank (of those who went)
    med = {}
    for (g, sp), rs in collections.defaultdict(list, {}).items(): pass
    by = collections.defaultdict(list)
    for r in rows:
        if r['opening'] != 'fake': by[(r['group'], r['spot'])].append(float(r['arrive']))
    for k, v in by.items(): med[k] = st.median(v)
    for r in rows:
        r['delayed'] = r['opening'] != 'fake' and float(r['arrive']) > med.get((r['group'], r['spot']), 9) + 0.3
        r['hook'] = float(r['pathdev']) > 350
    # pair the takers of each kickoff (for matchups and "who got there first")
    kick = collections.defaultdict(list)
    for r in rows: kick[r['kick']].append(r)
    for k, rs in kick.items():
        if len(rs) == 2:
            rs[0]['opp'], rs[1]['opp'] = rs[1], rs[0]
    print(f'{len(rows)} kickoff takers ({len(kick)} kickoffs). Result = your team won / even / lost the kickoff (%):')
    print('won = your team had the first touch after the kickoff hits; even = no touch for 3.5 s; lost = theirs.\n')
    for sp in 'DOB':
        print(f'=== {SPOTS[sp]} ===')
        for g in ORDER:
            R = [r for r in rows if r['group'] == g and r['spot'] == sp]
            if len(R) < 30: continue
            print(f'  {g} ({len(R)} takers; all kickoffs {wnl(R)}; median to ball {st.median(float(r["arrive"]) for r in R if r["opening"] != "fake"):.2f} s)')
            for o in OPENINGS:
                S = [r for r in R if r['opening'] == o]
                if not S: continue
                arr = st.median(float(r['arrive']) for r in S)
                goals = sum(int(r['goal']) > 0 for r in S) - sum(int(r['goal']) < 0 for r in S)
                print(f'    {o:10s} {100 * len(S) / len(R):4.0f}% used  {arr:.2f} s  {wnl(S)}  {upfield(S)}  (n {len(S)}, goals +{sum(int(r["goal"]) > 0 for r in S)}/-{sum(int(r["goal"]) < 0 for r in S)})')
            D = [r for r in R if r['delayed']]; H = [r for r in R if r['hook']]
            if D: print(f'    {"(delayed)":10s} {100 * len(D) / len(R):4.0f}% of them  {wnl(D)}')
            if H: print(f'    {"(hook)":10s} {100 * len(H) / len(R):4.0f}% of them  {wnl(H)}')
        print()
    # matchups: all ranks, diagonal and off-centre
    print('=== Matchups (diagonal + off-centre, all ranks): your opening vs theirs, you won / even / lost ===')
    P = [r for r in rows if r['spot'] in 'DO' and 'opp' in r]
    main_ = ['speedflip', 'flip-in', 'double', 'early flip', 'jump-in', 'fake']
    print('    you \\ them ' + ''.join(f'{o:>17s}' for o in main_))
    for a in main_:
        line = f'    {a:12s}'
        for b in main_:
            S = [r for r in P if r['opening'] == a and r['opp']['opening'] == b]
            line += f'  {wnl(S) if len(S) >= 40 else "      -":>15s}'
        print(line)
    print('\n=== Getting there first (all spawns and ranks): you won / even / lost ===')
    for lo, hi, nm in [(0.15, 9, 'first by 0.15 s+'), (0.05, 0.15, 'first by 0.05-0.15'), (-0.05, 0.05, 'level'), (-0.15, -0.05, 'second by 0.05-0.15'), (-9, -0.15, 'second by 0.15 s+')]:
        S = [r for r in rows if 'opp' in r and lo <= float(r['opp']['arrive']) - float(r['arrive']) < hi]
        print(f'    {nm:22s} {wnl(S)}  {upfield(S)}  (n {len(S)})')
    print('\n=== Teammates 1 s after contact (2s and 3s): what they did, and the team result ===')
    roles = collections.defaultdict(list)
    for r in rows:
        if int(r['size']) < 2 or not r['mates']: continue
        for m in r['mates'].split('|'):
            x, y = (float(v) for v in m.split(':'))
            role = 'cheat up' if y > -1500 else 'boost (corner)' if abs(x) > 2500 and y < -3000 else 'back post / goal' if y < -3800 else 'mid back'
            roles[(r['group'], role)].append(r)
    for g in ORDER:
        line = f'    {g:10s}'
        for role in ['cheat up', 'mid back', 'boost (corner)', 'back post / goal']:
            S = roles.get((g, role), [])
            line += f'  {role} {wnl(S)} (n {len(S)})' if len(S) >= 30 else ''
        print(line)
    with open('openings.csv', 'w', newline='') as fo:
        w = csv.writer(fo)
        w.writerow(['kick', 'group', 'size', 'spot', 'side', 'opening', 'delayed', 'hook', 'arrive', 'opp_opening', 'opp_arrive', 'result', 'goal', 'hitdir'])
        for r in rows:
            o = r.get('opp')
            w.writerow([r['kick'], r['group'], r['size'], r['spot'], r['side'], r['opening'], int(r['delayed']), int(r['hook']), r['arrive'],
                        o['opening'] if o else '', o['arrive'] if o else '', result(r), r['goal'], r['hitdir']])


if __name__ == '__main__':
    main()
