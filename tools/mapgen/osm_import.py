#!/usr/bin/env python3
"""
Import OpenStreetMap data (.osm XML exports) into an Alone map.

The base map was digitised from a map image (see digitize.py). OSM data is far
more detailed, so where it is available it takes over: real building
footprints with their types and names, roads and footpaths with street names,
land use (forest, parks, meadows, parking, squares, water), single trees and
street lamps. Outside the OSM area the digitised map is kept.

The OSM data is aligned to the existing map automatically by matching water
and roads (the map image is Web Mercator, so a shift and a small scale
correction are enough).

Usage:
  python3 tools/mapgen/osm_import.py <map dir> <file.osm> [more.osm ...]

Writes into <map dir>:
  terrain2m.pgm     land cover at 2 m per pixel for the whole map
  roads_m.txt       all roads in metres (OSM inside the coverage, digitised outside)
  osm_features.txt  buildings, trees, lamps, places and coverage, in metres
and updates map.ini.

Requires: numpy, pillow
"""
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image, ImageDraw

# Zone ids (must match alone::Zone in src/world/Terrain.h)
UNKNOWN, WATER, FOREST, WOODLAND, PARK, RESIDENTIAL, FIELD, MEADOW, COMMERCIAL, HOSPITAL, PAVED, PARKING = range(12)
# Road classes (must match alone::RoadClass in src/world/MapData.h)
R_MINOR, R_MAJOR, R_MOTORWAY, R_PATH, R_SERVICE, R_TRACK = 1, 2, 3, 5, 6, 7

OUT_MPP = 2.0
LAT0, LON0 = 57.925, 12.54


def project(lat, lon):
    """Local equirectangular projection in metres (x east, y south)."""
    x = (lon - LON0) * 111320.0 * math.cos(math.radians(LAT0))
    y = -(lat - LAT0) * 110574.0
    return x, y


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def load_osm(paths):
    nodes, ways, rels, bounds = {}, {}, {}, []
    import gzip
    for p in paths:
        src = gzip.open(p, "rb") if p.endswith(".gz") else open(p, "rb")
        for ev, el in ET.iterparse(src, events=("end",)):
            if el.tag == "node":
                tags = {t.get("k"): t.get("v") for t in el.findall("tag")}
                nodes[int(el.get("id"))] = (float(el.get("lat")), float(el.get("lon")), tags)
                el.clear()
            elif el.tag == "way":
                refs = [int(n.get("ref")) for n in el.findall("nd")]
                tags = {t.get("k"): t.get("v") for t in el.findall("tag")}
                ways[int(el.get("id"))] = (refs, tags)
                el.clear()
            elif el.tag == "relation":
                mem = [(m.get("type"), int(m.get("ref")), m.get("role")) for m in el.findall("member")]
                tags = {t.get("k"): t.get("v") for t in el.findall("tag")}
                rels[int(el.get("id"))] = (mem, tags)
                el.clear()
            elif el.tag == "bounds":
                bounds.append(tuple(float(el.get(k)) for k in ("minlat", "minlon", "maxlat", "maxlon")))
    print(f"OSM: {len(nodes)} nodes, {len(ways)} ways, {len(rels)} relations from {len(paths)} files")
    return nodes, ways, rels, bounds


def way_xy(nodes, refs):
    pts = []
    for r in refs:
        n = nodes.get(r)
        if n:
            pts.append(project(n[0], n[1]))
    return pts


def complete(nodes, refs):
    """True when every node of the ring is in the export."""
    return all(r in nodes for r in refs)


def join_rings(segments, closed_only=True):
    """Join multipolygon member ways into closed rings."""
    rings, open_ = [], [list(s) for s in segments if len(s) >= 2]
    while open_:
        ring = open_.pop()
        changed = True
        while changed and ring[0] != ring[-1]:
            changed = False
            for i, s in enumerate(open_):
                if s[0] == ring[-1]:
                    ring += s[1:]
                elif s[-1] == ring[-1]:
                    ring += s[::-1][1:]
                elif s[-1] == ring[0]:
                    ring = s[:-1] + ring
                elif s[0] == ring[0]:
                    ring = s[::-1][:-1] + ring
                else:
                    continue
                open_.pop(i)
                changed = True
                break
        if not closed_only or (len(ring) >= 4 and ring[0] == ring[-1]):
            rings.append(ring)   # unclosed rings (members outside the export) would draw chords
    return rings


def areas(nodes, ways, rels, closed_only=True):
    """All closed areas as (tags, outer rings [refs], inner rings [refs])."""
    out = []
    for wid, (refs, tags) in ways.items():
        if len(refs) >= 4 and refs[0] == refs[-1] and tags:
            out.append((tags, [refs], []))
    for rid, (mem, tags) in rels.items():
        if tags.get("type") != "multipolygon":
            continue
        outer = [ways[m[1]][0] for m in mem if m[0] == "way" and m[1] in ways and m[2] in ("outer", "")]
        inner = [ways[m[1]][0] for m in mem if m[0] == "way" and m[1] in ways and m[2] == "inner"]
        # tags may live on the outer way for old-style multipolygons
        t = dict(tags)
        t.pop("type", None)
        if not t and outer:
            t = ways.get(next(m[1] for m in mem if m[2] == "outer"), ([], {}))[1]
        out.append((t, join_rings(outer, closed_only), join_rings(inner, closed_only)))
    return out


# ---------------------------------------------------------------------------
# Alignment
# ---------------------------------------------------------------------------

def read_pgm(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P5\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data[m.end():m.end() + w * h], np.uint8).reshape(h, w).copy()


def align(base_zones, base_mpp, base_roads, nodes, ways, water_polys):
    """Find scale s and offset (tx, ty) so world = s * osm_xy + t."""
    h, w = base_zones.shape
    target = (base_zones == WATER).astype(np.float32)
    road_img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(road_img)
    for cls, pts in base_roads:
        d.line(pts, fill=1, width=2)
    target += np.asarray(road_img, np.float32) * 0.6

    major = [ways[i][0] for i in ways if ways[i][1].get("highway") in
             ("motorway", "trunk", "primary", "secondary", "tertiary", "residential")]
    best = None
    for s in np.arange(0.92, 1.085, 0.01):
        # rasterise OSM in pixel units of the base map, origin at the middle
        cw, ch = w * 2, h * 2
        img = Image.new("F", (cw, ch), 0)
        dd = ImageDraw.Draw(img)
        def px(p):
            return (p[0] * s / base_mpp + cw / 2, p[1] * s / base_mpp + ch / 2)
        for poly in water_polys:
            if len(poly) >= 3:
                dd.polygon([px(p) for p in poly], fill=1.0)
        road = Image.new("F", (cw, ch), 0)
        dr = ImageDraw.Draw(road)
        for refs in major:
            pts = [px(p) for p in way_xy(nodes, refs)]
            if len(pts) >= 2:
                dr.line(pts, fill=0.6, width=2)
        src = np.asarray(img, np.float32) + np.asarray(road, np.float32)
        # cross-correlate via FFT
        T = np.zeros((ch, cw), np.float32)
        T[:h, :w] = target
        corr = np.fft.irfft2(np.fft.rfft2(T) * np.conj(np.fft.rfft2(src)), s=(ch, cw))
        iy, ix = np.unravel_index(np.argmax(corr), corr.shape)
        score = corr[iy, ix] / (src.sum() + 1)
        # shift that maps src pixel (u,v) to target pixel (u+ix, v+iy) modulo size
        if ix > cw / 2: ix -= cw
        if iy > ch / 2: iy -= ch
        tx = (ix + cw / 2) * base_mpp
        ty = (iy + ch / 2) * base_mpp
        if best is None or score > best[0]:
            best = (score, s, tx, ty)
    score, s, tx, ty = best
    print(f"Alignment: scale {s:.3f}, offset ({tx:.0f}, {ty:.0f}) m, score {score:.3f}")
    return s, tx, ty


def align_anchors(path, base_mpp, nodes, ways, rels):
    """Least-squares fit of world = s * xy + t from named OSM features."""
    found = {}
    for tags, outers, inners in areas(nodes, ways, rels, closed_only=False):
        n = tags.get("name")
        if n and outers:
            xy = way_xy(nodes, max(outers, key=len))
            if len(xy) >= 3:
                a, cx, cy = poly_area_centroid(xy)
                found.setdefault(n, (cx, cy))
    for nid, (lat, lon, tags) in nodes.items():
        if tags.get("name"):
            found.setdefault(tags["name"], project(lat, lon))
    src, dst = [], []
    for line in open(path, encoding="utf-8"):
        m = re.match(r'\s*"([^"]+)"\s+([-\d.]+)\s+([-\d.]+)', line)
        if not m:
            continue
        if m.group(1) not in found:
            print(f"Anchor {m.group(1)} not found in OSM data")
            continue
        src.append(found[m.group(1)])
        dst.append((float(m.group(2)) * base_mpp, float(m.group(3)) * base_mpp))
    src, dst = np.array(src), np.array(dst)
    sc, dc = src.mean(0), dst.mean(0)
    s = float(np.sum((src - sc) * (dst - dc)) / np.sum((src - sc) ** 2))
    t = dc - s * sc
    res = np.sqrt(((src * s + t - dst) ** 2).sum(1))
    print(f"Alignment from {len(src)} anchors: scale {s:.3f}, offset ({t[0]:.0f}, {t[1]:.0f}) m, residuals " +
          ", ".join(f"{r:.0f}" for r in res) + " m")
    return s, float(t[0]), float(t[1])


# ---------------------------------------------------------------------------
# Buildings
# ---------------------------------------------------------------------------

def convex_hull(pts):
    pts = sorted(set(pts))
    if len(pts) <= 2:
        return pts
    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lower, upper = [], []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def min_rect(pts):
    """Minimum-area rectangle: (length, width, angle of the length side)."""
    hull = convex_hull(pts)
    best = None
    for i in range(len(hull)):
        a, b = hull[i], hull[(i + 1) % len(hull)]
        ang = math.atan2(b[1] - a[1], b[0] - a[0])
        c, s = math.cos(-ang), math.sin(-ang)
        xs = [p[0] * c - p[1] * s for p in hull]
        ys = [p[0] * s + p[1] * c for p in hull]
        w, h = max(xs) - min(xs), max(ys) - min(ys)
        if best is None or w * h < best[0]:
            best = (w * h, w, h, ang)
    _, w, h, ang = best
    if h > w:
        w, h, ang = h, w, ang + math.pi / 2
    return w, h, ang


def poly_area_centroid(pts):
    a = cx = cy = 0.0
    for i in range(len(pts)):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % len(pts)]
        c = x0 * y1 - x1 * y0
        a += c
        cx += (x0 + x1) * c
        cy += (y0 + y1) * c
    a *= 0.5
    if abs(a) < 1e-9:
        xs, ys = zip(*pts)
        return 0.0, sum(xs) / len(xs), sum(ys) / len(ys)
    return abs(a), cx / (6 * a), cy / (6 * a)


def point_in_poly(x, y, poly):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi:
            inside = not inside
        j = i
    return inside


SHOP_KIND = {
    "supermarket": "grocery", "convenience": "grocery", "greengrocer": "grocery", "bakery": "cafe", "kiosk": "kiosk",
    "clothes": "clothes", "shoes": "clothes", "fashion": "clothes", "boutique": "clothes", "bag": "clothes",
    "hardware": "hardware", "doityourself": "hardware", "garden_centre": "hardware", "trade": "hardware",
    "outdoor": "outdoor", "sports": "outdoor", "hunting": "outdoor", "bicycle": "outdoor",
    "chemist": "pharmacy", "pharmacy": "pharmacy", "medical_supply": "pharmacy", "optician": "office",
    "books": "books", "stationery": "books", "electronics": "electronics", "mobile_phone": "electronics",
    "alcohol": "alcohol", "beverages": "alcohol", "mall": "mall", "department_store": "mall",
    "variety_store": "variety", "gift": "variety", "toys": "variety", "interior_decoration": "variety",
    "furniture": "variety", "hairdresser": "office", "beauty": "office", "florist": "variety", "pet": "variety",
}
AMENITY_KIND = {
    "restaurant": "restaurant", "fast_food": "restaurant", "cafe": "cafe", "pub": "restaurant", "bar": "restaurant",
    "pharmacy": "pharmacy", "hospital": "hospital", "clinic": "clinic", "doctors": "clinic", "dentist": "clinic",
    "school": "school", "kindergarten": "school", "library": "books", "townhall": "townhall", "police": "police",
    "fire_station": "firestation", "place_of_worship": "church", "fuel": "gasstation", "bank": "office",
    "post_office": "office", "theatre": "office", "cinema": "office", "community_centre": "office",
}



CSS_NAMES = {"white": "f2f1ee", "grey": "9a9996", "gray": "9a9996", "black": "2a2a2c", "red": "a8322a", "brown": "6a4a30",
             "yellow": "e3c565", "beige": "d9cbb0", "blue": "6f8fa8", "green": "6a8a5a", "orange": "d86a2a", "darkgrey": "5e5c64"}


def css_colour(v):
    """'#ff7800', 'White', 'grey' -> 'ff7800' (or '')."""
    v = v.strip().lower()
    if re.fullmatch(r"#?[0-9a-f]{6}", v):
        return v.lstrip("#")
    if re.fullmatch(r"#[0-9a-f]{3}", v):
        return "".join(ch * 2 for ch in v[1:])
    return CSS_NAMES.get(v, "")

def building_kind(tags, pois, area, landuse):
    """Decide what a building is from its own tags, POIs inside it and land use around it."""
    name = tags.get("name", "")
    low = name.lower()
    if "vårdcentral" in low or "vårdcentral" in low or "tandvård" in low:
        return "clinic"
    if low.startswith("apotek") or "apotek" in low.split():
        return "pharmacy"
    if tags.get("amenity") == "parking" or tags.get("building") == "parking":
        return "parking_garage" if tags.get("parking") == "multi-storey" or tags.get("building") == "parking" else "skip"
    for k, table in (("shop", SHOP_KIND), ("amenity", AMENITY_KIND)):
        v = tags.get(k)
        if v in table:
            return table[v]
    b = tags.get("building", "yes")
    kinds = [p for p in pois if p]
    if "mall" in kinds or name.lower().startswith("gallerian"):
        return "mall"
    if len(kinds) >= 5 and area > 2500:
        return "mall"
    if kinds:
        return kinds[0]
    direct = {
        "house": "house", "detached": "house", "semidetached_house": "house", "bungalow": "house", "farm": "house",
        "terrace": "terrace", "apartments": "apartments", "dormitory": "apartments", "residential": "apartments",
        "garage": "shed", "garages": "garages", "shed": "shed", "carport": "shed", "hut": "shed", "cabin": "house",
        "barn": "barn", "stable": "barn", "farm_auxiliary": "barn", "greenhouse": "shed",
        "industrial": "warehouse", "warehouse": "warehouse", "storage_tank": "skip", "roof": "skip", "service": "shed",
        "retail": "variety", "supermarket": "grocery", "commercial": "office", "office": "office",
        "school": "school", "kindergarten": "school", "university": "school", "church": "church", "chapel": "church",
        "hospital": "hospital", "train_station": "station", "transportation": "station", "public": "office",
        "civic": "office", "government": "townhall", "sports_hall": "sports", "parking": "parking_garage", "fire_station": "firestation",
    }
    if b in direct:
        return direct[b]
    try:
        lv = int(float(tags.get("building:levels", "0")))
    except ValueError:
        lv = 0
    if lv >= 4 and landuse in (RESIDENTIAL, COMMERCIAL, UNKNOWN) and area > 300:
        return "apartments"   # tall blocks without a shop in them are flats
    # building=yes: guess from size and surroundings
    if landuse in (COMMERCIAL, PAVED):
        return "variety" if area < 400 else "office"
    if area < 30:
        return "shed"
    if area < 220:
        return "house"
    if landuse == HOSPITAL:
        return "hospital"
    return "apartments" if area < 2500 else "warehouse"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    map_dir, osm_paths = sys.argv[1], sys.argv[2:]
    ini = open(os.path.join(map_dir, "map.ini"), encoding="utf-8").read()
    base_mpp = float(re.search(r"^metres_per_pixel\s*=\s*([\d.]+)", ini, re.M).group(1))
    base = read_pgm(os.path.join(map_dir, "terrain.pgm"))
    H, W = base.shape
    world_w, world_h = W * base_mpp, H * base_mpp

    base_roads = []
    for line in open(os.path.join(map_dir, "roads.txt")):
        if line.startswith("#") or not line.strip():
            continue
        v = line.split()
        nums = list(map(float, v[1:]))
        base_roads.append((int(v[0]), [(nums[i] + 0.5, nums[i + 1] + 0.5) for i in range(0, len(nums) - 1, 2)]))

    nodes, ways, rels, bounds = load_osm(osm_paths)
    all_areas = areas(nodes, ways, rels)

    water_polys = []
    for tags, outers, inners in all_areas:
        if tags.get("natural") == "water" or tags.get("waterway") == "riverbank" or tags.get("water"):
            for r in outers:
                water_polys.append(way_xy(nodes, r))
    anchors_path = os.path.join(map_dir, "anchors.txt")
    if os.path.exists(anchors_path):
        s, tx, ty = align_anchors(anchors_path, base_mpp, nodes, ways, rels)
    else:
        s, tx, ty = align(base, base_mpp, base_roads, nodes, ways, water_polys)

    def W_(p):
        return (p[0] * s + tx, p[1] * s + ty)

    # Coverage: the union of the export boxes
    cover = []
    for (la0, lo0, la1, lo1) in bounds:
        x0, y1 = W_(project(la0, lo0))
        x1, y0 = W_(project(la1, lo1))
        cover.append((x0, y0, x1, y1))
    def covered(x, y, margin=0):
        return any(c[0] + margin <= x <= c[2] - margin and c[1] + margin <= y <= c[3] - margin for c in cover)

    # --- terrain at 2 m ----------------------------------------------------------
    ow, oh = int(math.ceil(world_w / OUT_MPP)), int(math.ceil(world_h / OUT_MPP))
    ys = (np.arange(oh) * OUT_MPP / base_mpp).astype(int).clip(0, H - 1)
    xs = (np.arange(ow) * OUT_MPP / base_mpp).astype(int).clip(0, W - 1)
    zones = base[ys][:, xs].copy()
    # Within the OSM area, the screenshot's "natural" light green is mostly forest
    cover_mask = np.zeros((oh, ow), bool)
    for c in cover:
        cover_mask[max(0, int(c[1] / OUT_MPP)):int(c[3] / OUT_MPP), max(0, int(c[0] / OUT_MPP)):int(c[2] / OUT_MPP)] = True
    zones[cover_mask & (zones == WOODLAND)] = FOREST

    img = Image.fromarray(zones, "L")
    draw = ImageDraw.Draw(img)

    def landuse_zone(t):
        lu, nat, lei, am = t.get("landuse"), t.get("natural"), t.get("leisure"), t.get("amenity")
        if nat == "water" or t.get("waterway") == "riverbank" or lu in ("reservoir", "basin"):
            return WATER, 9
        if am == "parking" or lu == "garages":
            return PARKING, 7
        if t.get("highway") == "pedestrian" or t.get("place") == "square" or (t.get("area") == "yes" and t.get("highway")):
            return PAVED, 8
        if am in ("hospital",) or lu == "hospital":
            return HOSPITAL, 4
        if lu in ("forest",) or nat in ("wood",):
            return FOREST, 5
        if nat in ("scrub", "heath", "wetland") or lu in ("greenfield",):
            return WOODLAND, 5
        if lei in ("park", "garden", "pitch", "playground", "recreation_ground", "golf_course", "sports_centre", "dog_park") or \
           lu in ("grass", "cemetery", "allotments", "village_green", "recreation_ground", "flowerbed"):
            return PARK, 6
        if lu in ("meadow",) or nat in ("grassland",):
            return MEADOW, 5
        if lu in ("farmland", "farmyard", "orchard"):
            return FIELD, 4
        if lu in ("residential",):
            return RESIDENTIAL, 1
        if lu in ("commercial", "retail", "industrial", "railway", "construction", "brownfield"):
            return COMMERCIAL, 2
        if nat in ("beach", "sand"):
            return MEADOW, 6
        return None, 0

    layered = []
    for tags, outers, inners in all_areas:
        if "building" in tags:
            continue
        z, prio = landuse_zone(tags)
        if z is None:
            continue
        layered.append((prio, z, outers, inners))
    layered.sort(key=lambda t: t[0])
    def opx(p):
        q = W_(p)
        return (q[0] / OUT_MPP, q[1] / OUT_MPP)
    for prio, z, outers, inners in layered:
        for r in outers:
            if not complete(nodes, r):
                continue
            pts = [opx(p) for p in way_xy(nodes, r)]
            if len(pts) >= 3:
                draw.polygon(pts, fill=z)
        for r in inners:
            if not complete(nodes, r):
                continue
            pts = [opx(p) for p in way_xy(nodes, r)]
            if len(pts) >= 3:
                draw.polygon(pts, fill=RESIDENTIAL if z != WATER else FOREST)
    # Rivers and streams
    for wid, (refs, tags) in ways.items():
        ww = tags.get("waterway")
        if ww in ("river", "stream", "canal", "ditch", "drain") and tags.get("tunnel") not in ("yes", "culvert"):
            width = {"river": 9, "canal": 6, "stream": 3, "ditch": 1.5, "drain": 1.5}[ww]
            pts = [opx(p) for p in way_xy(nodes, refs)]
            if len(pts) >= 2:
                draw.line(pts, fill=WATER, width=max(1, int(round(width / OUT_MPP))))
    zones = np.asarray(img, np.uint8)
    with open(os.path.join(map_dir, "terrain2m.pgm"), "wb") as f:
        f.write(f"P5\n{ow} {oh}\n255\n".encode())
        f.write(zones.tobytes())
    Image.fromarray(zones * 20, "L").save(os.path.join(map_dir, "preview_osm.png"))

    # --- roads ---------------------------------------------------------------
    road_lines = []
    kept = 0
    for cls, pts in base_roads:
        # keep digitised roads outside the OSM coverage
        seg = []
        for p in pts:
            q = (p[0] * base_mpp, p[1] * base_mpp)
            if covered(q[0], q[1], margin=-5):
                if len(seg) >= 2:
                    road_lines.append((cls, "-", seg))
                    kept += 1
                seg = []
            else:
                seg.append(q)
        if len(seg) >= 2:
            road_lines.append((cls, "-", seg))
            kept += 1
    osm_roads = 0
    for wid, (refs, tags) in ways.items():
        hw = tags.get("highway")
        if not hw or tags.get("area") == "yes" or tags.get("tunnel") in ("yes", "building_passage"):
            continue
        ref = tags.get("ref", "")
        if hw in ("motorway", "motorway_link") or (hw in ("trunk", "trunk_link") and "E" in ref):
            cls = R_MOTORWAY if "link" not in hw else R_MAJOR
        elif hw in ("trunk", "trunk_link", "primary", "primary_link", "secondary", "secondary_link", "tertiary", "tertiary_link"):
            cls = R_MAJOR
        elif hw in ("residential", "unclassified", "living_street", "road"):
            cls = R_MINOR
        elif hw == "service":
            cls = R_SERVICE
        elif hw == "track":
            cls = R_TRACK
        elif hw in ("footway", "path", "cycleway", "pedestrian", "steps", "bridleway"):
            cls = R_PATH
        else:
            continue
        pts = [W_(p) for p in way_xy(nodes, refs)]
        if len(pts) < 2:
            continue
        name = tags.get("name", "-").replace(" ", "_")
        road_lines.append((cls, name, pts))
        osm_roads += 1
    with open(os.path.join(map_dir, "roads_m.txt"), "w", encoding="utf-8") as f:
        f.write("# Roads in metres: <class> <name or -> x0 y0 x1 y1 ...\n")
        f.write("# classes: 1 minor, 2 major, 3 motorway, 5 path, 6 service, 7 track\n")
        for cls, name, pts in road_lines:
            f.write(f"{cls} {name} " + " ".join(f"{x:.1f} {y:.1f}" for x, y in pts) + "\n")
    print(f"Roads: {osm_roads} from OSM, {kept} digitised pieces kept outside")

    # --- features -------------------------------------------------------------
    out = []
    out.append("# Generated by tools/mapgen/osm_import.py from OpenStreetMap data (c) OpenStreetMap contributors, ODbL")
    out.append("# All coordinates in metres.")
    for c in cover:
        out.append(f"coverage {c[0]:.0f} {c[1]:.0f} {c[2]:.0f} {c[3]:.0f}")
    for wid, (refs, tags) in ways.items():
        if tags.get("railway") == "rail" and tags.get("service") not in ("yard", "siding", "spur"):
            pts = [W_(p) for p in way_xy(nodes, refs)]
            if len(pts) >= 2:
                out.append("railway " + " ".join(f"{x:.1f} {y:.1f}" for x, y in pts))
    for wid, (refs, tags) in ways.items():
        if tags.get("man_made") == "pier":
            pts = [W_(p) for p in way_xy(nodes, refs)]
            if len(pts) >= 2:
                closed = len(refs) >= 4 and refs[0] == refs[-1]
                out.append(("pier_area " if closed else "pier ") + " ".join(f"{x:.1f} {y:.1f}" for x, y in pts))
    for nid, (lat, lon, tags) in nodes.items():
        x, y = W_(project(lat, lon))
        if tags.get("place") in ("suburb", "neighbourhood", "quarter") and tags.get("name"):
            out.append(f'district "{tags["name"]}" {x:.0f} {y:.0f} 350')
        elif tags.get("natural") == "tree":
            out.append(f"tree {x:.1f} {y:.1f}")
        elif tags.get("highway") == "street_lamp":
            out.append(f"lamp {x:.1f} {y:.1f}")

    # POIs for typing buildings
    pois = []
    for nid, (lat, lon, tags) in nodes.items():
        kind = SHOP_KIND.get(tags.get("shop")) or AMENITY_KIND.get(tags.get("amenity"))
        if kind:
            pois.append((project(lat, lon), kind, tags.get("name", "")))

    # Land use around buildings
    def zone_at(x, y):
        ix, iy = int(x / OUT_MPP), int(y / OUT_MPP)
        if 0 <= ix < ow and 0 <= iy < oh:
            return int(zones[iy, ix])
        return UNKNOWN

    # Railway stations are usually mapped as a node on the platform: the
    # nearest building within 80 m is the station house
    forced = {}
    stations = [project(lat, lon) for (lat, lon, t) in nodes.values()
                if t.get("railway") == "station" and t.get("station") != "subway"]
    for sp in stations:
        best, bd = None, 80.0
        for ai, (tags, outers, inners) in enumerate(all_areas):
            if "building" not in tags or not outers:
                continue
            xy = way_xy(nodes, max(outers, key=len))
            if len(xy) < 3:
                continue
            a, cx, cy = poly_area_centroid(xy)
            d = math.hypot(cx - sp[0], cy - sp[1])
            if a > 60 and d < bd:
                best, bd = ai, d
        if best is not None:
            forced[best] = "station"

    nb = 0
    kinds_count = {}
    for ai, (tags, outers, inners) in enumerate(all_areas):
        is_garage = tags.get("amenity") == "parking" and tags.get("parking") == "multi-storey"
        if ("building" not in tags and not is_garage) or not outers:
            continue
        ring = max(outers, key=len)
        xy = way_xy(nodes, ring)
        if len(xy) < 3:
            continue
        area, cx, cy = poly_area_centroid(xy)
        if area < 8:
            continue
        inside = [k for (p, k, n) in pois if point_in_poly(p[0], p[1], xy)]
        names = [n for (p, k, n) in pois if n and point_in_poly(p[0], p[1], xy)]
        wx, wy = W_((cx, cy))
        kind = forced.get(ai) or building_kind(tags, inside, area, zone_at(wx, wy))
        if kind == "skip":
            continue
        L, Wd, ang = min_rect(xy)
        fill = area / max(1.0, L * Wd)
        if fill < 0.75:   # L-shapes etc: keep the same area
            k = math.sqrt(max(0.3, fill))
            L, Wd = L * k, Wd * k
        L, Wd = L * s, Wd * s
        # Snap the orientation to the grid
        if abs(math.cos(ang)) >= abs(math.sin(ang)):
            bw, bh = L, Wd
        else:
            bw, bh = Wd, L
        bw, bh = max(3, int(round(bw))), max(3, int(round(bh)))
        levels = tags.get("building:levels", "")
        try:
            levels = max(1, min(12, int(float(levels))))
        except ValueError:
            levels = 0
            try:
                levels = max(1, min(12, int(round(float(tags.get("height", "").replace("m", "").strip()) / 2.8))))
            except ValueError:
                levels = 0
        extra = ""
        for key, field in (("building:colour", "colour"), ("roof:colour", "roof_colour")):
            c = css_colour(tags.get(key, ""))
            if c:
                extra += f" {field}={c}"
        shape = tags.get("roof:shape", "")
        if shape in ("gabled", "gabled_height_moved", "crosspitched", "saltbox"):
            extra += " roof=gable"
        elif shape in ("hipped", "half-hipped", "pyramidal"):
            extra += " roof=hip"
        elif shape in ("flat", "skillion"):
            extra += " roof=flat"
        name = tags.get("name", "") or (names[0] if names else "")
        name = name.replace('"', "'")
        out.append(f'building {kind} {levels} {wx:.1f} {wy:.1f} {bw} {bh} "{name}"{extra}')
        kinds_count[kind] = kinds_count.get(kind, 0) + 1
        nb += 1
    with open(os.path.join(map_dir, "osm_features.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print(f"Buildings: {nb}  " + ", ".join(f"{k} {v}" for k, v in sorted(kinds_count.items(), key=lambda t: -t[1])))

    # --- map.ini -----------------------------------------------------------------
    def setkey(text, key, value):
        if re.search(rf"^{key}\s*=", text, re.M):
            return re.sub(rf"^{key}\s*=.*$", f"{key} = {value}", text, flags=re.M)
        return text.replace("[map]\n", f"[map]\n{key} = {value}\n", 1)
    ini = setkey(ini, "terrain_osm", "terrain2m.pgm")
    ini = setkey(ini, "terrain_osm_metres_per_pixel", f"{OUT_MPP}")
    ini = setkey(ini, "roads_metres", "roads_m.txt")
    ini = setkey(ini, "osm_features", "osm_features.txt")
    # world = geo_scale * project(lat, lon) + geo_offset, around (geo_lat0, geo_lon0):
    # lets other tools (dem_import.py) place real-world data on the map
    ini = setkey(ini, "geo_lat0", f"{LAT0}")
    ini = setkey(ini, "geo_lon0", f"{LON0}")
    ini = setkey(ini, "geo_scale", f"{s:.6f}")
    ini = setkey(ini, "geo_offset_x", f"{tx:.2f}")
    ini = setkey(ini, "geo_offset_y", f"{ty:.2f}")
    with open(os.path.join(map_dir, "map.ini"), "w", encoding="utf-8") as f:
        f.write(ini)
    print("Updated map.ini")


if __name__ == "__main__":
    main()
