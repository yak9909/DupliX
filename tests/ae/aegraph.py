# AE classic 3D's ordering graph rebuilt from a plan dump (RANDAMU_MERGE_DUMP): nodes = copies + other planes;
# for two nodes whose screen quads overlap, the depth difference at the overlap polygon's vertices gives
# "a in front somewhere" / "b in front somewhere" -> edges; then strongly connected components (Tarjan).
#   python aegraph.py plan.txt [copy ...]   (prints the SCCs with more than one node, and where the copies are)
import sys
import numpy as np

sys.setrecursionlimit(100000)
plan = sys.argv[1]
watch = [int(v) for v in sys.argv[2:]]
nodes = []  # (name, screen quad 4x2, world corners 4x3)
for line in open(plan):
    t = line.split()
    if t[0] == "cam":
        dist, W, H = float(t[2]), float(t[4]), float(t[6])
        M = np.array([float(v) for v in t[8:24]]).reshape(4, 4)
    elif t[0] == "plane":
        rect = [float(v) for v in t[4:8]]
        m = np.array([float(v) for v in t[9:25]]).reshape(4, 4)
        pts = [(rect[0], rect[1]), (rect[2], rect[1]), (rect[2], rect[3]), (rect[0], rect[3])]
        w = np.array([[x * m[0, j] + y * m[1, j] + m[3, j] for j in range(3)] for x, y in pts])
        nodes.append(("plane", None, w))
    elif t[0] == "copy":
        q = np.array([float(v) for v in t[9:17]]).reshape(4, 2)
        w = np.array([float(v) for v in t[18:30]]).reshape(4, 3)
        nodes.append((f"copy{t[2]}", q, w))
# camera: world -> camera space (rows of M are scaled axes)
R = M[:3, :3].copy()
sc = np.linalg.norm(R, axis=1)
Rn = R / sc[:, None]
o = M[3, :3]
def to_screen(p):
    c = Rn @ (p - o)
    return np.array([c[0] * dist / c[2] + W / 2, c[1] * dist / c[2] + H / 2]), c[2]
for i, (n, q, w) in enumerate(nodes):
    if q is None:
        nodes[i] = (n, np.array([to_screen(p)[0] for p in w]), w)
def depth_at(w, x, y):
    # camera z where the ray through screen (x, y) meets the plane of w
    d = np.array([x - W / 2, y - H / 2, dist])
    nrm = np.cross(w[1] - w[0], w[3] - w[0])
    nc = Rn @ nrm
    p0 = Rn @ (w[0] - o)
    t = np.dot(nc, p0) / np.dot(nc, d)
    return t * dist
def clip(subject, clipper):
    # convex polygon intersection (Sutherland-Hodgman); both in screen px
    def inside(p, a, b, s):
        return s * ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) >= 0
    area = sum(clipper[k][0] * clipper[(k + 1) % 4][1] - clipper[(k + 1) % 4][0] * clipper[k][1] for k in range(4))
    s = 1 if area > 0 else -1
    out = list(subject)
    for k in range(len(clipper)):
        a, b = clipper[k], clipper[(k + 1) % len(clipper)]
        inp, out = out, []
        if not inp: break
        prev = inp[-1]
        for cur in inp:
            if inside(cur, a, b, s):
                if not inside(prev, a, b, s):
                    out.append(inter(prev, cur, a, b))
                out.append(cur)
            elif inside(prev, a, b, s):
                out.append(inter(prev, cur, a, b))
            prev = cur
    return out
def inter(p, q, a, b):
    d1 = np.array(q) - np.array(p); d2 = np.array(b) - np.array(a)
    den = d1[0] * d2[1] - d1[1] * d2[0]
    if den == 0: return p
    t = ((a[0] - p[0]) * d2[1] - (a[1] - p[1]) * d2[0]) / den
    return (p[0] + t * d1[0], p[1] + t * d1[1])
n = len(nodes)
bb = [(q[:, 0].min(), q[:, 1].min(), q[:, 0].max(), q[:, 1].max()) for _, q, _ in nodes]
adj = [[] for _ in range(n)]
for i in range(n):
    for j in range(i + 1, n):
        if bb[i][2] <= bb[j][0] or bb[j][2] <= bb[i][0] or bb[i][3] <= bb[j][1] or bb[j][3] <= bb[i][1]:
            continue
        poly = clip([tuple(p) for p in nodes[i][1]], [tuple(p) for p in nodes[j][1]])
        if len(poly) < 3: continue
        a_front = b_front = False
        for (x, y) in poly:
            di = depth_at(nodes[i][2], x, y); dj = depth_at(nodes[j][2], x, y)
            if di < dj - 1e-6: a_front = True
            if dj < di - 1e-6: b_front = True
        # an edge "x before y" = x is behind y somewhere (x must be drawn first)
        if b_front: adj[i].append(j)
        if a_front: adj[j].append(i)
# Tarjan
index = [None] * n; low = [0] * n; onst = [False] * n; st = []; comps = []; cnt = [0]
def strong(v):
    index[v] = low[v] = cnt[0]; cnt[0] += 1; st.append(v); onst[v] = True
    for w in adj[v]:
        if index[w] is None:
            strong(w); low[v] = min(low[v], low[w])
        elif onst[w]:
            low[v] = min(low[v], index[w])
    if low[v] == index[v]:
        c = []
        while True:
            w = st.pop(); onst[w] = False; c.append(w)
            if w == v: break
        comps.append(c)
for v in range(n):
    if index[v] is None: strong(v)
big = [c for c in comps if len(c) > 1]
print(f"nodes {n}, components {len(comps)}, multi-node {len(big)}: sizes {sorted(len(c) for c in big)[::-1][:10]}")
for c in big:
    names = [nodes[k][0] for k in c]
    hit = [w for w in watch if f"copy{w}" in names]
    print(f"  size {len(c)} plane={'plane' in names} watched={hit}")
