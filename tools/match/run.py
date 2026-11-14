# Step 2: runs the match review (bulk, built from bulk.cpp) over every scan in scans/ listed in subset.csv.
# Writes out/stats.csv (per player and per team numbers, every touch's swing) and out/samples_*.bin (scoring-chance training rows).
import sys, os, gzip, subprocess, shutil, time
from multiprocessing import Pool
B = os.path.abspath('bulk')
def one(job):
    rid, rank, pl, k = job
    src = f'scans/{rid}.csv.gz'
    if not os.path.exists(src): return ''
    tmp = f'out/w_{os.getpid()}.csv'
    with gzip.open(src, 'rb') as f, open(tmp, 'wb') as o: shutil.copyfileobj(f, o)
    r = subprocess.run([B, tmp, rank, pl, f'out/samples_{os.getpid()}.bin'], capture_output=True, text=True)
    os.remove(tmp)
    return r.stdout if r.returncode == 0 else f'X,{rid},{r.stderr.strip()}\n'
if __name__ == '__main__':
    shutil.rmtree('out', ignore_errors=True); os.makedirs('out'); os.makedirs('models', exist_ok=True)
    jobs = [(*l.strip().split(','), i) for i, l in enumerate(open('subset.csv'))]
    t = time.time()
    with Pool(2) as p, open('out/stats.csv', 'w') as o:
        for s in p.imap_unordered(one, jobs, chunksize=4): o.write(s)
    print('secs', round(time.time() - t))
