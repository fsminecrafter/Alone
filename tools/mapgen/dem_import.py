#!/usr/bin/env python3
"""Import a digital elevation model (GeoTIFF) into an Alone map.

    python3 tools/mapgen/dem_import.py data/maps/alingsas tools/mapgen/dem/alingsas_cop30.tif [--preview relief.png]

Made for the Copernicus GLO-30 DEM as downloaded from OpenTopography (a
single-band float32 GeoTIFF in WGS 84 longitude/latitude), but any such file
works. Writes elevation.pgm (16-bit, centimetres above sea level) on an
8 metre grid covering the map, and its keys in map.ini.

The world <-> latitude/longitude transform comes from the geo_* keys in
map.ini, which tools/mapgen/osm_import.py writes. Run this after
osm_import.py and omap_import.py (it reads their terrain zones).

Copernicus is a surface model: it measures the tops of trees and roofs, not
the ground under them. So forests are lowered by an estimate of how high the
radar saw into their canopy, big buildings are cut out and filled in from the
ground around them, and lakes and rivers get one flat surface with banks that
stay above it.
"""
import math
import os
import re
import sys

import numpy as np
from PIL import Image

# Zone ids (must match alone::Zone in src/world/Terrain.h)
(UNKNOWN, WATER, FOREST, WOODLAND, PARK, RESIDENTIAL, FIELD, MEADOW, COMMERCIAL, HOSPITAL, PAVED, PARKING,
 FOREST_DENSE, THICKET, MARSH, ROCK) = range(16)

GRID = 8.0            # metres between elevation samples
# How far above the ground the radar "surface" sits in each kind of land
# cover, in metres (X-band radar sees roughly half way into Swedish conifer
# forest, which is 15-25 m tall)
CANOPY = {FOREST: 9.0, FOREST_DENSE: 10.0, WOODLAND: 5.0, THICKET: 4.0, PARK: 1.5, MARSH: 1.0}


def read_pgm(path):
    data = open(path, "rb").read()
    m = re.match(rb"P5\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    w, h, maxv = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if maxv < 256:
        return np.frombuffer(data[m.end():m.end() + w * h], np.uint8).reshape(h, w).copy()
    return np.frombuffer(data[m.end():m.end() + 2 * w * h], ">u2").reshape(h, w).copy()


def read_geotiff(path):
    """Heights and the lon/lat of the centre of pixel (0, 0) and the pixel size."""
    im = Image.open(path)
    tags = im.tag_v2
    sx, sy = tags[33550][0], tags[33550][1]
    tie = tags[33922]
    i, j, lon, lat = tie[0], tie[1], tie[3], tie[4]
    # GTRasterTypeGeoKey (1025): 1 = the tie point is a pixel's corner, 2 = its centre
    keys = tags.get(34735, ())
    area = True
    for k in range(4, len(keys), 4):
        if keys[k] == 1025:
            area = keys[k + 3] == 1
    lon0 = lon - i * sx + (sx / 2 if area else 0)
    lat0 = lat + j * sy - (sy / 2 if area else 0)
    z = np.array(im, np.float32)
    nodata = tags.get(42113)
    if nodata is not None:
        try:
            z[z == float(nodata)] = np.nan
        except ValueError:
            pass
    z[z < -1000] = np.nan
    return z, lon0, lat0, sx, sy


def bicubic(z, fx, fy):
    """Catmull-Rom sample of z at fractional pixel coordinates."""
    h, w = z.shape
    x0, y0 = np.floor(fx).astype(int), np.floor(fy).astype(int)
    tx, ty = fx - x0, fy - y0

    def weights(t):
        t2, t3 = t * t, t * t * t
        return [(-t3 + 2 * t2 - t) / 2, (3 * t3 - 5 * t2 + 2) / 2, (-3 * t3 + 4 * t2 + t) / 2, (t3 - t2) / 2]
    wx, wy = weights(tx), weights(ty)
    out = np.zeros_like(fx, dtype=np.float64)
    for a in range(4):
        yy = np.clip(y0 - 1 + a, 0, h - 1)
        row = np.zeros_like(out)
        for b in range(4):
            xx = np.clip(x0 - 1 + b, 0, w - 1)
            row += wx[b] * z[yy, xx]
        out += wy[a] * row
    return out


def bilinear(z, fx, fy):
    h, w = z.shape
    x0 = np.clip(np.floor(fx).astype(int), 0, w - 2)
    y0 = np.clip(np.floor(fy).astype(int), 0, h - 2)
    tx, ty = np.clip(fx - x0, 0, 1), np.clip(fy - y0, 0, 1)
    top = z[y0, x0] * (1 - tx) + z[y0, x0 + 1] * tx
    bot = z[y0 + 1, x0] * (1 - tx) + z[y0 + 1, x0 + 1] * tx
    return top * (1 - ty) + bot * ty


def box_blur(a, r):
    """Mean over a (2r+1)^2 window (edges clamped)."""
    if r <= 0:
        return a.copy()
    p = np.pad(a, r, mode="edge").astype(np.float64)
    c = p.cumsum(0).cumsum(1)
    c = np.pad(c, ((1, 0), (1, 0)))
    k = 2 * r + 1
    s = c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    return s / (k * k)


def smooth(a, r, passes=3):
    for _ in range(passes):
        a = box_blur(a, r)
    return a


def min4(a):
    """Minimum of each cell and its four neighbours (edges clamped)."""
    p = np.pad(a, 1, mode="edge")
    return np.minimum.reduce([p[1:-1, 1:-1], p[:-2, 1:-1], p[2:, 1:-1], p[1:-1, :-2], p[1:-1, 2:]])


def fill_holes(z, hole, iters=400):
    """Fill the masked cells by repeatedly averaging their neighbours."""
    z = z.copy()
    known = ~hole
    if not hole.any():
        return z
    z[hole] = np.nanmean(z[known]) if known.any() else 0
    for _ in range(iters):
        p = np.pad(z, 1, mode="edge")
        avg = (p[:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, :-2] + p[1:-1, 2:]) / 4
        z[hole] = avg[hole]
    return z


def main():
    args = [a for a in sys.argv[1:]]
    preview = None
    if "--preview" in args:
        i = args.index("--preview")
        preview = args[i + 1]
        del args[i:i + 2]
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)
    map_dir, tif = args[0], args[1]
    ini_path = os.path.join(map_dir, "map.ini")
    ini = open(ini_path, encoding="utf-8").read()

    def key(k, default=None):
        m = re.search(r"^%s\s*=\s*(\S+)" % re.escape(k), ini, re.M)
        if not m:
            if default is None:
                sys.exit(f"map.ini has no {k} (run osm_import.py first)")
            return default
        return float(m.group(1))
    lat0, lon0 = key("geo_lat0"), key("geo_lon0")
    gs, gtx, gty = key("geo_scale"), key("geo_offset_x"), key("geo_offset_y")
    tmpp = key("terrain_osm_metres_per_pixel")
    tname = re.search(r"^terrain_osm\s*=\s*(\S+)", ini, re.M).group(1)
    zones = read_pgm(os.path.join(map_dir, tname))
    H, W = zones.shape
    world_w, world_h = W * tmpp, H * tmpp

    z, dlon0, dlat0, sx, sy = read_geotiff(tif)
    print(f"DEM: {z.shape[1]}x{z.shape[0]}, {np.nanmin(z):.1f}..{np.nanmax(z):.1f} m, "
          f"lon {dlon0:.4f}..{dlon0 + sx * (z.shape[1] - 1):.4f}, lat {dlat0 - sy * (z.shape[0] - 1):.4f}..{dlat0:.4f}")
    if np.isnan(z).any():
        z = fill_holes(np.nan_to_num(z, nan=np.nanmean(z)), np.isnan(z))
    # Copernicus is made at 1.5" of longitude this far north; resampled to 1"
    # every third column is a copy of its neighbour, which shows as stripes
    # on slopes. Rebuild the real 1.5" columns (col0 = position of the first
    # in old columns, cstep = their spacing) and interpolate those.
    col0, cstep = 0.0, 1.0
    dx = np.abs(np.diff(z, axis=1))
    phase = [dx[:, k::3].mean() for k in range(3)]
    k = int(np.argmin(phase))
    if z.shape[1] > 12 and phase[k] < 0.02 * max(phase):
        cols = [3 * m + k for m in range((z.shape[1] - k - 2) // 3 + 1) if 3 * m + k + 2 < z.shape[1]]
        native = []
        for c in cols:
            native.append(z[:, c])        # the pair c, c+1 (one sample, at c + 0.5)
            native.append(z[:, c + 2])    # the single one (at c + 2)
        z = np.stack(native, 1)
        col0, cstep = k + 0.5, 1.5
        print(f"DEM columns: repeated every third (phase {k}), rebuilt {z.shape[1]} columns at 1.5x spacing")

    # --- sample the grid ---------------------------------------------------------------
    gw, gh = int(math.ceil(world_w / GRID)) + 1, int(math.ceil(world_h / GRID)) + 1
    wx = np.arange(gw) * GRID
    wy = np.arange(gh) * GRID
    WX, WY = np.meshgrid(wx, wy)
    # world -> local projection (osm_import.project) -> lon/lat
    px, py = (WX - gtx) / gs, (WY - gty) / gs
    lon = lon0 + px / (111320.0 * math.cos(math.radians(lat0)))
    lat = lat0 - py / 110574.0
    fx, fy = ((lon - dlon0) / sx - col0) / cstep, (dlat0 - lat) / sy
    outside = (fx < 0) | (fy < 0) | (fx > z.shape[1] - 1) | (fy > z.shape[0] - 1)
    if outside.any():
        print(f"Warning: {outside.mean() * 100:.1f}% of the map is outside the DEM (edge values used)")
    fx, fy = np.clip(fx, 0, z.shape[1] - 1), np.clip(fy, 0, z.shape[0] - 1)
    elev = bicubic(z, fx, fy)
    # (water levels come from straight-line interpolation: no overshoot at the shore)
    flat = bilinear(z, fx, fy)

    # --- land cover on the grid: share of each zone around every sample ----------------------
    step = int(round(GRID / tmpp))
    def zone_share(ids):
        m = np.isin(zones, ids).astype(np.float32)
        # average over GRID cells, then sample at the grid nodes
        hh, ww = (H // step) * step, (W // step) * step
        cells = m[:hh, :ww].reshape(hh // step, step, ww // step, step).mean((1, 3))
        out = np.zeros((gh, gw), np.float32)
        ch, cw = cells.shape
        out[:min(gh, ch), :min(gw, cw)] = cells[:min(gh, ch), :min(gw, cw)]
        if gh > ch:
            out[ch:, :] = out[ch - 1:ch, :]
        if gw > cw:
            out[:, cw:] = out[:, cw - 1:cw]
        return out
    water = zone_share([WATER]) > 0.5

    # --- buildings: cut out the big ones and fill in from the ground around them ------------------
    bmask = np.zeros((gh, gw), bool)
    feats = os.path.join(map_dir, "osm_features.txt")
    for line in open(feats, encoding="utf-8"):
        if not line.startswith("building "):
            continue
        v = line.split()
        cx, cy, bw, bh = float(v[3]), float(v[4]), float(v[5]), float(v[6])
        if bw * bh < 600:   # houses: the radar barely sees them at 30 m
            continue
        pad = 12.0          # the radar smears a building over its neighbours
        x0, x1 = int((cx - bw / 2 - pad) / GRID), int(math.ceil((cx + bw / 2 + pad) / GRID))
        y0, y1 = int((cy - bh / 2 - pad) / GRID), int(math.ceil((cy + bh / 2 + pad) / GRID))
        bmask[max(0, y0):max(0, y1 + 1), max(0, x0):max(0, x1 + 1)] = True
    bmask &= ~water
    elev = fill_holes(elev, bmask)
    print(f"Buildings: filled {bmask.mean() * 100:.1f}% of the map from the ground around them")

    # --- forests: lower by the canopy the radar saw, blended softly ---------------------------
    canopy = np.zeros((gh, gw), np.float64)
    for zid, metres in CANOPY.items():
        canopy += zone_share([zid]) * metres
    canopy = smooth(canopy, 2)   # ~40 m soft edges, no steps at forest edges
    elev -= canopy
    print(f"Canopy: lowered by up to {canopy.max():.1f} m (mean {canopy.mean():.1f} m)")

    # --- water: a flat surface per stretch, banks just above it -----------------------------
    # The level at each water sample: the lowest radar height in the water
    # within ~50 m (lakes come out flat, rivers step down along their course).
    # Copernicus already flattens lakes; the shore pixels mix in the land.
    big = np.where(water, flat, 1e9)
    lvl = big.copy()
    for _ in range(6):
        lvl = min4(lvl)
    lvl = np.where(water, lvl, 1e9)
    # spread the level a little onto the land next to it, for the banks
    near = lvl.copy()
    for _ in range(2):
        near = min4(near)
    bank = (~water) & (near < 1e8)
    elev = np.where(water, lvl, elev)
    elev = np.where(bank, np.maximum(elev, near + 0.35), elev)
    print(f"Water: {water.mean() * 100:.1f}% of the map, levels {lvl[water].min():.1f}..{lvl[water].max():.1f} m"
          if water.any() else "Water: none")

    # --- write ----------------------------------------------------------------------------------
    cm = np.clip(np.round(elev * 100), 0, 65535).astype(">u2")
    out = os.path.join(map_dir, "elevation.pgm")
    with open(out, "wb") as f:
        f.write(f"P5\n{gw} {gh}\n65535\n".encode())
        f.write(cm.tobytes())
    print(f"Wrote {out}: {gw}x{gh} at {GRID:.0f} m, {elev.min():.1f}..{elev.max():.1f} m above sea level")

    def setkey(text, k, v):
        if re.search(r"^%s\s*=" % re.escape(k), text, re.M):
            return re.sub(r"^%s\s*=.*$" % re.escape(k), f"{k} = {v}", text, flags=re.M)
        return text.rstrip("\n") + f"\n{k} = {v}\n"
    ini = setkey(ini, "elevation", "elevation.pgm")
    ini = setkey(ini, "elevation_metres_per_pixel", f"{GRID:g}")
    ini = setkey(ini, "elevation_units", "0.01")
    open(ini_path, "w", encoding="utf-8").write(ini)

    if not preview:
        return
    # a shaded relief picture to check the result against the source's own
    shade = preview
    gyy, gxx = np.gradient(elev, GRID)
    lit = np.clip(0.55 + (-gxx * 0.7 - gyy * 0.7) * 1.5, 0, 1)
    tint = (elev - elev.min()) / max(1e-6, elev.max() - elev.min())
    rgb = np.stack([0.35 + 0.5 * tint, 0.5 + 0.3 * tint, 0.3 + 0.2 * tint], -1) * lit[..., None]
    rgb[water] = (0.25, 0.4, 0.6)
    Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(shade)
    print(f"Wrote {shade}")


if __name__ == "__main__":
    main()
