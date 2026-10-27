#!/usr/bin/env python3
"""
Digitize a reference map screenshot into game map data for Alone.

Reads a map image (e.g. a screenshot of Alingsås), classifies every pixel by
colour into terrain classes, separates the road network and vectorises it into
polylines, and writes:

  terrain.pgm   8-bit binary PGM (read by the game), same as terrain.png
  terrain.png   8-bit grayscale, one terrain class id per pixel (see TERRAIN)
  roads.txt     one polyline per line: "<class> x0 y0 x1 y1 ..." in pixel coords
  debug.png     colour preview of the classification (not used by the game)

The game scales pixel coordinates to metres using `metres_per_pixel` from the
map's map.ini, so the image itself is never needed at runtime.

Usage:
  python3 tools/mapgen/digitize.py <screenshot.png> <out_dir>

Requires: pillow, numpy, scipy, scikit-image
"""
import sys
import os
import numpy as np
from PIL import Image
from scipy import ndimage
from skimage.morphology import skeletonize


def remove_small_objects(mask, min_size):
    """Drop connected blobs smaller than min_size pixels (8-connected)."""
    lab, n = ndimage.label(mask, np.ones((3, 3)))
    if n == 0:
        return mask
    sizes = ndimage.sum(mask, lab, range(1, n + 1))
    keep = np.zeros(n + 1, bool)
    keep[1:] = sizes >= min_size
    return keep[lab]

# Terrain class ids. Must match alone::Terrain in src/world/Terrain.h
UNKNOWN, WATER, FOREST, WOODLAND, PARK, RESIDENTIAL, FIELD, MEADOW, COMMERCIAL, HOSPITAL = range(10)

# Road class ids. Must match alone::RoadClass in src/world/MapData.h
ROAD_MINOR, ROAD_MAJOR, ROAD_MOTORWAY = 1, 2, 3

PALETTE = {
    FOREST:      [(205, 240, 218), (204, 239, 217), (204, 239, 218), (208, 241, 221)],
    WOODLAND:    [(211, 248, 224), (229, 246, 235)],
    PARK:        [(189, 240, 207), (195, 241, 212), (191, 240, 209), (191, 233, 220)],
    RESIDENTIAL: [(245, 244, 244), (242, 242, 242), (237, 236, 237), (244, 242, 241),
                  (244, 241, 237), (230, 230, 231)],
    FIELD:       [(236, 232, 223), (235, 231, 222)],
    MEADOW:      [(244, 240, 229), (244, 241, 233)],
    COMMERCIAL:  [(248, 240, 223)],
    HOSPITAL:    [(252, 232, 229)],
}
ROAD_PALETTE = {
    ROAD_MOTORWAY: [(147, 170, 198), (148, 169, 195)],
    ROAD_MAJOR:    [(207, 216, 227), (201, 211, 220), (204, 215, 224), (208, 217, 227),
                    (209, 218, 227), (201, 212, 220), (207, 217, 227)],
    ROAD_MINOR:    [(255, 255, 255), (253, 253, 253), (252, 252, 252)],
}
# Screen-space UI of the screenshot (buttons, logo, attribution) to ignore.
UI_RECTS = [
    (0, 355, 28, 412),       # side panel arrow
    (18, 660, 108, 750),     # layer thumbnail
    (1260, 12, 1352, 52),    # app grid + avatar
    (1308, 612, 1352, 752),  # zoom / location / street view buttons
    (620, 722, 745, 750),    # logo
    (790, 750, 1365, 767),   # attribution bar
]

DEBUG_COLOURS = {
    UNKNOWN: (255, 0, 255), WATER: (60, 120, 200), FOREST: (40, 110, 50),
    WOODLAND: (90, 160, 80), PARK: (120, 200, 100), RESIDENTIAL: (200, 200, 200),
    FIELD: (200, 180, 110), MEADOW: (170, 200, 120), COMMERCIAL: (210, 170, 150),
    HOSPITAL: (240, 140, 140),
}


def nearest_class(img, palette, max_dist):
    """Return per-pixel class from palette (0 where nothing is close enough)."""
    h, w, _ = img.shape
    best = np.full((h, w), 1e9)
    out = np.zeros((h, w), np.uint8)
    for cls, cols in palette.items():
        for c in cols:
            d = np.sqrt(((img - np.array(c, np.float32)) ** 2).sum(axis=2))
            better = d < best
            best[better] = d[better]
            out[better] = cls
    out[best > max_dist] = 0
    return out


def fill_unknown(cls, iterations=40):
    """Grow known classes into unknown pixels (labels, icons, border dots)."""
    cls = cls.copy()
    for _ in range(iterations):
        unknown = cls == UNKNOWN
        if not unknown.any():
            break
        # majority vote of the 3x3 neighbourhood, ignoring unknown
        votes = np.zeros((10,) + cls.shape, np.int32)
        for k in range(1, 10):
            votes[k] = ndimage.convolve((cls == k).astype(np.int32), np.ones((3, 3), np.int32), mode="nearest")
        winner = votes.argmax(axis=0).astype(np.uint8)
        has_vote = votes.max(axis=0) > 0
        upd = unknown & has_vote
        cls[upd] = winner[upd]
    return cls


def trace_polylines(skel):
    """Turn a 1-px skeleton into polylines by walking from endpoints/junctions."""
    h, w = skel.shape
    nb = ndimage.convolve(skel.astype(np.int32), np.ones((3, 3), np.int32), mode="constant") - 1
    nb[~skel] = 0
    visited_edges = set()
    offsets = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]

    def neighbours(y, x):
        for dy, dx in offsets:
            ny, nx = y + dy, x + dx
            if 0 <= ny < h and 0 <= nx < w and skel[ny, nx]:
                yield ny, nx

    nodes = set(zip(*np.where(skel & (nb != 2))))
    lines = []

    def walk(start, nxt):
        path = [start, nxt]
        visited_edges.add((start, nxt)); visited_edges.add((nxt, start))
        prev, cur = start, nxt
        while cur not in nodes:
            step = None
            for n in neighbours(*cur):
                if n != prev and (cur, n) not in visited_edges:
                    step = n
                    break
            if step is None:
                break
            visited_edges.add((cur, step)); visited_edges.add((step, cur))
            prev, cur = cur, step
            path.append(cur)
        return path

    for n in nodes:
        for m in neighbours(*n):
            if (n, m) not in visited_edges:
                lines.append(walk(n, m))
    # closed loops without nodes
    remaining = skel.copy()
    for line in lines:
        for y, x in line:
            remaining[y, x] = False
    for y, x in zip(*np.where(remaining)):
        if remaining[y, x]:
            loop = [(y, x)]
            remaining[y, x] = False
            cur = (y, x)
            while True:
                nxt = next((n for n in neighbours(*cur) if remaining[n]), None)
                if nxt is None:
                    break
                remaining[nxt] = False
                loop.append(nxt)
                cur = nxt
            if len(loop) > 3:
                lines.append(loop + [loop[0]])
    return lines


def rdp(points, eps):
    """Ramer-Douglas-Peucker simplification."""
    if len(points) < 3:
        return points
    a, b = np.array(points[0], float), np.array(points[-1], float)
    ab = b - a
    n = np.hypot(*ab)
    best_d, best_i = 0.0, 0
    for i in range(1, len(points) - 1):
        p = np.array(points[i], float)
        if n == 0:
            d = np.hypot(*(p - a))
        else:
            d = abs(ab[0] * (a[1] - p[1]) - ab[1] * (a[0] - p[0])) / n
        if d > best_d:
            best_d, best_i = d, i
    if best_d > eps:
        return rdp(points[:best_i + 1], eps)[:-1] + rdp(points[best_i:], eps)
    return [points[0], points[-1]]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    src, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    img = np.asarray(Image.open(src).convert("RGB")).astype(np.float32)
    r, g, b = img[..., 0], img[..., 1], img[..., 2]

    h, w = r.shape

    # --- labels, icons, shields and map UI: treated as "unknown" ---------------
    mx = img.max(axis=2)
    water_like = (b - r > 45) & (g - r > 45) & (r > 100)
    red = (r > 200) & (g < 140) & (b < 140)
    label_seed = (
        (mx < 130)                                   # dark text
        | ((r > 200) & (g < 150) & (b > 150))        # pink POI icons/text
        | ((g - b > 50) & (mx < 200))                # green road shields
        | ((g - r > 60) & (mx < 180))                # green park labels
        | ((b - r > 80) & (r < 100))                 # blue road shields
        | ((r > 200) & (g < 90) & (b < 90))          # red POI icons
    )
    labels = ndimage.binary_dilation(label_seed, np.ones((3, 3)), iterations=3)
    for x0, y0, x1, y1 in UI_RECTS:
        labels[max(0, y0):min(h, y1), max(0, x0):min(w, x1)] = True

    # --- roads --------------------------------------------------------------
    roads = nearest_class(img, ROAD_PALETTE, 7.0)
    roads[labels] = 0

    # --- water: anything clearly blue-cyan ------------------------------------
    water = water_like & ~labels
    # The municipal border (red dots) is drawn over Säveån: bridge the gaps.
    red = (r > 200) & (g < 140) & (b < 140)
    near_water = ndimage.convolve(water.astype(np.int32), np.ones((5, 5), np.int32), mode="constant")
    water |= red & (near_water >= 3)
    water = ndimage.binary_closing(water, np.ones((3, 3)), iterations=1)
    water = remove_small_objects(water, 6)

    # --- land cover ----------------------------------------------------------
    cls = nearest_class(img, PALETTE, 6.0)
    cls[labels] = UNKNOWN
    cls[water] = WATER
    road_px = (roads > 0) & ~water
    cls[road_px] = UNKNOWN          # roads get the land cover around them
    cls = fill_unknown(cls)
    cls[cls == UNKNOWN] = FOREST

    # --- vectorise roads -------------------------------------------------------
    polylines = []
    # The motorway is one long road broken up by shields and labels: take the
    # centre of motorway pixels per column, bridge the gaps, and simplify.
    mw = nearest_class(img, {1: ROAD_PALETTE[ROAD_MOTORWAY]}, 14.0) > 0
    mw = remove_small_objects(mw, 20)
    centre = {}
    prev_y = None
    for x in range(w):
        ys = np.where(mw[:, x])[0]
        if len(ys) == 0:
            continue
        if prev_y is not None:   # follow the main carriageway, not side ramps
            ys = ys[np.abs(ys - prev_y) < 12] if np.any(np.abs(ys - prev_y) < 12) else ys
        prev_y = float(np.median(ys))
        centre[x] = prev_y
    if centre:
        xs = sorted(centre)
        ys = np.array([centre[x] for x in xs])
        k = 9
        smooth = np.convolve(np.pad(ys, k // 2, mode="edge"), np.ones(k) / k, mode="valid")
        pts = [(x, int(round(y))) for x, y in zip(xs, smooth)]
        polylines.append((ROAD_MOTORWAY, rdp(pts, 1.5)))
    for rc in (ROAD_MAJOR, ROAD_MINOR):
        mask = (roads == rc) & ~water
        mask = ndimage.binary_closing(mask, np.ones((3, 3)))
        mask = remove_small_objects(mask, 12)
        skel = skeletonize(mask)
        for line in trace_polylines(skel):
            pts = [(int(x), int(y)) for y, x in line]
            pts = rdp(pts, 0.9)
            length = sum(np.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]) for i in range(len(pts) - 1))
            if length >= 4:
                polylines.append((rc, pts))

    Image.fromarray(cls, "L").save(os.path.join(out_dir, "terrain.png"))
    # Binary PGM: trivial for the game to read without an image library.
    with open(os.path.join(out_dir, "terrain.pgm"), "wb") as f:
        f.write(f"P5\n{cls.shape[1]} {cls.shape[0]}\n255\n".encode())
        f.write(cls.tobytes())
    with open(os.path.join(out_dir, "roads.txt"), "w") as f:
        f.write("# Generated by tools/mapgen/digitize.py - road polylines in map pixels\n")
        f.write("# <class: 1 minor, 2 major, 3 motorway> x0 y0 x1 y1 ...\n")
        for rc, pts in polylines:
            f.write(str(rc) + " " + " ".join(f"{x} {y}" for x, y in pts) + "\n")

    dbg = np.zeros(cls.shape + (3,), np.uint8)
    for k, c in DEBUG_COLOURS.items():
        dbg[cls == k] = c
    from PIL import ImageDraw
    dimg = Image.fromarray(dbg)
    d = ImageDraw.Draw(dimg)
    rc_col = {ROAD_MINOR: (255, 255, 255), ROAD_MAJOR: (255, 230, 0), ROAD_MOTORWAY: (255, 80, 0)}
    for rc, pts in polylines:
        d.line(pts, fill=rc_col[rc], width=1 if rc == ROAD_MINOR else 2)
    dimg.save(os.path.join(out_dir, "debug.png"))
    print(f"terrain {cls.shape[1]}x{cls.shape[0]}, {len(polylines)} road polylines")


if __name__ == "__main__":
    main()
