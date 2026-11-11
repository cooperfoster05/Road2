# Turns real passes (real_passes.csv from extract_passes.py) into the Combine's first-touch situations
# (data/Road2/combine/pass_situations.csv): turned so the passing team attacks the orange goal (+y), with the
# receiver's facing as Unreal pitch / yaw / roll. Receivers already jumping, mid-air or spinning are left out (bad
# spawns). Same columns as save_situations.csv, plus where the ball was received: its height, its speed, and how long
# after the pass.
#   python pass_situations.py real_passes.csv pass_situations.csv
import sys, csv, math

src, dst = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(src)))
out = []
left = {'jumping': 0, 'air': 0, 'spin': 0}
for r in rows:
    F = lambda k: float(r[k])
    if r['jump'] == '1': left['jumping'] += 1; continue
    cx, cy, cz = F('cx'), F('cy'), F('cz')
    ax, ay = abs(cx), abs(cy)
    on_surface = cz < 60 or cz > 1940 or ax > 3980 or (ay > 5000 and not (ax < 893 and cz < 642)) or ax + ay > 7900
    if not on_surface and cz > 40: left['air'] += 1; continue
    w = (F('cwx'), F('cwy'), F('cwz'))
    if math.sqrt(sum(x * x for x in w)) > 3.5: left['spin'] += 1; continue
    qw, qx, qy, qz = F('qw'), F('qx'), F('qy'), F('qz')
    fx, fy, fz = 1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qw * qz), 2 * (qx * qz - qw * qy)
    rz = 2 * (qy * qz + qw * qx)
    ux, uy, uz = 2 * (qx * qz + qw * qy), 2 * (qy * qz - qw * qx), 1 - 2 * (qx * qx + qy * qy)
    pitch = math.asin(max(-1.0, min(1.0, fz))); yaw = math.atan2(fy, fx)

    def up_err(roll):
        sp, cp, sy, cy_, sr, cr = math.sin(pitch), math.cos(pitch), math.sin(yaw), math.cos(yaw), math.sin(roll), math.cos(roll)
        ex, ey, ez = -(cr * sp * cy_ + sr * sy), cy_ * sr - cr * sp * sy, cr * cp
        return abs(ex - ux) + abs(ey - uy) + abs(ez - uz)
    roll = math.atan2(-rz, uz)
    if up_err(roll) > up_err(-roll): roll = -roll
    b = [F('bx'), F('by'), F('bz')]; bv = [F('bvx'), F('bvy'), F('bvz')]; bw = [F('bwx'), F('bwy'), F('bwz')]
    c = [cx, cy, cz]; cv = [F('cvx'), F('cvy'), F('cvz')]; cw = list(w)
    if int(r['att']) < 0:
        # mirror y so they attack +y (a reflection: spins flip in x and z, yaw and roll change sign)
        b[1], bv[1] = -b[1], -bv[1]; bw = [-bw[0], bw[1], -bw[2]]
        c[1], cv[1] = -c[1], -cv[1]; cw = [-cw[0], cw[1], -cw[2]]
        yaw, roll = -yaw, -roll
    rspeed = math.sqrt(F('rbvx') ** 2 + F('rbvy') ** 2 + F('rbvz') ** 2)
    out.append([r['label'], *[round(x, 1) for x in b], *[round(x, 1) for x in bv], *[round(x, 3) for x in bw],
                *[round(x, 1) for x in c], *[round(x, 1) for x in cv], *[round(x, 3) for x in cw],
                round(pitch, 4), round(yaw, 4), round(roll, 4), F('boost'), 1 if on_surface else 0,
                F('rbz'), round(rspeed, 1), F('dt')])
with open(dst, 'w', newline='') as fo:
    wr = csv.writer(fo)
    wr.writerow(['source', 'bx', 'by', 'bz', 'bvx', 'bvy', 'bvz', 'bwx', 'bwy', 'bwz', 'cx', 'cy', 'cz', 'cvx', 'cvy', 'cvz', 'cwx', 'cwy', 'cwz',
                 'pitch', 'yaw', 'roll', 'boost', 'surface', 'rz', 'rspeed', 'dt'])
    wr.writerows(out)
print(f'{len(rows)} passes, {len(out)} kept; left out: {left}')
