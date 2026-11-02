# Picks the Combine's 50s and shadow-defense situations from the extracted ones: up to N per level per kind (levels by
# the game's rank: 1-2 gold/plat, 3 diamond, 4 champ, 5 GC, 6 + pro SSL), duplicates dropped, clips left out.
#   python pick_challenges.py out.csv N in1.csv [in2.csv ...]
import sys, random
out, N, ins = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
LEVEL = {'1': 1, '2': 1, '3': 2, '4': 3, '5': 4, '6': 5, 'P': 5}
recs, seen = {}, set()
for path in ins:
    cur = None
    for line in open(path):
        line = line.rstrip('\r\n')
        if line.startswith('S,'):
            cur = None
            if line in seen: continue
            seen.add(line)
            kind, label = line.split(',')[1], line.split(',')[2]
            if label not in LEVEL: continue
            cur = [line]
            recs.setdefault((kind, LEVEL[label]), []).append(cur)
        elif cur is not None:
            cur.append(line)
random.seed(7)
with open(out, 'w') as fo:
    for key in sorted(recs):
        pool = recs[key]
        pick = random.sample(pool, min(N, len(pool)))
        print(key, len(pool), 'kept', len(pick))
        for r in pick: fo.write('\n'.join(r) + '\n')
