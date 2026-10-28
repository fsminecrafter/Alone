#!/usr/bin/env python3
"""Adds forest density and paths from orienteering maps (OCAD PDFs).

    python3 tools/mapgen/omap_import.py <map dir> <map.pdf>... [--debug DIR]

Orienteering maps are drawn by people who have walked every bit of the
forest. They show how thick it is (white: open forest you can run through,
light/medium/dark green: slow, difficult, impenetrable), open land, marsh,
bare rock, and every footpath. This tool:

  1. renders each PDF and finds where it lies on the game map by matching
     its black building shapes against the OpenStreetMap buildings
     (scale, rotation and offset are searched; the maps have no coordinates),
  2. classifies the map colours into land cover and writes them into the 2 m
     terrain grid (terrain2m.pgm), only inside each map's frame,
  3. reads the dashed path symbols from the PDF's vector data, joins the
     dashes into lines and adds paths that OpenStreetMap doesn't have to
     roads_m.txt.

Run osm_import.py first (it rewrites terrain2m.pgm and roads_m.txt).
The PDFs themselves are not part of the repository.
"""
import math
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

# Zone ids, as in src/world/Terrain.h
UNKNOWN, WATER, FOREST, WOODLAND, PARK, RESIDENTIAL, FIELD, MEADOW, COMMERCIAL, HOSPITAL, PAVED, PARKING, \
    FOREST_DENSE, THICKET, MARSH, ROCK = range(16)
OUT_MPP = 2.0
DPI = 300          # rendering; buildings and land cover are read at this resolution
ADPI = 75          # alignment works on a coarser copy
PT = 72.0

# ISOM colours as OCAD exports them (sRGB)
PALETTE = {
    "white": (255, 255, 255),
    "green20": (194, 232, 189),   # slow running
    "green50": (138, 212, 132),   # difficult
    "green100": (61, 185, 77),    # impassable thicket
    "yellow": (250, 185, 55),     # open land
    "yellow50": (252, 216, 145),  # rough open land
    "olive": (161, 166, 34),      # settlement / private
    "paved": (230, 168, 107),
    "blue": (0, 173, 239),
    "blue50": (77, 198, 244),
    "grey": (178, 177, 177),      # bare rock
    "grey2": (211, 210, 210),
    "black": (35, 31, 32),
    "brown": (206, 102, 17),      # contours
    "purple": (219, 42, 102),     # course overprint
}
NAMES = list(PALETTE)
COLS = np.array([PALETTE[n] for n in NAMES], np.float32)


def render(pdf, dpi, cache):
    base = os.path.join(cache, os.path.splitext(os.path.basename(pdf))[0].replace(" ", "_") + f"_{dpi}")
    png = base + ".png"
    if not os.path.exists(png):
        subprocess.run(["pdftoppm", "-r", str(dpi), "-png", "-singlefile", pdf, base], check=True,
                       stderr=subprocess.DEVNULL)
    return np.asarray(Image.open(png).convert("RGB"))


def classify(rgb):
    """Index of the nearest palette colour for every pixel."""
    h, w, _ = rgb.shape
    out = np.empty((h, w), np.uint8)
    flat = rgb.reshape(-1, 3).astype(np.float32)
    step = 400000
    o = out.reshape(-1)
    for i in range(0, flat.shape[0], step):
        d = ((flat[i:i + step, None, :] - COLS[None, :, :]) ** 2).sum(-1)
        o[i:i + step] = d.argmin(1)
    return out


def load_osm_buildings(map_dir):
    """Exact building outlines from the OpenStreetMap extracts, in game metres."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import glob
    import osm_import as oi
    ini = open(os.path.join(map_dir, "map.ini"), encoding="utf-8").read()
    base_mpp = float(re.search(r"^metres_per_pixel\s*=\s*([\d.]+)", ini, re.M).group(1))
    src = sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), "osm", "alingsas", "*.osm*")))
    nodes, ways, rels, bounds = oi.load_osm(src)
    s, tx, ty = oi.align_anchors(os.path.join(map_dir, "anchors.txt"), base_mpp, nodes, ways, rels)
    polys = []
    for tags, outers, inners in oi.areas(nodes, ways, rels):
        if "building" not in tags or not outers:
            continue
        for ring in outers:
            xy = oi.way_xy(nodes, ring)
            if len(xy) >= 3:
                polys.append([(p[0] * s + tx, p[1] * s + ty) for p in xy])
    return polys


def world_raster(polys, world_w, world_h, res):
    W, H = int(math.ceil(world_w / res)), int(math.ceil(world_h / res))
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    for poly in polys:
        d.polygon([(x / res, y / res) for x, y in poly], fill=255)
    return np.asarray(img, np.float32) / 255.0


def omap_buildings(cls, dpi=ADPI):
    """Houses: solid black areas at least ~0.6 mm across. Opening with a
    7 px square (0.6 mm at 300 dpi) removes path dashes, cliff lines,
    boulders and lettering."""
    black = (cls == NAMES.index("black")).astype(np.uint8) * 255
    img = Image.fromarray(black).filter(ImageFilter.MinFilter(7)).filter(ImageFilter.MaxFilter(7))
    small = img.resize((img.width * dpi // DPI, img.height * dpi // DPI), Image.BOX)
    return np.asarray(small, np.float32) / 255.0


def place(mask, s, theta, res, dpi=None):
    """The mask turned and scaled into world orientation (no offset).
    Returns the image and the world position of its top-left pixel."""
    h, w = mask.shape
    if dpi is None:
        dpi = ADPI if res >= 4 else ADPI * 2
    mpp_px = s * PT / dpi            # metres per pixel of the alignment copy
    c, sn = math.cos(theta), math.sin(theta)
    corners = [(0, 0), (w, 0), (0, h), (w, h)]
    wx = [mpp_px * (c * u - sn * v) for u, v in corners]
    wy = [mpp_px * (sn * u + c * v) for u, v in corners]
    x0, y0 = min(wx), min(wy)
    W, H = int((max(wx) - x0) / res) + 1, int((max(wy) - y0) / res) + 1
    # output pixel (X, Y) -> world (x0 + X res, y0 + Y res) -> omap pixel
    k = res / mpp_px
    a = c * k; b = sn * k
    cc = (c * x0 + sn * y0) / mpp_px
    d = -sn * k; e = c * k
    f = (-sn * x0 + c * y0) / mpp_px
    img = Image.fromarray((mask * 255).astype(np.uint8)).transform((W, H), Image.AFFINE, (a, b, cc, d, e, f),
                                                                   resample=Image.BILINEAR)
    return np.asarray(img, np.float32) / 255.0, (x0, y0)


def correlate(B, A):
    """Best offset of A over B. Both are zero-padded so nothing wraps round;
    offsets that would leave the map's middle outside the world are ignored."""
    H, W = B.shape
    h, w = A.shape
    PH, PW = H + h, W + w
    Bp = np.zeros((PH, PW), np.float32)
    Bp[:H, :W] = B
    Ap = np.zeros((PH, PW), np.float32)
    Ap[:h, :w] = A
    corr = np.fft.irfft2(np.fft.rfft2(Bp) * np.conj(np.fft.rfft2(Ap)), s=(PH, PW))
    # offset d (A's origin at B pixel d, d may be negative: wraps to the end)
    ys = np.arange(PH); ys = np.where(ys > H, ys - PH, ys)
    xs = np.arange(PW); xs = np.where(xs > W, xs - PW, xs)
    oky = ((ys + h / 2) > 0) & ((ys + h / 2) < H)
    okx = ((xs + w / 2) > 0) & ((xs + w / 2) < W)
    corr[~oky, :] = -1
    corr[:, ~okx] = -1
    iy, ix = np.unravel_index(np.argmax(corr), corr.shape)
    return corr[iy, ix], xs[ix], ys[iy]


def refine(mask, B2, s0, a0, tx0, ty0, res=2.0, reach=80.0):
    """Fine search near a known answer, at 2 m."""
    best = None
    for s in [s0 * f for f in np.linspace(0.985, 1.015, 7)]:
        for th in np.arange(a0 - 0.75, a0 + 0.76, 0.25):
            A, (x0, y0) = place(mask, s, math.radians(th), res)
            # offsets (in B pixels) around the expected one
            ex, ey = (tx0 + x0) / res, (ty0 + y0) / res
            r = int(reach / res)
            H, W = B2.shape
            h, w = A.shape
            best_here = None
            # cut the part of B that the search can touch
            X0, Y0 = int(ex) - r, int(ey) - r
            pad = np.zeros((h + 2 * r + 1, w + 2 * r + 1), np.float32)
            sx0, sy0 = max(0, X0), max(0, Y0)
            sx1, sy1 = min(W, X0 + pad.shape[1]), min(H, Y0 + pad.shape[0])
            if sx1 <= sx0 or sy1 <= sy0:
                continue
            pad[sy0 - Y0:sy1 - Y0, sx0 - X0:sx1 - X0] = B2[sy0:sy1, sx0:sx1]
            PH, PW = pad.shape
            Ap = np.zeros((PH, PW), np.float32)
            Ap[:h, :w] = A
            corr = np.fft.irfft2(np.fft.rfft2(pad) * np.conj(np.fft.rfft2(Ap)), s=(PH, PW))[:2 * r + 1, :2 * r + 1]
            iy, ix = np.unravel_index(np.argmax(corr), corr.shape)
            score = corr[iy, ix] / math.sqrt(A.sum() + 1)
            if best is None or score > best[0]:
                best = (score, s, th, (X0 + ix) * res - x0, (Y0 + iy) * res - y0)
    return best


def georeference(mask, B, res, scales, angles):
    best = None
    for s in scales:
        for th in angles:
            A, (x0, y0) = place(mask, s, math.radians(th), res)
            if A.shape[0] >= B.shape[0] or A.shape[1] >= B.shape[1]:
                continue
            peak, ix, iy = correlate(B, A)
            score = peak / math.sqrt(A.sum() + 1)
            if best is None or score > best[0]:
                best = (score, s, th, ix * res - x0, iy * res - y0)
    return best


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    debug = None
    if "--debug" in sys.argv:
        debug = sys.argv[sys.argv.index("--debug") + 1]
        args = [a for a in args if a != debug]
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)
    hint_scale = hint_angle = None
    for flag in ("--scale", "--angle"):
        if flag in sys.argv:
            v = sys.argv[sys.argv.index(flag) + 1]
            args = [a for a in args if a != v]
            if flag == "--scale": hint_scale = float(v)
            else: hint_angle = float(v)
    if hint_angle is None: hint_angle = 0.0
    map_dir, pdfs = args[0], args[1:]
    # Rendered pages are cached between runs in --cache DIR, otherwise in a
    # temporary folder that is removed again at the end
    tmp = None
    if "--cache" in sys.argv:
        cache = sys.argv[sys.argv.index("--cache") + 1]
        pdfs = [p for p in pdfs if p != cache]
        os.makedirs(cache, exist_ok=True)
    else:
        tmp = tempfile.TemporaryDirectory(prefix="omap_")
        cache = tmp.name
    # The game map
    with open(os.path.join(map_dir, "terrain2m.pgm"), "rb") as f:
        raw = f.read()
    m = re.match(rb"P5\s+(\d+)\s+(\d+)\s+(\d+)\s", raw)
    tw, th = int(m.group(1)), int(m.group(2))
    zones = np.frombuffer(raw[m.end():m.end() + tw * th], np.uint8).reshape(th, tw).copy()
    world_w, world_h = tw * OUT_MPP, th * OUT_MPP
    polys = load_osm_buildings(map_dir)
    B4 = world_raster(polys, world_w, world_h, 4.0)
    B2 = world_raster(polys, world_w, world_h, 2.0)
    fits = []
    print(f"World {world_w:.0f} x {world_h:.0f} m, {len(polys)} OSM buildings")

    for pdf in pdfs:
        name = os.path.basename(pdf)
        rgb = render(pdf, DPI, cache)
        fitfile = os.path.join(cache, name + ".fit")
        cls = classify(rgb)
        if os.path.exists(fitfile) and os.path.getmtime(fitfile) > os.path.getmtime(pdf):
            s2, ang2, tx2, ty2 = map(float, open(fitfile).read().split())
            print(f"{name}: scale 1:{s2 / 0.0254 * 72:.0f}, turned {ang2:+.2f} deg, offset ({tx2:.0f}, {ty2:.0f}) m (cached)")
            fits.append((pdf, name, rgb, cls, s2, ang2, tx2, ty2))
            continue
        mask = omap_buildings(cls)
        mask_fine = omap_buildings(cls, ADPI * 2)
        # Coarse search: map scale 1:7500 .. 1:15000, a few degrees of turn (magnetic
        # north), unless --scale/--angle say what it is (maps from the same event share them)
        m_per_pt = lambda denom: denom * 0.0254 / 72.0
        if hint_scale:
            coarse = [m_per_pt(hint_scale * f) for f in (0.98, 1.0, 1.02)]
            angles = np.arange(hint_angle - 1.5, hint_angle + 1.51, 0.5)
        else:
            coarse = [m_per_pt(d) for d in (7500, 9000, 10000, 11000, 12500, 15000)]
            angles = range(-9, 10, 2)
        best = georeference(mask, B4, 4.0, coarse, angles)
        score, s, ang, tx, ty = best
        # Refine
        fine_s = [s * f for f in np.linspace(0.97, 1.03, 7)] if hint_scale else [s * f for f in np.linspace(0.94, 1.06, 13)]
        best2 = georeference(mask, B4, 4.0, fine_s, np.arange(ang - 1.0, ang + 1.01, 0.25) if hint_scale else np.arange(ang - 2, ang + 2.01, 0.5))
        score2, s2, ang2, tx2, ty2 = best2
        score3, s2, ang2, tx2, ty2 = refine(mask_fine, B2, s2, ang2, tx2, ty2)
        print(f"{name}: scale 1:{s2 / 0.0254 * 72:.0f}, turned {ang2:+.2f} deg, offset ({tx2:.0f}, {ty2:.0f}) m, "
              f"match {score3:.2f} (coarse {score:.2f})")
        fits.append((pdf, name, rgb, cls, s2, ang2, tx2, ty2))
        with open(fitfile, "w") as f:
            f.write(f"{s2} {ang2} {tx2} {ty2}\n")
        if debug:
            os.makedirs(debug, exist_ok=True)
            A, (x0, y0) = place(mask, s2, math.radians(ang2), 4.0)
            over = np.stack([B4 * 255, np.zeros_like(B4), np.zeros_like(B4)], -1)
            oy, ox = int((ty2 + y0) / 4), int((tx2 + x0) / 4)
            hh, ww = A.shape
            sub = over[max(0, oy):oy + hh, max(0, ox):ox + ww]
            sub[..., 1] = np.maximum(sub[..., 1], A[:sub.shape[0], :sub.shape[1]] * 255)
            Image.fromarray(over.astype(np.uint8)).save(os.path.join(debug, name + "_align.png"))

    for fit in fits:
        print(fit[1])
        zones = land_cover(zones, fit)
    with open(os.path.join(map_dir, "terrain2m.pgm"), "wb") as f:
        f.write(f"P5\n{tw} {th}\n255\n".encode())
        f.write(zones.tobytes())
    Image.fromarray((zones * 16).astype(np.uint8)).save(os.path.join(map_dir, "preview_osm.png"))
    add_paths(map_dir, fits, cache, world_w, world_h)
    if tmp:
        tmp.cleanup()


def to_world(s, ang, tx, ty):
    """Affine from O-map page points to world metres: world = s R(ang) pt + t."""
    c, sn = math.cos(math.radians(ang)), math.sin(math.radians(ang))
    return lambda u, v: (s * (c * u - sn * v) + tx, s * (sn * u + c * v) + ty)


def warp_fraction(frac, s, ang, tx, ty, tw, th):
    """A per-pixel map (O-map raster at DPI) resampled onto the world's 2 m grid."""
    c, sn = math.cos(math.radians(ang)), math.sin(math.radians(ang))
    k = DPI / PT / s      # O-map pixels per metre
    # world cell (X, Y) centre -> world (2X+1, 2Y+1) -> page pt = R(-a)(w - t)/s -> pixel
    a = c * OUT_MPP * k;  b = sn * OUT_MPP * k
    d = -sn * OUT_MPP * k; e = c * OUT_MPP * k
    ox, oy = OUT_MPP * 0.5 - tx, OUT_MPP * 0.5 - ty
    cc = (c * ox + sn * oy) * k
    f = (-sn * ox + c * oy) * k
    img = Image.fromarray((np.clip(frac, 0, 1) * 255).astype(np.uint8))
    out = img.transform((tw, th), Image.AFFINE, (a, b, cc, d, e, f), resample=Image.BILINEAR, fillcolor=0)
    return np.asarray(out, np.float32) / 255.0


def land_cover(zones, fit):
    pdf, name, rgb, cls, s, ang, tx, ty = fit
    th, tw = zones.shape
    # The purple course overprint (circles, lines, control numbers) hides
    # what's underneath: leave it out and judge from the rest
    seen = (cls != NAMES.index("purple")).astype(np.uint8) * 255
    def frac(names, radius, normalise=True):
        m = np.isin(cls, [NAMES.index(n) for n in names]).astype(np.uint8) * 255
        img = np.asarray(Image.fromarray(m).filter(ImageFilter.BoxBlur(radius)), np.float32)
        if not normalise:
            return img / 255.0
        cover = np.asarray(Image.fromarray(seen).filter(ImageFilter.BoxBlur(radius)), np.float32)
        return np.where(cover > 60, img / np.maximum(cover, 1), 0)
    r = 3   # ~2.5 m either side at 1:10000
    F = {k: warp_fraction(frac(v, r), s, ang, tx, ty, tw, th) for k, v in {
        "open": ["white"], "dense": ["green20"], "thicket": ["green50", "green100"],
        "field": ["yellow"], "rough": ["yellow50"], "rock": ["grey"], "water": ["blue", "blue50"],
        "town": ["olive", "paved", "black"],
    }.items()}
    # The mapped area: where there's relief, vegetation or open land nearby (not the white margins)
    content = warp_fraction(frac(["brown", "green20", "green50", "green100", "yellow", "yellow50", "olive", "grey"], 45, False),
                            s, ang, tx, ty, tw, th) > 0.03
    known = sum(F[k] for k in F)
    content &= known > 0.45   # mostly overprint: keep what was there
    stack = np.stack([F["open"], F["dense"], F["thicket"], F["field"], F["rough"], F["rock"], F["water"], F["town"]])
    best = stack.argmax(0)
    out = zones.copy()
    built = np.isin(zones, [RESIDENTIAL, COMMERCIAL, HOSPITAL, PAVED, PARKING, PARK])
    wild = content & ~built & (F["town"] < 0.35)
    marsh = (F["water"] > 0.10) & (F["water"] < 0.55) & (best <= 2)
    put = lambda cond, z: np.copyto(out, np.uint8(z), where=cond)
    put(wild & (best == 0), FOREST)
    put(wild & (best == 1), FOREST_DENSE)
    put(wild & (best == 2), THICKET)
    put(wild & (best == 3) & ~np.isin(zones, [FIELD]), MEADOW)
    put(wild & (best == 4), MEADOW)
    put(wild & (best == 5), ROCK)
    put(wild & marsh, MARSH)
    put(content & (F["water"] > 0.6) & ~built, WATER)
    changed = (out != zones).sum()
    counts = {n: int(((out == z) & content).sum()) for n, z in
              (("forest", FOREST), ("dense", FOREST_DENSE), ("thicket", THICKET), ("marsh", MARSH), ("rock", ROCK))}
    print(f"  land cover: {changed * OUT_MPP * OUT_MPP / 1e6:.2f} km2 changed, " +
          ", ".join(f"{k} {v * 4 / 1e4:.0f} ha" for k, v in counts.items()))
    return out


def svg_paths(pdf, cache):
    """Dashed path symbols from the PDF's vector data, as page-point polylines.
    OCAD writes every dash as its own little line; they are joined again here."""
    base = os.path.join(cache, os.path.splitext(os.path.basename(pdf))[0].replace(" ", "_") + ".svg")
    if not os.path.exists(base):
        subprocess.run(["pdftocairo", "-svg", pdf, base], check=True, stderr=subprocess.DEVNULL)
    text = open(base, encoding="utf-8", errors="replace").read()
    segs = []   # (kind, [(x, y)...])
    widths = {0.77: 5, 1.06: 5, 1.49: 7}   # small path, footpath, vehicle track
    for m in re.finditer(r"<path ([^>]*)/>", text):
        a = m.group(1)
        if 'stroke="rgb(13.729858%, 12.159729%, 12.548828%)"' not in a or 'fill="none"' not in a:
            continue
        w = re.search(r'stroke-width="([\d.]+)"', a)
        if not w:
            continue
        kind = None
        for ref, k in widths.items():
            if abs(float(w.group(1)) - ref) < 0.03:
                kind = k
        if kind is None:
            continue
        nums = list(map(float, re.findall(r"-?[\d.]+", re.search(r' d="([^"]*)"', a).group(1))))
        pts = list(zip(nums[0::2], nums[1::2]))
        t = re.search(r'transform="matrix\(([^)]*)\)"', a)
        if t:
            ma, mb, mc, md, me, mf = map(float, t.group(1).split(","))
            pts = [(ma * x + mc * y + me, mb * x + md * y + mf) for x, y in pts]
        if len(pts) >= 2:
            segs.append((kind, pts))
    return join_dashes(segs)


def join_dashes(segs, gap=2.6):
    """Link dashes end to end where the next one carries on in about the same direction."""
    ends = {}
    cell = gap
    def key(p):
        return (int(p[0] // cell), int(p[1] // cell))
    def direction(pts, at_end):
        a, b = (pts[-2], pts[-1]) if at_end else (pts[1], pts[0])
        dx, dy = b[0] - a[0], b[1] - a[1]
        l = math.hypot(dx, dy) or 1
        return dx / l, dy / l
    for i, (k, pts) in enumerate(segs):
        for e in (0, 1):
            p = pts[-1] if e else pts[0]
            ends.setdefault(key(p), []).append((i, e))
    link = {}
    for i, (k, pts) in enumerate(segs):
        for e in (0, 1):
            p = pts[-1] if e else pts[0]
            out = direction(pts, e == 1)
            best, bestScore = None, 0.6
            kx, ky = key(p)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for j, f in ends.get((kx + dx, ky + dy), []):
                        if j == i or segs[j][0] != k:
                            continue
                        q = segs[j][1][-1] if f else segs[j][1][0]
                        dist = math.dist(p, q)
                        if dist > gap:
                            continue
                        into = direction(segs[j][1], f == 1)   # pointing out of j at that end
                        align = -(out[0] * into[0] + out[1] * into[1])
                        score = align - dist / gap * 0.3
                        if score > bestScore:
                            best, bestScore = (j, f), score
            if best:
                link[(i, e)] = best
    # keep mutual links only, then walk the chains
    used = set()
    lines = []
    for i in range(len(segs)):
        if i in used:
            continue
        # walk back to the start of the chain
        start, se = i, 0
        seen = {i}
        while True:
            nb = link.get((start, se))
            if not nb or link.get(nb) != (start, se) or nb[0] in seen:
                break
            start, se = nb[0], 1 - nb[1]
            seen.add(start)
        # walk forward
        chain, cur, ce = [], start, 1 - se
        pts = list(segs[cur][1]) if ce == 1 else list(reversed(segs[cur][1]))
        used.add(cur)
        count = 1
        while True:
            nb = link.get((cur, ce))
            if not nb or link.get(nb) != (cur, ce) or nb[0] in used:
                break
            j, f = nb
            nxt = segs[j][1] if f == 0 else list(reversed(segs[j][1]))
            pts.extend(nxt)
            used.add(j)
            cur, ce = j, 1 - f
            count += 1
        length = sum(math.dist(pts[t], pts[t + 1]) for t in range(len(pts) - 1))
        if count >= 3 and length > 18:
            lines.append((segs[i][0], simplify(pts, 0.4)))
    return lines


def simplify(pts, tol):
    if len(pts) < 3:
        return pts
    a, b = pts[0], pts[-1]
    dx, dy = b[0] - a[0], b[1] - a[1]
    l = math.hypot(dx, dy) or 1e-9
    worst, wi = 0, 0
    for i in range(1, len(pts) - 1):
        d = abs(dy * pts[i][0] - dx * pts[i][1] + b[0] * a[1] - b[1] * a[0]) / l
        if d > worst:
            worst, wi = d, i
    if worst <= tol:
        return [a, b]
    return simplify(pts[:wi + 1], tol)[:-1] + simplify(pts[wi:], tol)


def add_paths(map_dir, fits, cache, world_w, world_h):
    path = os.path.join(map_dir, "roads_m.txt")
    lines = [l for l in open(path, encoding="utf-8") if not l.startswith("# omap")]
    # where roads and paths already are (OSM)
    res = 2.0
    have = Image.new("L", (int(world_w / res) + 1, int(world_h / res) + 1), 0)
    d = ImageDraw.Draw(have)
    for l in lines:
        if l.startswith("#"):
            continue
        t = l.split()
        nums = list(map(float, t[2:]))
        pts = [(nums[i] / res, nums[i + 1] / res) for i in range(0, len(nums) - 1, 2)]
        if len(pts) >= 2:
            d.line(pts, fill=255, width=5)
    have = np.asarray(have.filter(ImageFilter.MaxFilter(3)))
    added, km = 0, 0.0
    out = []
    for fit in fits:
        pdf, name, rgb, cls, s, ang, tx, ty = fit
        W = to_world(s, ang, tx, ty)
        for kind, pts in svg_paths(pdf, cache):
            wpts = [W(x, y) for x, y in pts]
            # split where it runs along a path OSM already has, or leaves the world
            piece = []
            def flush():
                nonlocal added, km
                if len(piece) >= 2:
                    L = sum(math.dist(piece[i], piece[i + 1]) for i in range(len(piece) - 1))
                    if L > 25:
                        out.append(f"{kind} - " + " ".join(f"{x:.1f} {y:.1f}" for x, y in piece) + "\n")
                        added += 1
                        km += L / 1000
            # densify so the check sees every few metres
            dense = []
            for i in range(len(wpts) - 1):
                a, b = wpts[i], wpts[i + 1]
                n = max(1, int(math.dist(a, b) / 3))
                for j in range(n):
                    dense.append((a[0] + (b[0] - a[0]) * j / n, a[1] + (b[1] - a[1]) * j / n))
            dense.append(wpts[-1])
            for p in dense:
                ix, iy = int(p[0] / res), int(p[1] / res)
                inside = 0 <= ix < have.shape[1] and 0 <= iy < have.shape[0]
                if inside and not have[iy, ix]:
                    piece.append(p)
                else:
                    flush()
                    piece = []
            flush()
    with open(path, "w", encoding="utf-8") as f:
        f.writelines(lines)
        f.write("# omap: paths from orienteering maps (not in OpenStreetMap)\n")
        f.writelines(out)
    print(f"Paths: {added} new paths, {km:.1f} km, from the orienteering maps")


if __name__ == "__main__":
    main()
