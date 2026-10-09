"""Prototype of the controller overlay drawing, to iterate on looks before porting to C++.
Unit space: 240 x 165 at scale 1. Renders previews with PIL (supersampled).
"""
import math, sys, json
from PIL import Image, ImageDraw, ImageFont

def cubic(p0, c1, c2, p1, n=8):
    pts = []
    for i in range(1, n + 1):
        t = i / n
        a = (1 - t) ** 3; b = 3 * (1 - t) ** 2 * t; c = 3 * (1 - t) * t * t; d = t ** 3
        pts.append((a * p0[0] + b * c1[0] + c * c2[0] + d * p1[0], a * p0[1] + b * c1[1] + c * c2[1] + d * p1[1]))
    return pts

def path(start, segs, n=8):
    pts = [start]
    cur = start
    for c1, c2, p in segs:
        pts += cubic(cur, c1, c2, p, n)
        cur = p
    return pts

def mirror_half(left, cx=120.0):
    """left: points from top-center going around the left side to bottom-center.
    Returns a closed outline (clockwise on screen)."""
    right = [(2 * cx - x, y) for (x, y) in reversed(left)]
    pts = left + right[1:-1]
    return pts

# ---------- ear clipping ----------
def area(poly):
    s = 0
    for i in range(len(poly)):
        x0, y0 = poly[i]; x1, y1 = poly[(i + 1) % len(poly)]
        s += x0 * y1 - x1 * y0
    return s / 2

def point_in_tri(p, a, b, c):
    def sign(p1, p2, p3):
        return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])
    d1 = sign(p, a, b); d2 = sign(p, b, c); d3 = sign(p, c, a)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)

def dedupe(poly):
    out = []
    for p in poly:
        if not out or (abs(out[-1][0] - p[0]) > 1e-3 or abs(out[-1][1] - p[1]) > 1e-3):
            out.append(p)
    if len(out) > 1 and abs(out[0][0] - out[-1][0]) < 1e-3 and abs(out[0][1] - out[-1][1]) < 1e-3:
        out.pop()
    return out

def triangulate(poly):
    poly = dedupe(poly)
    if area(poly) < 0:
        poly = list(reversed(poly))
    idx = list(range(len(poly)))
    tris = []
    guard = 0
    while len(idx) > 3 and guard < 100000:
        guard += 1
        found = False
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            if cross <= 1e-9:
                continue
            if any(point_in_tri(poly[j], a, b, c) for j in idx if j not in (i0, i1, i2)):
                continue
            tris.append((a, b, c))
            idx.pop(k)
            found = True
            break
        if not found:
            # degenerate: drop a vertex
            idx.pop(0)
    if len(idx) == 3:
        tris.append(tuple(poly[i] for i in idx))
    return tris

# ---------- shapes ----------
def ds4_body():
    left = path((120, 30), [
        ((100, 30), (84, 27), (66, 27)),      # top edge
        ((50, 27), (38, 30), (31, 40)),       # shoulder
        ((24, 50), (21, 64), (19, 80)),       # outer side
        ((16, 100), (12, 124), (16, 140)),    # grip outer
        ((19, 152), (32, 158), (42, 154)),    # grip tip
        ((52, 150), (58, 140), (64, 128)),    # grip inner
        ((70, 118), (78, 113), (90, 113)),    # inner toward center
        ((102, 113), (110, 114), (120, 114)), # bottom center
    ])
    return mirror_half(left)

def xbox_body():
    left = path((120, 34), [
        ((104, 34), (92, 30), (74, 28)),      # top edge, slight rise
        ((56, 26), (40, 30), (32, 40)),       # shoulder
        ((22, 54), (16, 76), (14, 100)),      # outer side
        ((12, 122), (14, 142), (26, 150)),    # grip
        ((36, 157), (48, 153), (56, 142)),    # grip bottom
        ((64, 130), (72, 121), (86, 118)),    # inner up
        ((100, 116), (110, 118), (120, 118)), # bottom center
    ])
    return mirror_half(left)

def ds4_trigger(left=True):
    # rounded tab behind the shoulder
    pts = path((34, 26), [
        ((32, 12), (38, 2), (52, 1)),
        ((66, 0), (74, 8), (76, 26)),
    ])
    pts = pts + [(76, 30), (34, 30)]
    if not left:
        pts = [(240 - x, y) for x, y in pts]
    return pts

def xbox_trigger(left=True):
    pts = path((36, 30), [
        ((34, 14), (38, 0), (56, -2)),
        ((70, -3), (78, 8), (80, 28)),
    ])
    pts = pts + [(80, 32), (36, 32)]
    if not left:
        pts = [(240 - x, y) for x, y in pts]
    return pts


def offset(poly, d):
    poly = dedupe(poly)
    if area(poly) < 0: poly = list(reversed(poly))
    n = len(poly); out = []
    for i in range(n):
        a = poly[i - 1]; b = poly[i]; c = poly[(i + 1) % n]
        # outward normal for CCW in y-down coords: average of edge normals
        def nrm(p, q):
            dx, dy = q[0] - p[0], q[1] - p[1]; L = math.hypot(dx, dy) or 1
            return (dy / L, -dx / L)
        n1 = nrm(a, b); n2 = nrm(b, c)
        nx, ny = n1[0] + n2[0], n1[1] + n2[1]; L = math.hypot(nx, ny) or 1
        out.append((b[0] + nx / L * d, b[1] + ny / L * d))
    return out

# ---------- renderer ----------
class R:
    def __init__(self, w=240, h=170, s=4, bg=(70, 72, 78)):
        self.s = s
        self.img = Image.new("RGBA", (w * s, h * s), bg + (255,))
        self.d = ImageDraw.Draw(self.img, "RGBA")
    def P(self, p):
        return (p[0] * self.s, p[1] * self.s)
    def tris(self, tris, col):
        for t in tris:
            self.d.polygon([self.P(p) for p in t], fill=col)
    def poly(self, pts, col):
        self.tris(triangulate(pts), col)
    def outline(self, pts, col, w=1.0):
        q = [self.P(p) for p in pts] + [self.P(pts[0])]
        self.d.line(q, fill=col, width=max(1, int(w * self.s)))
    def disc(self, c, r, col):
        x, y = self.P(c); r *= self.s
        self.d.ellipse([x - r, y - r, x + r, y + r], fill=col)
    def ring(self, c, r, w, col):
        x, y = self.P(c); r *= self.s
        self.d.ellipse([x - r, y - r, x + r, y + r], outline=col, width=max(1, int(w * self.s)))
    def rbox(self, x, y, w, h, r, col):
        self.d.rounded_rectangle([x * self.s, y * self.s, (x + w) * self.s, (y + h) * self.s], radius=r * self.s, fill=col)
    def line(self, a, b, w, col):
        self.d.line([self.P(a), self.P(b)], fill=col, width=max(1, int(w * self.s)))
    def text(self, c, s, size, col):
        try:
            f = ImageFont.truetype("DejaVuSans-Bold.ttf", int(size * self.s))
        except Exception:
            f = ImageFont.load_default()
        x, y = self.P(c)
        bb = self.d.textbbox((0, 0), s, font=f)
        self.d.text((x - (bb[2] - bb[0]) / 2, y - (bb[3] - bb[1]) / 2 - bb[1]), s, font=f, fill=col)
    def save(self, path, final_scale=2):
        im = self.img.resize((self.img.width * final_scale // self.s, self.img.height * final_scale // self.s), Image.LANCZOS)
        im.save(path)

THEMES = {
    "dark": dict(body=(30, 33, 42), body_edge=(52, 57, 70), face=(38, 42, 53), well=(20, 22, 28), part=(48, 53, 66),
                 trig=(24, 26, 33), label=(150, 158, 175), cap=(58, 63, 78), capHi=(225, 230, 240), rim=(10, 11, 14), bevel=(44, 48, 60)),
    "light": dict(body=(232, 234, 238), body_edge=(205, 208, 215), face=(218, 221, 228), well=(40, 43, 52), part=(60, 64, 76),
                  trig=(200, 203, 210), label=(90, 96, 110), cap=(70, 75, 88), capHi=(255, 255, 255), rim=(28, 30, 36), bevel=(196, 199, 207)),
}
ACCENT = (90, 170, 255)

PS_COL = {"S": (124, 178, 232), "E": (255, 102, 102), "W": (214, 145, 193), "N": (64, 226, 160)}
XB_COL = {"S": (96, 185, 64), "E": (224, 70, 50), "W": (52, 132, 232), "N": (245, 200, 30)}

def draw(style, theme, state, out):
    T = THEMES[theme]
    r = R()
    ps = style == "ps4"
    # triggers (fill meter from the bottom)
    for left in (True, False):
        val = state.get("lt" if left else "rt", 0)
        if ps:
            x, w = (36, 40) if left else (164, 40)
            y, h = 0, 30
        else:
            x, w = (40, 40) if left else (160, 40)
            y, h = -6, 38
        r.rbox(x, y, w, h, 10, T["trig"])
        if val > 0.02:
            fh = h * val
            r.rbox(x, y + h - fh, w, fh, min(10, fh / 2), (255, 150, 90) if left else (90, 220, 140))
        r.text((x + w / 2, y + 11), ("L2" if left else "R2") if ps else ("LT" if left else "RT"), 7, T["label"] if val < 0.5 else (20, 22, 28))
    # bumpers
    for left in (True, False):
        on = state.get("l1" if left else "r1")
        x = 34 if left else 150
        r.rbox(x, 22 if ps else 24, 56, 10, 5, ACCENT if on else T["part"])
    body = ds4_body() if ps else xbox_body()
    r.poly(offset(body, 2.2), T["rim"])
    r.poly(body, T["bevel"])
    r.poly(offset(body, -2.5), T["body"])

    if ps:
        # touchpad
        r.rbox(86, 30, 68, 40, 7, T["face"])
        r.rbox(86, 30, 68, 40, 7, None) if False else None
        # share/options
        r.rbox(72, 34, 6, 12, 3, T["part"]); r.rbox(162, 34, 6, 12, 3, T["part"])
        # PS button
        r.disc((120, 90), 6, T["part"])
        dpad, ls, rs, face = (58, 62), (88, 98), (152, 98), (182, 62)
    else:
        r.disc((120, 48), 9, T["part"]); r.ring((120, 48), 9, 1.2, T["bevel"] if theme == "dark" else T["rim"]); r.disc((120, 48), 4, T["label"])
        r.disc((105, 68), 3.5, T["part"]); r.disc((135, 68), 3.5, T["part"])
        ls, dpad, rs, face = (62, 62), (94, 98), (148, 98), (180, 62)

    # wells
    for c in (dpad, face):
        r.disc(c, 24 if c == face else 21, T["face"])
    # dpad
    r.rbox(dpad[0] - 5, dpad[1] - 15, 10, 30, 2.5, T["part"])
    r.rbox(dpad[0] - 15, dpad[1] - 5, 30, 10, 2.5, T["part"])
    # sticks
    for c, key, click in ((ls, "ls", "l3"), (rs, "rs", "r3")):
        r.disc(c, 19, T["well"])
        sx, sy = state.get(key, (0, 0))
        on = abs(sx) + abs(sy) > 0.08
        cc = (c[0] + sx * 7, c[1] + sy * 7)
        if on:
            r.line(c, (c[0] + sx * 13, c[1] + sy * 13), 2.0, ACCENT)
        r.disc(cc, 13, T["cap"])
        r.ring(cc, 13, 1.4, ACCENT if (state.get(click) or on) else T["body_edge"])
        r.disc(cc, 8.5, T["part"] if not on else T["cap"])
        r.ring(cc, 8.5, 0.8, T["body_edge"])
    # face buttons
    cols = PS_COL if ps else XB_COL
    for k, (dx, dy) in {"N": (0, -15), "W": (-15, 0), "E": (15, 0), "S": (0, 15)}.items():
        c = (face[0] + dx, face[1] + dy)
        on = state.get(k)
        col = cols[k]
        if on:
            mix = tuple(int(col[i]*0.45 + T["face"][i]*0.55) for i in range(3))
            r.disc(c, 10.5, mix)
            r.disc(c, 8, col)
        else:
            r.disc(c, 8, T["well"])
        sym = (255, 255, 255) if on else col
        if ps:
            rr = 3.4
            if k == "S":
                r.line((c[0] - rr, c[1] - rr), (c[0] + rr, c[1] + rr), 1.4, sym); r.line((c[0] - rr, c[1] + rr), (c[0] + rr, c[1] - rr), 1.4, sym)
            elif k == "E":
                r.ring(c, rr, 1.3, sym)
            elif k == "W":
                r.d.rectangle([ (c[0]-rr)*r.s, (c[1]-rr)*r.s, (c[0]+rr)*r.s, (c[1]+rr)*r.s ], outline=sym, width=int(1.3*r.s))
            else:
                r.d.polygon([r.P((c[0], c[1] - rr)), r.P((c[0] + rr, c[1] + rr * 0.8)), r.P((c[0] - rr, c[1] + rr * 0.8))], outline=sym, width=int(1.3*r.s))
        else:
            r.text(c, {"S": "A", "E": "B", "W": "X", "N": "Y"}[k], 8, sym)
    if state.get("tag"):
        r.text((120, 160), state["tag"], 7, (190, 150, 255))
    r.save(out)

if __name__ == "__main__":
    st = {"lt": 0.0, "rt": 0.7, "ls": (0.6, -0.5), "S": True, "r1": True, "tag": "AIR ROLL LEFT"}
    for style in ("ps4", "xbox"):
        for theme in ("dark", "light"):
            draw(style, theme, st, f"/tmp/claude-0/pad/{style}_{theme}.png")
    # combine
    ims = [Image.open(f"/tmp/claude-0/pad/{s}_{t}.png") for s in ("ps4", "xbox") for t in ("dark", "light")]
    W = ims[0].width; H = ims[0].height
    sheet = Image.new("RGBA", (W * 2, H * 2))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % 2) * W, (i // 2) * H))
    sheet.save("/tmp/claude-0/pad/sheet.png")
    print("ok")
