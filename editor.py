#!/usr/bin/env python3
"""
Alone World Editor
Native format: .svworld (JSON) — what Save/Open uses.
Export format: .world  (binary, ChunkLibrary.cpp compatible) — via File > Export.

Changes vs original:
 - Save/Open uses .svworld (lossless JSON editor format)
 - Export writes .world binary (floor + objects merged, bottom faces stripped,
   all vertex values clamped to s16 range -32768..32767, textures DS-ified)
 - Floor and imported object geometry unified into one vertex list per chunk
 - Texture can be applied to imported models at import time
 - UV coords use actual texture dimensions (width*16, height*16 for full tile)
"""

import copy
import io
import json
import math
import os
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

try:
    import trimesh
    HAS_TRIMESH = True
except ImportError:
    HAS_TRIMESH = False

from OpenGL.GL import *
from OpenGL.GLU import *
from PyQt6.QtCore import QPoint, QSize, Qt, QThread, QTimer, pyqtSignal, pyqtSlot
from PyQt6.QtOpenGLWidgets import QOpenGLWidget
from PyQt6.QtGui import (
    QAction, QColor, QCursor, QFont, QIcon, QKeySequence,
    QMatrix4x4, QPainter, QPalette, QPixmap, QImage,
    QSurfaceFormat, QVector3D,
)
from PyQt6.QtWidgets import (
    QApplication, QCheckBox, QColorDialog, QComboBox, QDialog,
    QDialogButtonBox, QDoubleSpinBox, QFileDialog, QFormLayout,
    QFrame, QGroupBox, QHBoxLayout, QInputDialog, QLabel,
    QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMenu,
    QMessageBox, QProgressBar, QPushButton, QScrollArea,
    QSizePolicy, QSlider, QSpinBox, QSplitter, QStatusBar,
    QTabWidget, QToolBar, QTreeWidget, QTreeWidgetItem,
    QVBoxLayout, QWidget,
)

# ---------------------------------------------------------------------------
# .world binary format  (matches ChunkLibrary.h)
# ---------------------------------------------------------------------------
WORLD_MAGIC = b"ALWF"
WORLD_VERSION = 1
CHUNK_WORLD_UNIT = 16          # world-space units per chunk side
FLOOR_TILES = 8                # tile grid dimension (8×8 per chunk)
TILE_SIZE = CHUNK_WORLD_UNIT / FLOOR_TILES   # world units per tile

# GL_TEXTURE_TYPE_ENUM values used by libnds
GL_RGB32_A3 = 1
GL_RGB4      = 2
GL_RGB16     = 3
GL_RGB256    = 4
GL_TEXF8     = 5   # compressed
GL_RGB8_A5   = 6
GL_RGB       = 7   # 15-bit
GL_RGBA      = 8   # 16-bit RGBA (A1 RGB5)

LIBNDS_FORMATS = {
    "RGB (15-bit)":   GL_RGB,
    "RGBA (A1 RGB5)": GL_RGBA,
    "RGB4 (4-color)": GL_RGB4,
    "RGB16 (16-color)":GL_RGB16,
    "RGB256":          GL_RGB256,
    "RGB32 (A3)":     GL_RGB32_A3,
    "RGB8 (A5)":      GL_RGB8_A5,
}

DS_MAX_TEX_SIZE  = 64
DS_MAX_POLYS     = 2048
DS_VRAM_BYTES    = 512 * 1024

NO_TEX = 0xFF   # tex_id sentinel for "no texture"

# ---------------------------------------------------------------------------
# Fixed-point helpers (NDS f32 = float * (1<<12))
# ---------------------------------------------------------------------------
FP = 1 << 12

def to_fp(f: float) -> int:
    return int(f * FP)

def from_fp(i: int) -> float:
    return i / FP

def clamp_s16(v: int) -> int:
    """Clamp an integer to signed 16-bit range (-32768..32767)."""
    return max(-32768, min(32767, v))


def _spiral_grid_pos(n: int) -> tuple[int, int]:
    """Return (grid_x, grid_z) for the nth chunk in a clockwise outward spiral.

    Layout (chunk numbers):
        10 11 ...
         9  2  3
         8  1  4
         7  6  5
    n=0 → (0,0), n=1 → (1,-1), etc.
    """
    if n == 0:
        return (0, 0)
    x, z = 0, 0
    count = 0
    ring = 0
    while True:
        ring += 1
        x += 1
        z -= 1
        side = ring * 2
        for dx, dz, steps in [
            (0,  1, side),   # right column going down  (+Z)
            (-1, 0, side),   # bottom row going left    (-X)
            (0, -1, side),   # left column going up     (-Z)
            (1,  0, side),   # top row going right      (+X)
        ]:
            for _ in range(steps):
                count += 1
                if count == n:
                    return (x, z)
                x += dx
                z += dz

# NDS t16 texture coords: value in texel units * 16 (i.e. float texel * 16)
def to_t16(texel: float) -> int:
    return int(texel * 16)


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------
class DSVertex:
    __slots__ = ("x","y","z","nx","ny","nz","r","g","b","tex_id","u","v")

    def __init__(self, x=0,y=0,z=0, nx=0,ny=0,nz=0,
                 r=255,g=255,b=255, tex_id=NO_TEX, u=0,v=0):
        self.x,self.y,self.z       = x,y,z
        self.nx,self.ny,self.nz    = nx,ny,nz
        self.r,self.g,self.b       = r,g,b
        self.tex_id                = tex_id
        self.u,self.v              = u,v

    def pack(self):
        return struct.pack("<6h4B2h",
            self.x,self.y,self.z,
            self.nx,self.ny,self.nz,
            self.r,self.g,self.b,self.tex_id,
            self.u,self.v)

    @staticmethod
    def unpack(data, offset=0):
        v = DSVertex()
        f = struct.unpack_from("<6h4B2h", data, offset)
        v.x,v.y,v.z             = f[0],f[1],f[2]
        v.nx,v.ny,v.nz          = f[3],f[4],f[5]
        v.r,v.g,v.b,v.tex_id   = f[6],f[7],f[8],f[9]
        v.u,v.v                  = f[10],f[11]
        return v

    @staticmethod
    def size():
        return struct.calcsize("<6h4B2h")


class DSTexture:
    def __init__(self, tex_id=0, width=8, height=8, fmt=GL_RGBA,
                 data=b"", name="tex"):
        self.tex_id   = tex_id
        self.width    = width
        self.height   = height
        self.fmt      = fmt
        self.data     = data        # raw RGBA bytes (editor uses RGBA internally)
        self.name     = name
        self.pil_img  = None        # PIL Image (always RGBA in editor)
        self.gl_tex   = None        # OpenGL handle (editor side)
        self._rgba_cache = None     # numpy RGBA for viewport CPU sampling

    def width_log2(self):  return int(math.log2(self.width))
    def height_log2(self): return int(math.log2(self.height))

    def pack_header(self):
        return struct.pack("<BBBBL",
            self.tex_id,
            self.width_log2(), self.height_log2(),
            self.fmt,
            len(self.data))

    def get_pil(self) -> Image.Image | None:
        """Return (cached) PIL Image in RGBA mode."""
        if self.pil_img:
            return self.pil_img
        if self.data:
            try:
                arr = np.frombuffer(self.data, dtype=np.uint8)
                expected = self.width * self.height * 4
                if len(arr) >= expected:
                    arr = arr[:expected].reshape(self.height, self.width, 4)
                    self.pil_img = Image.fromarray(arr, "RGBA")
                    return self.pil_img
                # DS-ified 16-bit packed data (2 bytes per pixel)
                expected16 = self.width * self.height * 2
                if len(arr) >= expected16:
                    packed = np.frombuffer(self.data[:expected16], dtype=np.uint16)
                    packed = packed.reshape(self.height, self.width)
                    r = ((packed & 0x1F) << 3).astype(np.uint8)
                    g = (((packed >> 5) & 0x1F) << 3).astype(np.uint8)
                    b = (((packed >> 10) & 0x1F) << 3).astype(np.uint8)
                    a = np.where(packed & 0x8000, 255, 0).astype(np.uint8)
                    rgba = np.dstack([r, g, b, a])
                    self.pil_img = Image.fromarray(rgba, "RGBA")
                    return self.pil_img
            except Exception:
                pass
        return None


class FloorTile:
    """One tile cell in the chunk floor grid."""
    __slots__ = ("tex_id", "r", "g", "b")

    def __init__(self, tex_id=NO_TEX, r=180, g=180, b=180):
        self.tex_id = tex_id
        self.r = r
        self.g = g
        self.b = b


class DSChunk:
    def __init__(self, grid_x=0, grid_z=0):
        self.grid_x   = grid_x
        self.grid_z   = grid_z
        # World-space offset applied to model (object) verts during export.
        # Lets you position an imported model precisely within its chunk.
        self.world_x  = 0.0
        self.world_y  = 0.0
        self.world_z  = 0.0
        self.vertices: list[DSVertex] = []
        self.name     = f"chunk_{grid_x}_{grid_z}"
        # Floor texture tiling (how many times the texture repeats across the chunk)
        self.floor_tile_u: float = 1.0
        self.floor_tile_v: float = 1.0
        # Floor tile grid  (FLOOR_TILES × FLOOR_TILES)
        self.floor: list[list[FloorTile]] = [
            [FloorTile() for _ in range(FLOOR_TILES)]
            for _ in range(FLOOR_TILES)
        ]

    def poly_count(self):
        """Total poly count: floor is always 2 polys (1 quad) + object verts."""
        obj_verts = max(0, len(self.vertices) - 6)   # 6 verts = the floor quad
        return 2 + obj_verts // 3

    # ------------------------------------------------------------------
    # Build vertex list from floor tiles + object verts (unified)
    # Each tile = 2 triangles (6 verts).
    # UVs map 0..width / 0..height in NDS t16 units (texel * 16).
    # Bottom-facing faces are stripped here.
    # All fixed-point values clamped to s16 range.
    # ------------------------------------------------------------------
    def bake_to_vertices(self, world: "WorldFile | None" = None) -> list:
        """Return a unified vertex list: 1 floor quad (6 verts, 2 tris) + object verts.

        Floor is now a single quad covering the whole chunk.
        Color and texture are taken from floor[0][0] (the representative tile).
        UVs span the full texture once (0..tex_w*16, 0..tex_h*16) so the
        texture tiles naturally across the chunk when repeated.
        Object verts follow after, with bottom-facing triangles stripped.
        """
        verts: list[DSVertex] = []
        half = CHUNK_WORLD_UNIT / 2.0

        # --- Single floor quad ---
        tile = self.floor[0][0]   # representative tile: color + texture
        r, g, b = tile.r, tile.g, tile.b
        tid = tile.tex_id

        tex_w = tex_h = 1
        if world and tid != NO_TEX:
            dtex = world.texture_by_id(tid)
            if dtex:
                tex_w = dtex.width
                tex_h = dtex.height

        # UV spans the texture across the chunk, scaled by tiling factor
        tu = getattr(self, 'floor_tile_u', 1.0)
        tv = getattr(self, 'floor_tile_v', 1.0)
        u0, u1 = 0, int(tex_w * 16 * tu)
        v0, v1 = 0, int(tex_h * 16 * tv)
        ny = clamp_s16(to_fp(1.0))

        def fv(lx, lz, u, v):
            return DSVertex(
                x=clamp_s16(to_fp(lx)), y=0, z=clamp_s16(to_fp(lz)),
                nx=0, ny=ny, nz=0,
                r=r, g=g, b=b,
                tex_id=tid,
                u=clamp_s16(u), v=clamp_s16(v),
            )

        x0, x1 = -half, half
        z0, z1 = -half, half
        # CCW winding when viewed from above (Y-up) so front face points upward
        verts += [
            fv(x0, z0, u0, v0), fv(x0, z1, u0, v1), fv(x1, z0, u1, v0),
            fv(x1, z0, u1, v0), fv(x0, z1, u0, v1), fv(x1, z1, u1, v1),
        ]

        # --- Object vertices (skip bottom-facing triangles) ---
        # Object verts start after the 6 floor verts in the editor's vertex list
        floor_verts_count = 6
        obj_verts = self.vertices[floor_verts_count:]
        ox_fp = clamp_s16(to_fp(self.world_x))
        oy_fp = clamp_s16(to_fp(self.world_y))
        oz_fp = clamp_s16(to_fp(self.world_z))
        for ti in range(len(obj_verts) // 3):
            a, b_v, c = obj_verts[ti*3], obj_verts[ti*3+1], obj_verts[ti*3+2]
            avg_ny = (a.ny + b_v.ny + c.ny) / 3.0 / FP
            if avg_ny <= -0.5:
                continue
            def _shift(v, ox=ox_fp, oy=oy_fp, oz=oz_fp):
                import copy as _copy
                sv = _copy.copy(v)
                sv.x = clamp_s16(sv.x + ox)
                sv.y = clamp_s16(sv.y + oy)
                sv.z = clamp_s16(sv.z + oz)
                return sv
            verts += [_shift(a), _shift(b_v), _shift(c)]

        return verts

    # Legacy alias — rebuilds just the 6-vert floor quad at vertices[0:6]
    def bake_floor_to_vertices(self, world: "WorldFile | None" = None):
        """Replace or prepend the 6-vert floor quad at vertices[0:6]."""
        floor_verts = self.bake_to_vertices(world=world)[:6]
        if len(self.vertices) >= 6:
            self.vertices[:6] = floor_verts
        else:
            self.vertices = floor_verts + self.vertices

    def pack(self):
        hdr = struct.pack("<2h2H",
            self.grid_x, self.grid_z,
            len(self.vertices), self.poly_count())
        vdata = b"".join(v.pack() for v in self.vertices)
        return hdr + vdata

    @staticmethod
    def header_size():
        return struct.calcsize("<2h2H")


class WorldFile:
    def __init__(self):
        self.textures: list[DSTexture] = []
        self.chunks:   list[DSChunk]   = []
        self.path = None

    def save(self, path):
        """Save as .svworld (JSON editor format, lossless)."""
        import base64
        doc = {
            "version": 1,
            "textures": [],
            "chunks": [],
        }
        for tex in self.textures:
            doc["textures"].append({
                "tex_id": tex.tex_id,
                "width": tex.width,
                "height": tex.height,
                "fmt": tex.fmt,
                "name": tex.name,
                "data_b64": base64.b64encode(tex.data).decode(),
            })
        for chunk in self.chunks:
            floor_rows = []
            for row in chunk.floor:
                floor_rows.append([
                    {"tex_id": t.tex_id, "r": t.r, "g": t.g, "b": t.b}
                    for t in row
                ])
            verts_data = []
            for v in chunk.vertices[6:]:   # skip the 6-vert floor quad
                verts_data.append({
                    "x": v.x, "y": v.y, "z": v.z,
                    "nx": v.nx, "ny": v.ny, "nz": v.nz,
                    "r": v.r, "g": v.g, "b": v.b,
                    "tex_id": v.tex_id,
                    "u": v.u, "v": v.v,
                })
            doc["chunks"].append({
                "grid_x": chunk.grid_x,
                "grid_z": chunk.grid_z,
                "name": chunk.name,
                "world_x": chunk.world_x,
                "world_y": chunk.world_y,
                "world_z": chunk.world_z,
                "floor_tile_u": getattr(chunk, 'floor_tile_u', 1.0),
                "floor_tile_v": getattr(chunk, 'floor_tile_v', 1.0),
                "floor": floor_rows,
                "object_verts": verts_data,
            })
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=2)
        self.path = path

    def export_world(self, path):
        """Export as .world binary (ChunkLibrary.cpp format).

        Key differences from the editor's internal format:
        - Texture data is converted from RGBA8 → packed NDS 16-bit format
          (ABGR1555 for GL_RGBA, BGR555 for GL_RGB, etc.)
        - All chunks sharing the same grid (gx, gz) are merged into one
          (the runtime only keeps one slot per grid position)
        - Floor tiles baked in; object verts appended after; bottom faces stripped
        - All fixed-point values clamped to s16 (-32768..32767)
        """
        # --- Build NDS texture list ---
        tex_list = []
        for tex in self.textures:
            img = tex.get_pil()
            if img:
                dsimg, _, nw, nh = dsify_image(img, DS_MAX_TEX_SIZE, tex.fmt)
                nds_raw = nds_pack_texture(dsimg, tex.fmt)
                etex = DSTexture(tex.tex_id, nw, nh, tex.fmt, nds_raw, tex.name)
            else:
                etex = tex
            tex_list.append(etex)

        # --- Merge chunks with identical grid coords ---
        # The ChunkLibrary runtime uses (gridX, gridZ) as a key and only stores
        # one slot per position.  Multiple editor objects at the same grid cell
        # (e.g. a floor chunk + an imported model chunk) must be written as one
        # ChunkEntry with all their vertices concatenated.
        from collections import OrderedDict
        merged_chunks: OrderedDict[tuple, list] = OrderedDict()
        for chunk in self.chunks:
            key = (chunk.grid_x, chunk.grid_z)
            verts = chunk.bake_to_vertices(world=self)
            if key not in merged_chunks:
                merged_chunks[key] = verts
            else:
                merged_chunks[key].extend(verts)

        with open(path, "wb") as f:
            f.write(struct.pack("<4sHHLh",
                WORLD_MAGIC, WORLD_VERSION,
                len(tex_list), len(merged_chunks),
                CHUNK_WORLD_UNIT))
            for tex in tex_list:
                f.write(tex.pack_header())
                f.write(tex.data)
            for (gx, gz), verts in merged_chunks.items():
                poly_count = len(verts) // 3
                hdr = struct.pack("<2h2H", gx, gz, len(verts), poly_count)
                f.write(hdr)
                for v in verts:
                    f.write(v.pack())

    @staticmethod
    def load(path):
        """Load a .svworld (JSON) or legacy .world (binary) file."""
        import base64
        with open(path, "rb") as f:
            header = f.read(4)

        if header != WORLD_MAGIC:
            # Try JSON .svworld
            with open(path, "r", encoding="utf-8") as f:
                doc = json.load(f)
            w = WorldFile()
            w.path = path
            for td in doc.get("textures", []):
                raw = base64.b64decode(td["data_b64"])
                tex = DSTexture(td["tex_id"], td["width"], td["height"],
                                td["fmt"], raw, td.get("name", "tex"))
                w.textures.append(tex)
            for cd in doc.get("chunks", []):
                chunk = DSChunk(cd["grid_x"], cd["grid_z"])
                chunk.name    = cd.get("name", chunk.name)
                chunk.world_x = cd.get("world_x", 0.0)
                chunk.world_y = cd.get("world_y", 0.0)
                chunk.world_z = cd.get("world_z", 0.0)
                chunk.floor_tile_u = cd.get("floor_tile_u", 1.0)
                chunk.floor_tile_v = cd.get("floor_tile_v", 1.0)
                for tz, row in enumerate(cd.get("floor", [])):
                    for tx, td2 in enumerate(row):
                        t = chunk.floor[tz][tx]
                        t.tex_id = td2.get("tex_id", NO_TEX)
                        t.r = td2.get("r", 180)
                        t.g = td2.get("g", 180)
                        t.b = td2.get("b", 180)
                # Rebuild floor verts so viewport can draw them (6 verts = 1 quad)
                chunk.bake_floor_to_vertices(w)
                for vd in cd.get("object_verts", []):
                    chunk.vertices.append(DSVertex(
                        x=vd["x"], y=vd["y"], z=vd["z"],
                        nx=vd["nx"], ny=vd["ny"], nz=vd["nz"],
                        r=vd["r"], g=vd["g"], b=vd["b"],
                        tex_id=vd["tex_id"],
                        u=vd["u"], v=vd["v"],
                    ))
                w.chunks.append(chunk)
            return w

        # Legacy binary .world
        w = WorldFile()
        w.path = path
        with open(path, "rb") as f:
            data = f.read()
        offset = 0
        magic, ver, tex_count, chunk_count, unit = struct.unpack_from("<4sHHLh", data, offset)
        offset += struct.calcsize("<4sHHLh")
        if magic != WORLD_MAGIC or ver != WORLD_VERSION:
            raise ValueError("Invalid .world file")

        for i in range(tex_count):
            tid, wlog, hlog, fmt, dbytes = struct.unpack_from("<BBBBL", data, offset)
            offset += struct.calcsize("<BBBBL")
            raw = data[offset:offset + dbytes]
            offset += dbytes
            tex = DSTexture(tid, 1 << wlog, 1 << hlog, fmt, raw)
            w.textures.append(tex)

        for _ in range(chunk_count):
            gx, gz, vc, pc = struct.unpack_from("<2h2H", data, offset)
            offset += DSChunk.header_size()
            chunk = DSChunk(gx, gz)
            vsz = DSVertex.size()
            for vi in range(vc):
                chunk.vertices.append(DSVertex.unpack(data, offset))
                offset += vsz
            _reconstruct_floor(chunk)
            w.chunks.append(chunk)

        return w

    def new_tex_id(self):
        used = {t.tex_id for t in self.textures}
        for i in range(256):
            if i not in used:
                return i
        return 0

    def texture_by_id(self, tid) -> DSTexture | None:
        if tid is None or int(tid) == NO_TEX:
            return None
        tid = int(tid)
        for t in self.textures:
            if int(t.tex_id) == tid:
                return t
        return None


def _reconstruct_floor(chunk: DSChunk):
    """Reconstruct the representative floor tile from the first vertex of the
    floor quad (vertex 0).  The floor is now a single quad (6 verts)."""
    if len(chunk.vertices) < 6:
        return
    v = chunk.vertices[0]
    # Propagate to all tiles so the editor grid shows correctly
    for tz in range(FLOOR_TILES):
        for tx in range(FLOOR_TILES):
            chunk.floor[tz][tx].tex_id = v.tex_id
            chunk.floor[tz][tx].r = v.r
            chunk.floor[tz][tx].g = v.g
            chunk.floor[tz][tx].b = v.b


# ---------------------------------------------------------------------------
# DSify helpers
# ---------------------------------------------------------------------------
def next_power_of_two(n):
    p = 1
    while p < n:
        p <<= 1
    return p


def _texture_rgba_array(tex: "DSTexture") -> np.ndarray | None:
    """Cached RGBA uint8 array (H, W, 4) for fast sampling."""
    if getattr(tex, "_rgba_cache", None) is not None:
        return tex._rgba_cache
    img = tex.get_pil()
    if img is None:
        return None
    tex._rgba_cache = np.array(img.convert("RGBA"), dtype=np.uint8)
    return tex._rgba_cache


def sample_ds_texture(tex: "DSTexture", u_t16: int, v_t16: int) -> tuple[int, int, int]:
    """Sample an editor texture at NDS t16 coords (texel * 16). Returns RGB 0..255."""
    arr = _texture_rgba_array(tex)
    if arr is None:
        return 255, 255, 255
    h, w = arr.shape[:2]
    tx = (int(u_t16) // 16) % w
    ty = h - 1 - ((int(v_t16) // 16) % h)
    r, g, b = arr[ty, tx, :3]
    return int(r), int(g), int(b)


def sample_ds_texture_uv(tex: "DSTexture", u: float, v: float,
                          *, flip_v: bool = False) -> tuple[int, int, int]:
    """Sample at normalized UV (0..1 per tile repeat), nearest texel."""
    arr = _texture_rgba_array(tex)
    if arr is None:
        return 255, 255, 255
    h, w = arr.shape[:2]
    tx = int(u * w) % w
    ty = int(v * h) % h
    if flip_v:
        ty = h - 1 - ty
    r, g, b = arr[ty, tx, :3]
    return int(r), int(g), int(b)


def dsify_image(pil_img, max_size=DS_MAX_TEX_SIZE, fmt=GL_RGBA):
    """Resize to NDS-compatible power-of-two dimensions.
    Returns (pil_img_rgba, raw_rgba8_bytes, new_w, new_h).
    The raw bytes are RGBA8 — suitable for editor display.
    Use nds_pack_texture() to convert for .world export."""
    w, h = pil_img.size
    nw = min(next_power_of_two(w), max_size)
    nh = min(next_power_of_two(h), max_size)
    img = pil_img.resize((nw, nh), Image.LANCZOS).convert("RGBA")
    raw = np.array(img, dtype=np.uint8).tobytes()
    return img, raw, nw, nh


def nds_pack_texture(pil_img_rgba: Image.Image, fmt: int) -> bytes:
    """Convert a PIL RGBA image to the packed NDS texture format expected by
    glTexImage2D.  The NDS stores textures in VRAM as packed 16-bit words.

    GL_RGBA  (8) = A1BGR5  — 1 bit alpha, 5 bits each B G R  (16-bit)
    GL_RGB   (7) = 0BGR5   — no alpha, 5 bits each             (16-bit)
    GL_RGB32_A3 (1) = A3BGR5 — 3-bit alpha                     (16-bit)
    GL_RGB8_A5  (6) = A5BGR5 — 5-bit alpha                     (16-bit)
    Palette modes (GL_RGB4/16/256) kept as-is (raw indexed), but editor
    doesn't generate those so we fall back to ABGR1555 for unknown.
    """
    arr = np.array(pil_img_rgba.convert("RGBA"), dtype=np.uint32)
    r = arr[:, :, 0]
    g = arr[:, :, 1]
    b = arr[:, :, 2]
    a = arr[:, :, 3]

    if fmt in (GL_RGBA,):          # A1BGR5
        r5 = (r >> 3).astype(np.uint16)
        g5 = (g >> 3).astype(np.uint16)
        b5 = (b >> 3).astype(np.uint16)
        a1 = (a >> 7).astype(np.uint16)
        packed = (a1 << 15) | (b5 << 10) | (g5 << 5) | r5
    elif fmt == GL_RGB:             # 0BGR5
        r5 = (r >> 3).astype(np.uint16)
        g5 = (g >> 3).astype(np.uint16)
        b5 = (b >> 3).astype(np.uint16)
        packed = (b5 << 10) | (g5 << 5) | r5
    elif fmt == GL_RGB32_A3:        # A3BGR5
        r5 = (r >> 3).astype(np.uint16)
        g5 = (g >> 3).astype(np.uint16)
        b5 = (b >> 3).astype(np.uint16)
        a3 = (a >> 5).astype(np.uint16)
        packed = (a3 << 13) | (b5 << 10) | (g5 << 5) | r5
    elif fmt == GL_RGB8_A5:         # A5BGR5
        r5 = (r >> 3).astype(np.uint16)
        g5 = (g >> 3).astype(np.uint16)
        b5 = (b >> 3).astype(np.uint16)
        a5 = (a >> 3).astype(np.uint16)
        packed = (a5 << 11) | (b5 << 10) | (g5 << 5) | r5
    else:                           # fallback: A1BGR5
        r5 = (r >> 3).astype(np.uint16)
        g5 = (g >> 3).astype(np.uint16)
        b5 = (b >> 3).astype(np.uint16)
        a1 = (a >> 7).astype(np.uint16)
        packed = (a1 << 15) | (b5 << 10) | (g5 << 5) | r5

    return packed.astype(np.uint16).tobytes()


def import_model_as_chunk(filepath, tex_id=NO_TEX, grid_x=0, grid_z=0,
                           scale=1.0, max_polys=DS_MAX_POLYS,
                           world=None):
    """Import OBJ/GLB/STL and convert to DSChunk with fixed-point vertices.
    tex_id: texture to apply to all faces (NO_TEX = vertex colour only).
    world: WorldFile ref used to compute UV range for the texture.
    """
    if not HAS_TRIMESH:
        raise RuntimeError("trimesh not installed")
    mesh = trimesh.load(filepath, force="mesh")
    if hasattr(mesh, "dump"):
        mesh = mesh.dump(concatenate=True)
    verts   = np.array(mesh.vertices,      dtype=np.float32) * scale
    faces   = np.array(mesh.faces,         dtype=np.int32)
    normals = np.array(mesh.vertex_normals, dtype=np.float32)
    if len(faces) > max_polys:
        ratio = max_polys / len(faces)
        mesh  = mesh.simplify_quadric_decimation(int(len(faces) * ratio))
        verts   = np.array(mesh.vertices,      dtype=np.float32) * scale
        faces   = np.array(mesh.faces,         dtype=np.int32)
        normals = np.array(mesh.vertex_normals, dtype=np.float32)
    # Strip bottom faces
    keep = [fi for fi, n in enumerate(mesh.face_normals) if n[1] > -0.5]
    faces = faces[keep]

    # Work out UV range for the chosen texture
    tex_w = tex_h = 1
    if world and tex_id != NO_TEX:
        dtex = world.texture_by_id(tex_id)
        if dtex:
            tex_w = dtex.width
            tex_h = dtex.height

    # Compute per-vertex planar UV from XZ position (top-down projection)
    # Normalised 0..1 across the mesh bounding box, then scaled to tex size
    if len(verts) > 0:
        min_x, min_z = verts[:, 0].min(), verts[:, 2].min()
        rng_x = max(verts[:, 0].max() - min_x, 1e-6)
        rng_z = max(verts[:, 2].max() - min_z, 1e-6)
    else:
        min_x = min_z = 0.0; rng_x = rng_z = 1.0

    chunk = DSChunk(grid_x, grid_z)
    for face in faces:
        for vi in face:
            p, n = verts[vi], normals[vi]
            u = clamp_s16(int(((p[0] - min_x) / rng_x) * tex_w * 16))
            v = clamp_s16(int(((p[2] - min_z) / rng_z) * tex_h * 16))
            chunk.vertices.append(DSVertex(
                x=clamp_s16(to_fp(float(p[0]))),
                y=clamp_s16(to_fp(float(p[1]))),
                z=clamp_s16(to_fp(float(p[2]))),
                nx=clamp_s16(to_fp(float(n[0]))),
                ny=clamp_s16(to_fp(float(n[1]))),
                nz=clamp_s16(to_fp(float(n[2]))),
                r=200, g=200, b=200, tex_id=tex_id,
                u=u, v=v,
            ))
    chunk.bake_floor_to_vertices(world=world)
    return chunk


# ---------------------------------------------------------------------------
# OpenGL Viewport
# ---------------------------------------------------------------------------
class Viewport(QOpenGLWidget):
    object_clicked = pyqtSignal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.world: WorldFile | None = None
        self.selected_chunk  = -1
        self.selected_chunks: set = set()
        self.cam_yaw   = 30.0
        self.cam_pitch = -45.0
        self.cam_dist  = 80.0
        self.cam_target = [0.0, 0.0, 0.0]
        self._last_mouse = None
        self._rmb_down = False
        self._mmb_down = False
        self.grid_visible = True
        self.wireframe    = False
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMouseTracking(True)
        self._gl_textures: dict[int, int] = {}
        self._vp_w = 1
        self._vp_h = 1
        # Default projection — overwritten by resizeGL before first paint
        self._proj = self._perspective_matrix(45.0, 1.0, 0.5, 2000.0)
        # Shader program handles — set in initializeGL
        self._prog_chunk = 0
        self._prog_flat  = 0

    def set_world(self, world):
        self.world = world
        self._gl_textures.clear()
        self.update()

    def set_selected_chunks(self, indices: list):
        self.selected_chunks = set(indices)
        self.selected_chunk  = indices[0] if len(indices) == 1 else -1
        self.update()

    # ------------------------------------------------------------------
    # Shader sources
    # ------------------------------------------------------------------
    _VERT_SRC = """
#version 120
uniform mat4 u_mvp;
attribute vec3 a_pos;
attribute vec3 a_color;
attribute vec2 a_uv;
varying vec3 v_color;
varying vec2 v_uv;
void main() {
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_color = a_color;
    v_uv    = a_uv;
}
"""
    _FRAG_TEXTURED_SRC = """
#version 120
uniform sampler2D u_tex;
uniform int       u_has_tex;
varying vec3 v_color;
varying vec2 v_uv;
void main() {
    vec4 t    = texture2D(u_tex, v_uv);
    vec3 tint = v_color * t.rgb;
    // mix(v_color, tint, ...) avoids branching on the uniform
    // u_has_tex == 1 → textured, 0 → vertex colour only
    float use = float(u_has_tex);
    gl_FragColor = vec4(mix(v_color, tint, use), 1.0);
}
"""
    _FRAG_FLAT_SRC = """
#version 120
varying vec3 v_color;
void main() {
    gl_FragColor = vec4(v_color, 1.0);
}
"""

    # ------------------------------------------------------------------
    @staticmethod
    def _compile_shader(src, kind):
        sh = glCreateShader(kind)
        glShaderSource(sh, src)
        glCompileShader(sh)
        if not glGetShaderiv(sh, GL_COMPILE_STATUS):
            raise RuntimeError(f"Shader compile error:\n{glGetShaderInfoLog(sh).decode()}")
        return sh

    @staticmethod
    def _link_program(vert_src, frag_src):
        vs = Viewport._compile_shader(vert_src, GL_VERTEX_SHADER)
        fs = Viewport._compile_shader(frag_src, GL_FRAGMENT_SHADER)
        prog = glCreateProgram()
        glAttachShader(prog, vs)
        glAttachShader(prog, fs)
        # Bind attribute locations before linking so we can reference them by index
        glBindAttribLocation(prog, 0, "a_pos")
        glBindAttribLocation(prog, 1, "a_color")
        glBindAttribLocation(prog, 2, "a_uv")
        glLinkProgram(prog)
        if not glGetProgramiv(prog, GL_LINK_STATUS):
            raise RuntimeError(f"Program link error:\n{glGetProgramInfoLog(prog).decode()}")
        glDeleteShader(vs)
        glDeleteShader(fs)
        return prog

    def initializeGL(self):
        glEnable(GL_DEPTH_TEST)
        glEnable(GL_CULL_FACE)
        glCullFace(GL_BACK)
        glFrontFace(GL_CCW)
        glClearColor(0.10, 0.11, 0.13, 1.0)

        # Shader: chunks (textured or vertex-colour)
        self._prog_chunk = self._link_program(self._VERT_SRC, self._FRAG_TEXTURED_SRC)
        self._uloc_mvp_chunk  = glGetUniformLocation(self._prog_chunk, "u_mvp")
        self._uloc_tex_chunk  = glGetUniformLocation(self._prog_chunk, "u_tex")
        self._uloc_has_tex    = glGetUniformLocation(self._prog_chunk, "u_has_tex")

        # Shader: flat lines (grid, selection outlines)
        self._prog_flat  = self._link_program(self._VERT_SRC, self._FRAG_FLAT_SRC)
        self._uloc_mvp_flat = glGetUniformLocation(self._prog_flat, "u_mvp")

        # 1×1 opaque-white fallback texture — always bound when no real texture is
        # needed so the sampler uniform is never pointing at an unbound unit
        self._tex_white = int(glGenTextures(1))
        glBindTexture(GL_TEXTURE_2D, self._tex_white)
        white = np.array([255, 255, 255, 255], dtype=np.uint8)
        # Use 0x1908 (GL_RGBA) as integer to avoid PyOpenGL constant lookup issues
        glTexImage2D(GL_TEXTURE_2D, 0, 0x1908, 1, 1, 0, 0x1908, GL_UNSIGNED_BYTE, white)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST)
        glBindTexture(GL_TEXTURE_2D, 0)

    @staticmethod
    def _perspective_matrix(fovy_deg, aspect, near, far):
        f  = 1.0 / math.tan(math.radians(fovy_deg) / 2.0)
        nf = 1.0 / (near - far)
        m  = np.zeros((4, 4), dtype=np.float32)
        m[0,0] = f / aspect
        m[1,1] = f
        m[2,2] = (far + near) * nf
        m[2,3] = -1.0
        m[3,2] = 2.0 * far * near * nf
        return m

    @staticmethod
    def _lookat_matrix(eye, target, up):
        f = np.array(target, dtype=np.float64) - np.array(eye, dtype=np.float64)
        f /= np.linalg.norm(f)
        u = np.array(up, dtype=np.float64)
        s = np.cross(f, u); s /= np.linalg.norm(s)
        u = np.cross(s, f)
        m = np.eye(4, dtype=np.float32)
        m[0,:3] = s;  m[0,3] = -float(np.dot(s, eye))
        m[1,:3] = u;  m[1,3] = -float(np.dot(u, eye))
        m[2,:3] = -f; m[2,3] =  float(np.dot(f, eye))
        return m

    def resizeGL(self, w, h):
        self._vp_w, self._vp_h = max(w, 1), max(h, 1)
        glViewport(0, 0, w, h)
        self._proj = self._perspective_matrix(45.0, w / max(h, 1), 0.5, 2000.0)

    def _build_mvp(self):
        """Return proj * view as a column-major float32 array for glUniformMatrix4fv."""
        yaw   = math.radians(self.cam_yaw)
        pitch = math.radians(self.cam_pitch)
        cx = self.cam_target[0] + self.cam_dist * math.cos(pitch) * math.sin(yaw)
        cy = self.cam_target[1] + self.cam_dist * math.sin(pitch)
        cz = self.cam_target[2] + self.cam_dist * math.cos(pitch) * math.cos(yaw)
        mv = self._lookat_matrix([cx, cy, cz], self.cam_target, [0, 1, 0])
        mvp = self._proj @ mv
        return np.ascontiguousarray(mvp.T, dtype=np.float32)

    def _build_mvp_translated(self, tx, ty, tz):
        """MVp with an additional translation (for chunk origins)."""
        T = np.eye(4, dtype=np.float32)
        T[0, 3] = tx; T[1, 3] = ty; T[2, 3] = tz
        yaw   = math.radians(self.cam_yaw)
        pitch = math.radians(self.cam_pitch)
        cx = self.cam_target[0] + self.cam_dist * math.cos(pitch) * math.sin(yaw)
        cy = self.cam_target[1] + self.cam_dist * math.sin(pitch)
        cz = self.cam_target[2] + self.cam_dist * math.cos(pitch) * math.cos(yaw)
        mv = self._lookat_matrix([cx, cy, cz], self.cam_target, [0, 1, 0])
        mvp = self._proj @ mv @ T
        return np.ascontiguousarray(mvp.T, dtype=np.float32)

    def paintGL(self):
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
        self._pick_viewport = (0, 0, self._vp_w, self._vp_h)

        mvp = self._build_mvp()

        if self.grid_visible:
            self._draw_grid(mvp)
        if self.world:
            for i, chunk in enumerate(self.world.chunks):
                self._draw_chunk(chunk, selected=(i in self.selected_chunks), base_mvp=mvp)

        glUseProgram(0)

    # _apply_camera kept for ray-picking which reads cam_yaw/pitch/dist directly
    def _apply_camera(self):
        pass

    # ------------------------------------------------------------------
    # Helper: draw a flat-shaded line list with the flat shader
    # pts_colors: list of (x,y,z, r,g,b) floats
    # ------------------------------------------------------------------
    def _draw_lines(self, mvp, pts_colors, line_width=1.0):
        if pts_colors is None or (hasattr(pts_colors, '__len__') and len(pts_colors) == 0):
            return
        arr = np.array(pts_colors, dtype=np.float32)  # N x 6
        n   = len(arr)
        pos = np.ascontiguousarray(arr[:, :3])
        col = np.ascontiguousarray(arr[:, 3:])
        uv  = np.zeros((n, 2), dtype=np.float32)

        glUseProgram(self._prog_flat)
        glUniformMatrix4fv(self._uloc_mvp_flat, 1, GL_FALSE, mvp)

        glEnableVertexAttribArray(0)
        glEnableVertexAttribArray(1)
        glEnableVertexAttribArray(2)
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, pos)
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, col)
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, uv)

        glLineWidth(line_width)
        glDrawArrays(GL_LINES, 0, n)
        glLineWidth(1.0)

        glDisableVertexAttribArray(0)
        glDisableVertexAttribArray(1)
        glDisableVertexAttribArray(2)

    def _draw_grid(self, mvp):
        size = 10 * CHUNK_WORLD_UNIT
        step = CHUNK_WORLD_UNIT
        gc = (0.22, 0.24, 0.27)
        pts = []
        for i in range(-size, size + step, step):
            pts += [i, 0, -size, *gc,  i, 0,  size, *gc]
            pts += [-size, 0, i, *gc,   size, 0, i, *gc]
        # axis lines
        pts += [0,0,0, 0.8,0.2,0.2,  8,0,0, 0.8,0.2,0.2]
        pts += [0,0,0, 0.2,0.8,0.2,  0,8,0, 0.2,0.8,0.2]
        pts += [0,0,0, 0.2,0.4,0.9,  0,0,8, 0.2,0.4,0.9]
        arr = np.array(pts, dtype=np.float32).reshape(-1, 6)
        self._draw_lines(mvp, arr, line_width=1.0)

    def _get_or_upload_texture(self, tex: DSTexture) -> int | None:
        if tex.tex_id in self._gl_textures:
            return self._gl_textures[tex.tex_id]
        img = tex.get_pil()
        if img is None:
            return None
        handle = glGenTextures(1)
        glBindTexture(GL_TEXTURE_2D, handle)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT)
        try:
            img = img.convert("RGBA")
            if img.size != (tex.width, tex.height):
                img = img.resize((tex.width, tex.height), Image.NEAREST)
            arr = np.ascontiguousarray(np.flipud(np.array(img, dtype=np.uint8)))
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         tex.width, tex.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, arr)
        except Exception:
            glDeleteTextures(1, [handle])
            return None
        self._gl_textures[tex.tex_id] = handle
        return handle

    def invalidate_texture(self, tex_id: int):
        """Force re-upload next frame (call after texture data changes)."""
        if self.world:
            tex = self.world.texture_by_id(tex_id)
            if tex:
                tex._rgba_cache = None
                tex.pil_img = None
        if tex_id in self._gl_textures:
            self.makeCurrent()
            glDeleteTextures(1, [self._gl_textures.pop(tex_id)])

    # ------------------------------------------------------------------
    # Core triangle-batch draw — shader-based, no legacy state machine
    # verts_data: list of (x,y,z, r,g,b, u,v)  floats
    # tex_handle: GL texture handle or None
    # ------------------------------------------------------------------
    def _draw_tris(self, mvp, verts_data, tex_handle=None):
        if verts_data is None or len(verts_data) == 0:
            return
        n   = len(verts_data)
        arr = np.array(verts_data, dtype=np.float32)   # N x 8
        pos = np.ascontiguousarray(arr[:, 0:3])
        col = np.ascontiguousarray(arr[:, 3:6])
        uv  = np.ascontiguousarray(arr[:, 6:8])

        glUseProgram(self._prog_chunk)
        glUniformMatrix4fv(self._uloc_mvp_chunk, 1, GL_FALSE, mvp)
        glUniform1i(self._uloc_tex_chunk, 0)

        has_tex = tex_handle is not None
        glUniform1i(self._uloc_has_tex, 1 if has_tex else 0)
        glActiveTexture(GL_TEXTURE0)
        glBindTexture(GL_TEXTURE_2D, tex_handle if has_tex else self._tex_white)

        glEnableVertexAttribArray(0)
        glEnableVertexAttribArray(1)
        glEnableVertexAttribArray(2)
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, pos)
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, col)
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, uv)

        glDrawArrays(GL_TRIANGLES, 0, n)

        glDisableVertexAttribArray(0)
        glDisableVertexAttribArray(1)
        glDisableVertexAttribArray(2)
        glBindTexture(GL_TEXTURE_2D, 0)

    def _draw_chunk(self, chunk: DSChunk, selected=False, base_mvp=None):
        ox = float(chunk.grid_x * CHUNK_WORLD_UNIT)
        oz = float(chunk.grid_z * CHUNK_WORLD_UNIT)
        mvp = self._build_mvp_translated(ox, 0.0, oz)

        if selected:
            s  = CHUNK_WORLD_UNIT / 2.0
            sc = (1.0, 0.75, 0.1)
            sel_pts = [
                -s, 0.15, -s, *sc,   s, 0.15, -s, *sc,
                 s, 0.15, -s, *sc,   s, 0.15,  s, *sc,
                 s, 0.15,  s, *sc,  -s, 0.15,  s, *sc,
                -s, 0.15,  s, *sc,  -s, 0.15, -s, *sc,
            ]
            arr = np.array(sel_pts, dtype=np.float32).reshape(-1, 6)
            self._draw_lines(mvp, arr, line_width=2.5)

        mode = GL_LINE if self.wireframe else GL_FILL
        glPolygonMode(GL_FRONT_AND_BACK, mode)

        self._draw_floor(chunk, mvp)

        extra_verts = chunk.vertices[6:]
        if extra_verts:
            self._draw_vertex_list(extra_verts, mvp)

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL)

    def _draw_floor(self, chunk: DSChunk, mvp):
        """Shader-based floor quad. Texture modulates the vertex colour."""
        half = CHUNK_WORLD_UNIT / 2.0
        tile = chunk.floor[0][0]
        rc, gc, bc = tile.r / 255.0, tile.g / 255.0, tile.b / 255.0
        tiling_u = getattr(chunk, "floor_tile_u", 1.0)
        tiling_v = getattr(chunk, "floor_tile_v", 1.0)

        x0, x1 = -half, half
        z0, z1 = -half, half

        # 6 verts, 2 triangles — CCW winding seen from above
        verts = [
            x0, 0, z0,  rc, gc, bc,  0,          0,
            x0, 0, z1,  rc, gc, bc,  0,          tiling_v,
            x1, 0, z0,  rc, gc, bc,  tiling_u,   0,
            x0, 0, z1,  rc, gc, bc,  0,          tiling_v,
            x1, 0, z1,  rc, gc, bc,  tiling_u,   tiling_v,
            x1, 0, z0,  rc, gc, bc,  tiling_u,   0,
        ]

        tex_h = None
        if tile.tex_id != NO_TEX and self.world:
            dtex = self.world.texture_by_id(tile.tex_id)
            if dtex:
                tex_h = self._get_or_upload_texture(dtex)

        verts_data = np.array(verts, dtype=np.float32).reshape(-1, 8)
        self._draw_tris(mvp, verts_data, tex_handle=tex_h)

    def _draw_vertex_list(self, verts, mvp):
        """Shader-based object vertex list; batched per texture."""
        if verts is None or len(verts) == 0:
            return
        # Group consecutive verts by tex_id
        batches: dict[int, list] = {}
        for v in verts:
            tid = int(v.tex_id)
            if tid not in batches:
                batches[tid] = []
            # Convert NDS fixed-point to floats; UV: NDS t16 = texel*16
            rc, gc, bc = v.r / 255.0, v.g / 255.0, v.b / 255.0
            vx = v.x / FP;  vy = v.y / FP;  vz = v.z / FP
            if tid != NO_TEX and self.world:
                dtex = self.world.texture_by_id(tid)
                if dtex:
                    tu = v.u / (max(dtex.width,  1) * 16.0)
                    tv = v.v / (max(dtex.height, 1) * 16.0)
                else:
                    tu = tv = 0.0
            else:
                tu = tv = 0.0
            batches[tid].append((vx, vy, vz, rc, gc, bc, tu, tv))

        for tid, vdata in batches.items():
            tex_h = None
            if tid != NO_TEX and self.world:
                dtex = self.world.texture_by_id(tid)
                if dtex:
                    tex_h = self._get_or_upload_texture(dtex)
            arr = np.array(vdata, dtype=np.float32)
            self._draw_tris(mvp, arr, tex_handle=tex_h)

    @staticmethod
    def _ray_triangle(orig, direction, v0, v1, v2):
        """Möller–Trumbore; return distance t along ray or None."""
        edge1 = v1 - v0
        edge2 = v2 - v0
        pvec = np.cross(direction, edge2)
        det = float(np.dot(edge1, pvec))
        if abs(det) < 1e-8:
            return None
        inv_det = 1.0 / det
        tvec = orig - v0
        u = float(np.dot(tvec, pvec)) * inv_det
        if u < 0.0 or u > 1.0:
            return None
        qvec = np.cross(tvec, edge1)
        v = float(np.dot(direction, qvec)) * inv_det
        if v < 0.0 or u + v > 1.0:
            return None
        t = float(np.dot(edge2, qvec)) * inv_det
        return t if t > 1e-6 else None

    def _screen_ray(self, sx: float, sy: float):
        """World-space ray for a widget-local pixel (logical coords)."""
        dpr = self.devicePixelRatioF()
        vx = sx * dpr
        vy = sy * dpr
        vp = getattr(self, "_pick_viewport", None)
        if vp is not None and vp[2] > 0 and vp[3] > 0:
            w, h = float(vp[2]), float(vp[3])
        else:
            w, h = float(self._vp_w), float(self._vp_h)
        ndc_x = 2.0 * vx / w - 1.0
        ndc_y = 1.0 - 2.0 * vy / h
        aspect = w / h
        tan_f = math.tan(math.radians(22.5))  # half of 45° FOV

        yaw = math.radians(self.cam_yaw)
        pitch = math.radians(self.cam_pitch)
        cx = self.cam_target[0] + self.cam_dist * math.cos(pitch) * math.sin(yaw)
        cy = self.cam_target[1] + self.cam_dist * math.sin(pitch)
        cz = self.cam_target[2] + self.cam_dist * math.cos(pitch) * math.cos(yaw)
        eye = np.array([cx, cy, cz], dtype=np.float64)
        target = np.array(self.cam_target, dtype=np.float64)
        forward = target - eye
        forward /= np.linalg.norm(forward)
        world_up = np.array([0.0, 1.0, 0.0], dtype=np.float64)
        right = np.cross(forward, world_up)
        rn = np.linalg.norm(right)
        if rn < 1e-8:
            right = np.array([1.0, 0.0, 0.0], dtype=np.float64)
        else:
            right /= rn
        up = np.cross(right, forward)
        up /= np.linalg.norm(up)
        direction = forward + right * (ndc_x * tan_f * aspect) + up * (ndc_y * tan_f)
        direction /= np.linalg.norm(direction)
        return eye, direction

    def _pick_chunk_at(self, sx: float, sy: float) -> int:
        """Return chunk index under cursor (mesh triangles + floor fallback)."""
        if not self.world or not self.world.chunks:
            return -1
        orig, direction = self._screen_ray(sx, sy)
        best_i, best_t = -1, float("inf")
        half = CHUNK_WORLD_UNIT / 2.0
        for i, chunk in enumerate(self.world.chunks):
            ox = chunk.grid_x * CHUNK_WORLD_UNIT
            oz = chunk.grid_z * CHUNK_WORLD_UNIT
            hit = False
            for ti in range(len(chunk.vertices) // 3):
                a, b, c = (chunk.vertices[ti * 3 + k] for k in range(3))
                v0 = np.array([ox + a.x / FP, a.y / FP, oz + a.z / FP])
                v1 = np.array([ox + b.x / FP, b.y / FP, oz + b.z / FP])
                v2 = np.array([ox + c.x / FP, c.y / FP, oz + c.z / FP])
                t = self._ray_triangle(orig, direction, v0, v1, v2)
                if t is not None and t < best_t:
                    best_t, best_i = t, i
                    hit = True
            if hit:
                continue
            # Empty chunk: test ground quad
            y0 = 0.0
            if abs(direction[1]) > 1e-8:
                t = (y0 - orig[1]) / direction[1]
                if t > 1e-6:
                    wx = orig[0] + direction[0] * t
                    wz = orig[2] + direction[2] * t
                    if ox - half <= wx <= ox + half and oz - half <= wz <= oz + half:
                        if t < best_t:
                            best_t, best_i = t, i
        return best_i

    # ---- Mouse ----
    def mousePressEvent(self, e):
        self._last_mouse = e.position()
        if e.button() == Qt.MouseButton.LeftButton:
            idx = self._pick_chunk_at(e.position().x(), e.position().y())
            if idx >= 0:
                self.object_clicked.emit(idx)
        if e.button() == Qt.MouseButton.RightButton:  self._rmb_down = True
        if e.button() == Qt.MouseButton.MiddleButton: self._mmb_down = True

    def mouseReleaseEvent(self, e):
        if e.button() == Qt.MouseButton.RightButton:  self._rmb_down = False
        if e.button() == Qt.MouseButton.MiddleButton: self._mmb_down = False

    def mouseMoveEvent(self, e):
        if self._last_mouse is None:
            self._last_mouse = e.position(); return
        dx = e.position().x() - self._last_mouse.x()
        dy = e.position().y() - self._last_mouse.y()
        self._last_mouse = e.position()
        if self._rmb_down:
            self.cam_yaw   += dx * 0.4
            self.cam_pitch += dy * 0.3
            self.cam_pitch  = max(-89, min(89, self.cam_pitch))
            self.update()
        elif self._mmb_down:
            yaw   = math.radians(self.cam_yaw)
            right   = [math.cos(yaw), 0, -math.sin(yaw)]
            spd = self.cam_dist * 0.005
            self.cam_target[0] -= right[0] * dx * spd
            self.cam_target[2] -= right[2] * dx * spd
            self.cam_target[1] += dy * spd
            self.update()

    def wheelEvent(self, e):
        delta = e.angleDelta().y()
        self.cam_dist *= 0.9 if delta > 0 else 1.1
        self.cam_dist  = max(2, min(1000, self.cam_dist))
        self.update()

    def focus_on_chunk(self, chunk: DSChunk):
        self.cam_target = [chunk.grid_x * CHUNK_WORLD_UNIT, 0.0,
                           chunk.grid_z * CHUNK_WORLD_UNIT]
        self.cam_dist = 50.0
        self.update()


# ---------------------------------------------------------------------------
# Floor Tile Painter  (widget embedded in Inspector)
# ---------------------------------------------------------------------------
class FloorPainter(QWidget):
    """
    Visual N×N grid.  Click a cell to paint the currently selected texture.
    The active tile_size in screen pixels is auto-computed from widget size.
    """
    tile_painted = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.chunk:  DSChunk  | None = None
        self.chunks: list[DSChunk]     = []
        self.world:  WorldFile| None = None
        self.active_tex_id = NO_TEX
        self.active_r, self.active_g, self.active_b = 180, 180, 180
        self._hover = (-1, -1)
        self._tex_pixmaps: dict[int, QPixmap] = {}
        self.setMouseTracking(True)
        self.setMinimumSize(160, 160)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

    def set_chunk(self, chunk, world):
        self.set_chunks([chunk] if chunk else [], world)

    def set_chunks(self, chunks, world):
        self.chunks = list(chunks)
        self.chunk = self.chunks[0] if self.chunks else None
        self.world = world
        self._tex_pixmaps.clear()
        self.update()

    def invalidate_tex_cache(self):
        self._tex_pixmaps.clear()
        self.update()

    def _cell_size(self):
        return min(self.width(), self.height()) / FLOOR_TILES

    def _cell_at(self, x, y):
        cs = self._cell_size()
        tx = int(x / cs)
        tz = int(y / cs)
        if 0 <= tx < FLOOR_TILES and 0 <= tz < FLOOR_TILES:
            return tx, tz
        return -1, -1

    def _get_pixmap(self, tex_id):
        if tex_id in self._tex_pixmaps:
            return self._tex_pixmaps[tex_id]
        if self.world:
            dtex = self.world.texture_by_id(tex_id)
            if dtex:
                img = dtex.get_pil()
                if img:
                    img = img.resize((32, 32), Image.NEAREST).convert("RGBA")
                    arr = np.array(img, dtype=np.uint8)
                    h, w, _ = arr.shape
                    qimg = QImage(arr.data, w, h, 4*w, QImage.Format.Format_RGBA8888)
                    pm = QPixmap.fromImage(qimg)
                    self._tex_pixmaps[tex_id] = pm
                    return pm
        return None

    def paintEvent(self, e):
        p = QPainter(self)
        if not self.chunks:
            p.fillRect(self.rect(), QColor(30, 32, 40))
            p.setPen(QColor(100, 100, 120))
            p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "No chunk selected")
            return

        # Floor is a single quad — show first chunk as preview
        tile = self.chunks[0].floor[0][0]
        rw, rh = self.width(), self.height()

        p.fillRect(0, 0, rw, rh, QColor(tile.r, tile.g, tile.b))

        pm = self._get_pixmap(tile.tex_id) if tile.tex_id != NO_TEX else None
        if pm:
            p.drawPixmap(0, 0, rw, rh, pm)

        p.setPen(QColor(40, 44, 52))
        p.drawRect(0, 0, rw - 1, rh - 1)

        # Label
        p.setPen(QColor(220, 220, 220))
        label = "No texture" if tile.tex_id == NO_TEX else f"Tex {tile.tex_id}"
        multi = f" ({len(self.chunks)} chunks)" if len(self.chunks) > 1 else ""
        p.drawText(self.rect(), Qt.AlignmentFlag.AlignBottom | Qt.AlignmentFlag.AlignHCenter,
                   f" {label}{multi} | RGB({tile.r},{tile.g},{tile.b}) ")

    def mouseMoveEvent(self, e):
        self._hover = self._cell_at(e.position().x(), e.position().y())
        self.update()
        if e.buttons() & Qt.MouseButton.LeftButton:
            self._paint_at(e.position().x(), e.position().y())

    def mousePressEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton:
            self._paint_at(e.position().x(), e.position().y())
        elif e.button() == Qt.MouseButton.RightButton:
            self._paint_at(e.position().x(), e.position().y(), clear=True)

    def leaveEvent(self, e):
        self._hover = (-1, -1)
        self.update()

    def _paint_at(self, x, y, clear=False):
        targets = self.chunks if self.chunks else ([self.chunk] if self.chunk else [])
        if not targets:
            return
        for chunk in targets:
            for tz in range(FLOOR_TILES):
                for tx in range(FLOOR_TILES):
                    if clear:
                        chunk.floor[tz][tx] = FloorTile()
                    else:
                        tile = chunk.floor[tz][tx]
                        tile.tex_id = self.active_tex_id
                        # If a texture is selected, use white so it isn't tinted
                        if self.active_tex_id != NO_TEX:
                            tile.r, tile.g, tile.b = 255, 255, 255
                        else:
                            tile.r = self.active_r
                            tile.g = self.active_g
                            tile.b = self.active_b
        self.update()
        self.tile_painted.emit()


# ---------------------------------------------------------------------------
# Texture strip (small palette bar for the painter)
# ---------------------------------------------------------------------------
class TexturePalette(QWidget):
    """Horizontal strip of texture thumbnails — click to select active texture."""
    texture_selected = pyqtSignal(int)   # tex_id

    def __init__(self, parent=None):
        super().__init__(parent)
        self.world: WorldFile | None = None
        self.selected_id = NO_TEX
        self._thumbs: list[tuple[int, QPixmap]] = []   # (tex_id, pixmap)
        self.setFixedHeight(40)
        self.setMouseTracking(True)

    def set_world(self, world):
        self.world = world
        self.refresh()

    def refresh(self):
        self._thumbs.clear()
        if not self.world:
            self.update(); return
        for tex in self.world.textures:
            img = tex.get_pil()
            if img:
                img = img.resize((32, 32), Image.NEAREST).convert("RGBA")
                arr = np.array(img, dtype=np.uint8)
                qimg = QImage(arr.data, 32, 32, 4*32, QImage.Format.Format_RGBA8888)
                self._thumbs.append((tex.tex_id, QPixmap.fromImage(qimg)))
            else:
                self._thumbs.append((tex.tex_id, None))
        self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(26, 28, 34))
        x = 2
        # "No texture" slot
        p.setPen(QColor(200,200,220) if self.selected_id == NO_TEX else QColor(80,80,100))
        p.fillRect(x, 4, 32, 32, QColor(60,40,40) if self.selected_id == NO_TEX else QColor(35,35,40))
        p.drawRect(x, 4, 31, 31)
        p.setPen(QColor(180,180,200))
        p.drawText(x, 4, 32, 32, Qt.AlignmentFlag.AlignCenter, "∅")
        x += 36
        for tid, pm in self._thumbs:
            sel = (tid == self.selected_id)
            p.fillRect(x, 4, 32, 32, QColor(50,50,55) if sel else QColor(30,32,38))
            if pm:
                p.drawPixmap(x, 4, 32, 32, pm)
            p.setPen(QColor(255,220,80) if sel else QColor(60,65,75))
            p.drawRect(x, 4, 31, 31)
            x += 36

    def mousePressEvent(self, e):
        if e.button() != Qt.MouseButton.LeftButton:
            return
        x = e.position().x()
        # No-tex slot
        if 2 <= x < 34:
            self.selected_id = NO_TEX
            self.texture_selected.emit(NO_TEX)
            self.update(); return
        slot = int((x - 2) / 36) - 1
        if 0 <= slot < len(self._thumbs):
            tid = self._thumbs[slot][0]
            self.selected_id = tid
            self.texture_selected.emit(tid)
            self.update()


# ---------------------------------------------------------------------------
# Collapsible section widget — used inside the context-sensitive right panel
# ---------------------------------------------------------------------------
class CollapsibleSection(QWidget):
    """A titled, collapsible group for the right panel."""

    def __init__(self, title: str, parent=None):
        super().__init__(parent)
        self._collapsed = False
        self._build(title)

    def _build(self, title: str):
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 2)
        outer.setSpacing(0)

        # Header button
        self._toggle_btn = QPushButton(f"▾  {title}")
        self._toggle_btn.setCheckable(True)
        self._toggle_btn.setChecked(True)
        self._toggle_btn.setStyleSheet(
            "QPushButton { text-align:left; font-size:11px; font-weight:bold;"
            "  color:#9ab; background:#1e2130; border:none;"
            "  border-top:1px solid #2a2d35; padding:4px 6px; }"
            "QPushButton:hover { background:#252838; }"
        )
        self._toggle_btn.toggled.connect(self._on_toggle)
        outer.addWidget(self._toggle_btn)

        # Content container
        self._content = QWidget()
        self._content.setStyleSheet(
            "QWidget { background:#181a22; }"
        )
        outer.addWidget(self._content)

        self._inner = QVBoxLayout(self._content)
        self._inner.setContentsMargins(4, 4, 4, 4)
        self._inner.setSpacing(3)

    def _on_toggle(self, checked: bool):
        self._content.setVisible(checked)
        arrow = "▾" if checked else "▸"
        txt = self._toggle_btn.text()
        # Replace first char (arrow)
        self._toggle_btn.setText(arrow + txt[1:])

    def add_widget(self, w: QWidget):
        self._inner.addWidget(w)

    def add_layout(self, lay):
        self._inner.addLayout(lay)

    def inner_layout(self) -> QVBoxLayout:
        return self._inner


# ---------------------------------------------------------------------------
# Inspector Panel  (context-sensitive, section-based)
# ---------------------------------------------------------------------------
class InspectorPanel(QWidget):
    changed = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._chunk:  DSChunk   | None = None
        self._chunks: list             = []
        self._world:  WorldFile | None = None
        self._viewport: Viewport | None = None
        self._build_ui()

    def set_viewport(self, vp: Viewport):
        self._viewport = vp

    # ------------------------------------------------------------------
    # Multi-chunk entry point  (called by MainWindow on every selection change)
    # ------------------------------------------------------------------
    def set_chunks(self, chunks: list):
        """Set the active selection.  chunks is a list of DSChunk objects."""
        self._chunks = chunks
        self._chunk  = chunks[0] if len(chunks) == 1 else None

        if not chunks:
            self.title.setText("Nothing selected")
            self._update_visible_sections(False)
            return

        if len(chunks) == 1:
            self.set_chunk(chunks[0])
            return

        # ── Multi-select UI ──
        self._update_visible_sections(True)
        self.title.setText(f"{len(chunks)} chunks selected")

        # Hide stats (not as useful for multi-select) but keep floor for batch paint
        self._sec_floor.setVisible(True)
        self._sec_stats.setVisible(False)
        self.floor_painter.setVisible(True)
        self.floor_painter.set_chunks(chunks, self._world)

        # Show common grid offset if all share the same value, else blank
        def _common(getter):
            vals = [getter(c) for c in chunks]
            return vals[0] if len(set(vals)) == 1 else None

        for spin, getter in [
            (self.gx_spin,  lambda c: c.grid_x),
            (self.gz_spin,  lambda c: c.grid_z),
        ]:
            v = _common(getter)
            spin.blockSignals(True)
            spin.setValue(v if v is not None else 0)
            spin.blockSignals(False)
            spin.setSpecialValueText("" if v is not None else "—")

        for spin, getter in [
            (self.wx_spin, lambda c: c.world_x),
            (self.wy_spin, lambda c: c.world_y),
            (self.wz_spin, lambda c: c.world_z),
        ]:
            v = _common(getter)
            spin.blockSignals(True)
            spin.setValue(v if v is not None else 0.0)
            spin.blockSignals(False)

        self.chunk_name.setText("" if _common(lambda c: c.name) is None else chunks[0].name)
        self.poly_label.setText(str(sum(c.poly_count() for c in chunks)))
        self._refresh_model_tex_combo()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------
    def _build_ui(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # ── Title bar ──
        self.title = QLabel("Nothing selected")
        self.title.setStyleSheet(
            "font-weight:bold; font-size:13px; color:#c8ccd4;"
            "background:#13151a; padding:6px 8px;"
            "border-bottom:1px solid #2a2d35;")
        root.addWidget(self.title)

        # ── Scroll area holds all collapsible sections ──
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setStyleSheet("QScrollArea { border:none; background:#13151a; }")
        root.addWidget(scroll, 1)

        self._scroll_widget = QWidget()
        self._scroll_widget.setStyleSheet("background:#13151a;")
        self._scroll_layout = QVBoxLayout(self._scroll_widget)
        self._scroll_layout.setContentsMargins(0, 0, 0, 0)
        self._scroll_layout.setSpacing(0)
        self._scroll_layout.addStretch(1)
        scroll.setWidget(self._scroll_widget)

        # Build all sections (some hidden by default)
        self._build_chunk_section()
        self._build_model_section()
        self._build_floor_section()
        self._build_textures_section()
        self._build_stats_section()

        # Default state: no selection → show only textures
        self._update_visible_sections(chunk_selected=False)

    # ── Section: Chunk ──
    def _build_chunk_section(self):
        self._sec_chunk = CollapsibleSection("Chunk")
        il = self._sec_chunk.inner_layout()

        form_w = QWidget()
        cf = QFormLayout(form_w)
        cf.setContentsMargins(0,0,0,0)
        cf.setSpacing(3)

        self.gx_spin = QSpinBox(); self.gx_spin.setRange(-9999,9999)
        self.gz_spin = QSpinBox(); self.gz_spin.setRange(-9999,9999)
        self.chunk_name = QLineEdit()
        self.poly_label = QLabel("0")
        cf.addRow("Grid X:",  self.gx_spin)
        cf.addRow("Grid Z:",  self.gz_spin)
        cf.addRow("Name:",    self.chunk_name)
        cf.addRow("Polys:",   self.poly_label)

        apply_btn = QPushButton("Apply Grid / Name")
        apply_btn.clicked.connect(self._apply_chunk)
        cf.addRow("", apply_btn)

        il.addWidget(form_w)
        self._insert_section(self._sec_chunk)

    # ── Section: Model Transform ──
    def _build_model_section(self):
        self._sec_model = CollapsibleSection("Model Transform")
        il = self._sec_model.inner_layout()

        form_w = QWidget()
        mf = QFormLayout(form_w)
        mf.setContentsMargins(0,0,0,0)
        mf.setSpacing(3)

        self.wx_spin = QDoubleSpinBox(); self.wx_spin.setRange(-9999,9999); self.wx_spin.setDecimals(3)
        self.wy_spin = QDoubleSpinBox(); self.wy_spin.setRange(-9999,9999); self.wy_spin.setDecimals(3)
        self.wz_spin = QDoubleSpinBox(); self.wz_spin.setRange(-9999,9999); self.wz_spin.setDecimals(3)
        mf.addRow("Offset X:", self.wx_spin)
        mf.addRow("Offset Y:", self.wy_spin)
        mf.addRow("Offset Z:", self.wz_spin)

        apply_xform_btn = QPushButton("Apply Offset")
        apply_xform_btn.clicked.connect(self._apply_chunk)
        mf.addRow("", apply_xform_btn)

        # Texture assignment for model verts
        self.model_tex_combo = QComboBox()
        mf.addRow("Texture:", self.model_tex_combo)
        apply_tex_btn = QPushButton("Apply Texture to Model")
        apply_tex_btn.clicked.connect(self._apply_model_texture)
        mf.addRow("", apply_tex_btn)

        il.addWidget(form_w)
        self._insert_section(self._sec_model)

    # ── Section: Floor ──
    def _build_floor_section(self):
        self._sec_floor = CollapsibleSection("Floor")
        il = self._sec_floor.inner_layout()

        # Brush row
        brush_row = QHBoxLayout()
        brush_lbl = QLabel("Brush:")
        brush_lbl.setFixedWidth(38)
        brush_row.addWidget(brush_lbl)

        self.brush_color_btn = QPushButton()
        self.brush_color_btn.setFixedSize(24, 24)
        self._brush_color = QColor(180, 180, 180)
        self._update_brush_btn()
        self.brush_color_btn.clicked.connect(self._pick_color)
        brush_row.addWidget(self.brush_color_btn)

        fill_btn = QPushButton("Fill All")
        fill_btn.setFixedHeight(22)
        fill_btn.clicked.connect(self._fill_all)
        brush_row.addWidget(fill_btn)

        clear_btn = QPushButton("Clear")
        clear_btn.setFixedHeight(22)
        clear_btn.clicked.connect(self._clear_floor)
        brush_row.addWidget(clear_btn)

        il.addLayout(brush_row)

        # Texture palette (floor brush)
        self.palette = TexturePalette()
        self.palette.texture_selected.connect(self._on_tex_selected)
        il.addWidget(self.palette)

        # Tiling
        tiling_row = QHBoxLayout()
        tiling_row.addWidget(QLabel("Tile U:"))
        self.floor_tile_u_spin = QDoubleSpinBox()
        self.floor_tile_u_spin.setRange(0.1, 64.0)
        self.floor_tile_u_spin.setSingleStep(0.25)
        self.floor_tile_u_spin.setDecimals(2)
        self.floor_tile_u_spin.setValue(1.0)
        tiling_row.addWidget(self.floor_tile_u_spin)
        tiling_row.addWidget(QLabel("V:"))
        self.floor_tile_v_spin = QDoubleSpinBox()
        self.floor_tile_v_spin.setRange(0.1, 64.0)
        self.floor_tile_v_spin.setSingleStep(0.25)
        self.floor_tile_v_spin.setDecimals(2)
        self.floor_tile_v_spin.setValue(1.0)
        tiling_row.addWidget(self.floor_tile_v_spin)
        apply_tiling_btn = QPushButton("Apply")
        apply_tiling_btn.setFixedWidth(48)
        apply_tiling_btn.clicked.connect(self._apply_floor_tiling)
        tiling_row.addWidget(apply_tiling_btn)
        il.addLayout(tiling_row)

        # Floor painter
        self.floor_painter = FloorPainter()
        self.floor_painter.tile_painted.connect(self._on_tile_painted)
        self.floor_painter.setMinimumHeight(160)
        il.addWidget(self.floor_painter)

        self._insert_section(self._sec_floor)

    # ── Section: Textures ──
    def _build_textures_section(self):
        self._sec_textures = CollapsibleSection("Textures")
        il = self._sec_textures.inner_layout()

        import_btn = QPushButton("⊕  Import Texture(s)…")
        import_btn.clicked.connect(self._import_textures)
        il.addWidget(import_btn)

        self.tex_list_widget = QListWidget()
        self.tex_list_widget.setMinimumHeight(80)
        self.tex_list_widget.currentRowChanged.connect(self._on_tex_list_select)
        il.addWidget(self.tex_list_widget)

        # Preview + props
        self.tex_preview = QLabel()
        self.tex_preview.setFixedHeight(72)
        self.tex_preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.tex_preview.setStyleSheet("background:#1a1c20; border:1px solid #333;")
        il.addWidget(self.tex_preview)

        pf = QFormLayout()
        pf.setContentsMargins(0,0,0,0)
        pf.setSpacing(2)
        self.tp_id    = QLabel("-")
        self.tp_size  = QLabel("-")
        self.tp_fmt   = QLabel("-")
        self.tp_bytes = QLabel("-")
        pf.addRow("ID:",     self.tp_id)
        pf.addRow("Size:",   self.tp_size)
        pf.addRow("Format:", self.tp_fmt)
        pf.addRow("Bytes:",  self.tp_bytes)
        il.addLayout(pf)

        fmt_row = QHBoxLayout()
        self.dsify_fmt = QComboBox()
        for name in LIBNDS_FORMATS:
            self.dsify_fmt.addItem(name, LIBNDS_FORMATS[name])
        self.dsify_fmt.setCurrentIndex(1)
        fmt_row.addWidget(QLabel("DS fmt:"))
        fmt_row.addWidget(self.dsify_fmt)
        il.addLayout(fmt_row)

        size_row = QHBoxLayout()
        self.dsify_maxsize = QComboBox()
        for s in [8,16,32,64,128,256]:
            self.dsify_maxsize.addItem(f"{s}×{s}", s)
        self.dsify_maxsize.setCurrentIndex(3)
        size_row.addWidget(QLabel("Max px:"))
        size_row.addWidget(self.dsify_maxsize)
        il.addLayout(size_row)

        dsify_btn = QPushButton("DS-ify selected")
        dsify_btn.clicked.connect(self._dsify_selected)
        il.addWidget(dsify_btn)

        dsify_all_btn = QPushButton("DS-ify all")
        dsify_all_btn.clicked.connect(self._dsify_all)
        il.addWidget(dsify_all_btn)

        del_btn = QPushButton("Remove selected")
        del_btn.clicked.connect(self._remove_tex)
        il.addWidget(del_btn)

        self._insert_section(self._sec_textures)

    # ── Section: Stats ──
    def _build_stats_section(self):
        self._sec_stats = CollapsibleSection("Stats")
        il = self._sec_stats.inner_layout()

        sf = QFormLayout()
        sf.setContentsMargins(0,0,0,0)
        sf.setSpacing(2)
        self.stat_verts = QLabel("0")
        self.stat_polys = QLabel("0")
        sf.addRow("Vertices:",  self.stat_verts)
        sf.addRow("Triangles:", self.stat_polys)
        il.addLayout(sf)

        self._insert_section(self._sec_stats)

    def _insert_section(self, sec: CollapsibleSection):
        """Insert section before the trailing stretch."""
        lay = self._scroll_layout
        # Remove the stretch, insert section, re-add stretch
        stretch_item = lay.takeAt(lay.count() - 1)
        lay.addWidget(sec)
        lay.addStretch(1)

    # ------------------------------------------------------------------
    # Context switching
    # ------------------------------------------------------------------
    def _update_visible_sections(self, chunk_selected: bool):
        self._sec_chunk.setVisible(chunk_selected)
        self._sec_model.setVisible(chunk_selected)
        self._sec_floor.setVisible(chunk_selected)
        self._sec_stats.setVisible(chunk_selected)
        # Textures always visible

    # ------------------------------------------------------------------
    def set_world(self, world: WorldFile | None):
        self._world = world
        self.palette.set_world(world)
        self.floor_painter.world = world
        self._refresh_tex_list()
        self._refresh_model_tex_combo()

    def set_chunk(self, chunk: DSChunk | None):
        self._chunks = [chunk] if chunk else []
        self._chunk = chunk
        self.floor_painter.set_chunks(self._chunks, self._world)
        self._refresh_model_tex_combo()

        if chunk is None:
            self.title.setText("Nothing selected")
            self._update_visible_sections(False)
            return

        self._update_visible_sections(True)
        self.floor_painter.setVisible(True)
        self.title.setText(f"Chunk  [{chunk.grid_x}, {chunk.grid_z}]  —  {chunk.name}")
        self.gx_spin.setValue(chunk.grid_x)
        self.gz_spin.setValue(chunk.grid_z)
        self.chunk_name.setText(chunk.name)
        self.wx_spin.setValue(chunk.world_x)
        self.wy_spin.setValue(chunk.world_y)
        self.wz_spin.setValue(chunk.world_z)
        self.floor_tile_u_spin.setValue(getattr(chunk, 'floor_tile_u', 1.0))
        self.floor_tile_v_spin.setValue(getattr(chunk, 'floor_tile_v', 1.0))
        polys = chunk.poly_count()
        self.poly_label.setText(str(polys))
        self.stat_verts.setText(str(len(chunk.vertices)))
        self.stat_polys.setText(str(polys))

    # ---- Chunk / Model apply ----
    def _apply_chunk(self):
        targets = self._chunks if self._chunks else ([self._chunk] if self._chunk else [])
        if not targets: return
        for c in targets:
            c.grid_x  = self.gx_spin.value()
            c.grid_z  = self.gz_spin.value()
            if self.chunk_name.text():
                c.name = self.chunk_name.text()
            c.world_x = self.wx_spin.value()
            c.world_y = self.wy_spin.value()
            c.world_z = self.wz_spin.value()
        self.changed.emit()

    def _refresh_model_tex_combo(self):
        self.model_tex_combo.clear()
        self.model_tex_combo.addItem("(none)", NO_TEX)
        if self._world:
            for tex in self._world.textures:
                self.model_tex_combo.addItem(f"[{tex.tex_id}] {tex.name}", tex.tex_id)

    def _apply_model_texture(self):
        """Apply selected texture to all object verts in all selected chunks."""
        targets = self._chunks if self._chunks else ([self._chunk] if self._chunk else [])
        if not targets: return
        tex_id = self.model_tex_combo.currentData()
        tex_w = tex_h = 1
        if self._world and tex_id != NO_TEX:
            dtex = self._world.texture_by_id(tex_id)
            if dtex:
                tex_w = dtex.width
                tex_h = dtex.height
        any_model = False
        for chunk in targets:
            obj_verts = chunk.vertices[6:]
            if not obj_verts:
                continue
            any_model = True
            xs = [v.x for v in obj_verts]; zs = [v.z for v in obj_verts]
            min_x, max_x = min(xs), max(xs)
            min_z, max_z = min(zs), max(zs)
            rng_x = max(max_x - min_x, 1); rng_z = max(max_z - min_z, 1)
            for v in obj_verts:
                v.tex_id = tex_id
                v.u = clamp_s16(int(((v.x - min_x) / rng_x) * tex_w * 16))
                v.v = clamp_s16(int(((v.z - min_z) / rng_z) * tex_h * 16))
        if not any_model:
            QMessageBox.information(self, "No model", "None of the selected chunks have imported model verts.")
            return
        if self._viewport:
            self._viewport.update()
        self.changed.emit()

    # ---- Floor ----
    def _apply_floor_tiling(self):
        targets = self._chunks if self._chunks else ([self._chunk] if self._chunk else [])
        if not targets: return
        u = self.floor_tile_u_spin.value()
        v = self.floor_tile_v_spin.value()
        for c in targets:
            c.floor_tile_u = u
            c.floor_tile_v = v
            c.bake_floor_to_vertices(self._world)
        if self._viewport: self._viewport.update()
        self.changed.emit()

    def _on_tex_selected(self, tex_id: int):
        self.floor_painter.active_tex_id = tex_id

    def _on_tile_painted(self):
        targets = self._chunks if self._chunks else ([self._chunk] if self._chunk else [])
        for c in targets:
            c.bake_floor_to_vertices(self._world)
        if self._viewport:
            self._viewport.update()

    def _update_brush_btn(self):
        c = self._brush_color
        self.brush_color_btn.setStyleSheet(
            f"background:{c.name()}; border:1px solid #555; border-radius:2px;")

    def _pick_color(self):
        c = QColorDialog.getColor(self._brush_color, self, "Pick Tile Colour")
        if c.isValid():
            self._brush_color = c
            self._update_brush_btn()
            self.floor_painter.active_r = c.red()
            self.floor_painter.active_g = c.green()
            self.floor_painter.active_b = c.blue()

    def _fill_all(self):
        targets = self._chunks if self._chunks else ([self._chunk] if self._chunk else [])
        if not targets:
            return
        for chunk in targets:
            for tz in range(FLOOR_TILES):
                for tx in range(FLOOR_TILES):
                    t = chunk.floor[tz][tx]
                    t.tex_id = self.floor_painter.active_tex_id
                    if self.floor_painter.active_tex_id != NO_TEX:
                        t.r, t.g, t.b = 255, 255, 255
                    else:
                        t.r = self.floor_painter.active_r
                        t.g = self.floor_painter.active_g
                        t.b = self.floor_painter.active_b
            chunk.bake_floor_to_vertices(self._world)
        self.floor_painter.update()
        if self._viewport:
            self._viewport.update()

    def _clear_floor(self):
        targets = self._chunks if self._chunks else ([self._chunk] if self._chunk else [])
        if not targets: return
        for chunk in targets:
            for tz in range(FLOOR_TILES):
                for tx in range(FLOOR_TILES):
                    chunk.floor[tz][tx] = FloorTile()
            chunk.bake_floor_to_vertices(self._world)
        self.floor_painter.update()
        if self._viewport: self._viewport.update()

    # ---- Textures tab ----
    def _refresh_tex_list(self):
        self.tex_list_widget.clear()
        if not self._world: return
        for tex in self._world.textures:
            self.tex_list_widget.addItem(
                f"[{tex.tex_id}] {tex.name}  {tex.width}×{tex.height}")

    def _on_tex_list_select(self, row):
        if not self._world or row < 0 or row >= len(self._world.textures):
            return
        tex = self._world.textures[row]
        self.tp_id.setText(str(tex.tex_id))
        self.tp_size.setText(f"{tex.width}×{tex.height}")
        fmt_name = next((k for k,v in LIBNDS_FORMATS.items() if v==tex.fmt), str(tex.fmt))
        self.tp_fmt.setText(fmt_name)
        self.tp_bytes.setText(f"{len(tex.data):,} B")
        img = tex.get_pil()
        if img:
            self._show_tex_preview(img)
        else:
            self.tex_preview.setText("No preview")

    def _show_tex_preview(self, img: Image.Image):
        img = img.convert("RGBA").resize((80, 80), Image.NEAREST)
        arr = np.array(img, dtype=np.uint8)
        qimg = QImage(arr.data, 80, 80, 4*80, QImage.Format.Format_RGBA8888)
        self.tex_preview.setPixmap(QPixmap.fromImage(qimg))

    def _import_textures(self):
        if not self._world:
            QMessageBox.information(self, "No world", "Open or create a world first.")
            return
        paths, _ = QFileDialog.getOpenFileNames(
            self, "Import Textures", "",
            "Images (*.png *.jpg *.jpeg *.bmp *.tga *.tiff *.tif *.gif *.webp "
            "*.psd *.ico *.hdr *.exr *.dds *.pcx *.ppm *.pgm *.pbm *.xbm "
            "*.jp2 *.j2k);;All Files (*)")
        fmt      = self.dsify_fmt.currentData()
        max_size = self.dsify_maxsize.currentData()
        added = 0
        errors = []
        for path in paths:
            try:
                # Open with PIL — handles virtually all formats.
                # Save to an in-memory PNG first so even exotic formats
                # (PSD, HDR, EXR, DDS) are normalised before DSify.
                raw_img = Image.open(path)
                raw_img.load()   # force decode (some formats are lazy)
                buf = io.BytesIO()
                raw_img.convert("RGBA").save(buf, format="PNG")
                buf.seek(0)
                img = Image.open(buf).convert("RGBA")
                dsimg, raw, nw, nh = dsify_image(img, max_size, fmt)
                tex = DSTexture(
                    tex_id=self._world.new_tex_id(),
                    width=nw, height=nh, fmt=fmt, data=raw,
                    name=Path(path).stem)
                tex.pil_img = dsimg
                self._world.textures.append(tex)
                added += 1
            except Exception as ex:
                errors.append(f"{Path(path).name}: {ex}")
        if errors:
            QMessageBox.warning(self, "Some imports failed",
                                "\n".join(errors))
        if added:
            self._refresh_tex_list()
            self._refresh_model_tex_combo()
            self.palette.refresh()
            self.floor_painter.invalidate_tex_cache()
            if self._viewport:
                for tex in self._world.textures[-added:]:
                    self._viewport.invalidate_texture(tex.tex_id)
                self._viewport.update()

    def _dsify_selected(self):
        row = self.tex_list_widget.currentRow()
        if not self._world or row < 0: return
        tex = self._world.textures[row]
        self._do_dsify(tex)
        self._refresh_tex_list()
        self.tex_list_widget.setCurrentRow(row)

    def _dsify_all(self):
        if not self._world: return
        for tex in self._world.textures:
            self._do_dsify(tex)
        self._refresh_tex_list()
        self.palette.refresh()
        self.floor_painter.invalidate_tex_cache()
        if self._viewport:
            self._viewport._gl_textures.clear()
            self._viewport.update()

    def _do_dsify(self, tex: DSTexture):
        fmt      = self.dsify_fmt.currentData()
        max_size = self.dsify_maxsize.currentData()
        img = tex.get_pil()
        if img is None:
            return
        dsimg, raw, nw, nh = dsify_image(img, max_size, fmt)
        tex.width   = nw
        tex.height  = nh
        tex.fmt     = fmt
        tex.data    = raw
        tex.pil_img = dsimg
        tex._rgba_cache = None
        if self._viewport:
            self._viewport.invalidate_texture(tex.tex_id)
        self.palette.refresh()
        self.floor_painter.invalidate_tex_cache()

    def _remove_tex(self):
        row = self.tex_list_widget.currentRow()
        if not self._world or row < 0: return
        tex = self._world.textures[row]
        if self._viewport:
            self._viewport.invalidate_texture(tex.tex_id)
        self._world.textures.pop(row)
        self._refresh_tex_list()
        self.palette.refresh()
        self.floor_painter.invalidate_tex_cache()
        if self._viewport: self._viewport.update()


# ---------------------------------------------------------------------------
# DSify Dialog (full-world batch)
# ---------------------------------------------------------------------------
class DSifyDialog(QDialog):
    def __init__(self, world: WorldFile, parent=None):
        super().__init__(parent)
        self.world = world
        self.setWindowTitle("DS-ify World")
        self.setMinimumWidth(400)
        self._build_ui()

    def _build_ui(self):
        layout = QVBoxLayout(self)
        info = QLabel(
            "Convert all textures and geometry to NDS hardware limits.\n"
            "This modifies the world in-place — save afterwards.")
        info.setWordWrap(True)
        layout.addWidget(info)

        tex_grp = QGroupBox("Texture limits")
        tf = QFormLayout(tex_grp)
        self.max_size = QComboBox()
        for s in [8,16,32,64,128,256]:
            self.max_size.addItem(f"{s}×{s}", s)
        self.max_size.setCurrentIndex(3)
        tf.addRow("Max dimension:", self.max_size)
        self.tex_fmt = QComboBox()
        for name in LIBNDS_FORMATS:
            self.tex_fmt.addItem(name, LIBNDS_FORMATS[name])
        self.tex_fmt.setCurrentIndex(1)
        tf.addRow("Target format:", self.tex_fmt)
        layout.addWidget(tex_grp)

        geo_grp = QGroupBox("Geometry limits")
        gf = QFormLayout(geo_grp)
        self.max_polys = QSpinBox()
        self.max_polys.setRange(1, 4096)
        self.max_polys.setValue(DS_MAX_POLYS)
        gf.addRow("Max polys per chunk:", self.max_polys)
        self.strip_bottom = QCheckBox("Strip downward-facing faces")
        self.strip_bottom.setChecked(True)
        gf.addRow("", self.strip_bottom)
        layout.addWidget(geo_grp)

        self.progress = QProgressBar(); self.progress.setVisible(False)
        layout.addWidget(self.progress)
        self.status_label = QLabel("")
        layout.addWidget(self.status_label)

        btns = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        btns.accepted.connect(self._run)
        btns.rejected.connect(self.reject)
        layout.addWidget(btns)

    def _run(self):
        max_size = self.max_size.currentData()
        fmt      = self.tex_fmt.currentData()
        max_polys = self.max_polys.value()
        strip_bot = self.strip_bottom.isChecked()

        total = len(self.world.textures) + len(self.world.chunks)
        self.progress.setMaximum(max(total,1))
        self.progress.setValue(0); self.progress.setVisible(True)

        for i, tex in enumerate(self.world.textures):
            self.status_label.setText(f"Texture {i+1}/{len(self.world.textures)}…")
            QApplication.processEvents()
            img = tex.get_pil()
            if img is None: self.progress.setValue(self.progress.value()+1); continue
            dsimg, raw, nw, nh = dsify_image(img, max_size, fmt)
            tex.width=nw; tex.height=nh; tex.fmt=fmt; tex.data=raw; tex.pil_img=dsimg
            self.progress.setValue(self.progress.value()+1)

        for i, chunk in enumerate(self.world.chunks):
            self.status_label.setText(f"Chunk {i+1}/{len(self.world.chunks)}…")
            QApplication.processEvents()
            if len(chunk.vertices)//3 > max_polys:
                chunk.vertices = chunk.vertices[:max_polys*3]
            if strip_bot:
                new_v = []
                for ti in range(len(chunk.vertices)//3):
                    a,b,c = chunk.vertices[ti*3:ti*3+3]
                    if (a.ny+b.ny+c.ny)/3.0/FP > -0.5:
                        new_v += [a,b,c]
                chunk.vertices = new_v
            self.progress.setValue(self.progress.value()+1)

        self.status_label.setText("Done!")
        self.accept()


# ---------------------------------------------------------------------------
# Object Selector (bottom strip)
# ---------------------------------------------------------------------------
class ObjectSelector(QWidget):
    # Emits a list of selected indices (may be empty or have multiple items)
    chunks_selected = pyqtSignal(list)
    chunk_deleted   = pyqtSignal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.world: WorldFile | None = None
        self._build_ui()

    def _build_ui(self):
        layout = QHBoxLayout(self)
        layout.setContentsMargins(4,4,4,4)

        chunk_grp = QGroupBox("Chunks")
        cl = QVBoxLayout(chunk_grp)
        self.chunk_list = QListWidget()
        self.chunk_list.setMaximumHeight(110)
        self.chunk_list.setSelectionMode(QListWidget.SelectionMode.ExtendedSelection)
        self.chunk_list.itemSelectionChanged.connect(self._on_selection_changed)
        self.chunk_list.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.chunk_list.customContextMenuRequested.connect(self._chunk_context)
        cl.addWidget(self.chunk_list)

        btn_row = QHBoxLayout()
        add_chunk = QPushButton("+ Chunk"); add_chunk.clicked.connect(self._add_chunk)
        import_btn = QPushButton("Import Model…"); import_btn.clicked.connect(self._import_model)
        btn_row.addWidget(add_chunk); btn_row.addWidget(import_btn)
        cl.addLayout(btn_row)
        layout.addWidget(chunk_grp, 2)

        tex_grp = QGroupBox("Textures (quick view)")
        tl = QVBoxLayout(tex_grp)
        self.tex_list = QListWidget(); self.tex_list.setMaximumHeight(110)
        tl.addWidget(self.tex_list)
        layout.addWidget(tex_grp, 1)

    def set_world(self, world):
        self.world = world
        self.refresh()

    def refresh(self, keep_selection: bool = True):
        selected: set[int] = set()
        if keep_selection:
            for item in self.chunk_list.selectedItems():
                selected.add(item.data(Qt.ItemDataRole.UserRole))
        self.chunk_list.clear()
        self.tex_list.clear()
        if not self.world:
            return
        for i, c in enumerate(self.world.chunks):
            item = QListWidgetItem(
                f"[{c.grid_x},{c.grid_z}] {c.name}  ({c.poly_count()} polys)")
            item.setData(Qt.ItemDataRole.UserRole, i)
            self.chunk_list.addItem(item)
            if i in selected:
                item.setSelected(True)
        for t in self.world.textures:
            self.tex_list.addItem(f"[{t.tex_id}] {t.name} ({t.width}×{t.height})")

    def _add_chunk(self):
        if not self.world: return
        gx, gz = _spiral_grid_pos(len(self.world.chunks))
        chunk = DSChunk(gx, gz)
        chunk.bake_floor_to_vertices()
        self.world.chunks.append(chunk)
        self.refresh()
        self.chunk_list.setCurrentRow(len(self.world.chunks) - 1)

    def _import_model(self):
        if not self.world:
            QMessageBox.information(self,"No world","Create or open a world first."); return
        if not HAS_TRIMESH:
            QMessageBox.warning(self,"Missing dep","pip install trimesh"); return
        path, _ = QFileDialog.getOpenFileName(
            self,"Import 3D Model","",
            "3D Models (*.obj *.glb *.gltf *.stl *.ply);;All Files (*)")
        if not path: return
        try:
            scale, ok = QInputDialog.getDouble(self,"Scale","World scale:",1.0,0.001,10000,4)
            if not ok: return

            # Texture picker
            tex_id = NO_TEX
            if self.world.textures:
                choices = ["(none — vertex colour)"] + [
                    f"[{t.tex_id}] {t.name}" for t in self.world.textures]
                choice, ok2 = QInputDialog.getItem(
                    self, "Apply Texture", "Texture for imported model:", choices, 0, False)
                if ok2 and choice != choices[0]:
                    idx = choices.index(choice) - 1
                    tex_id = self.world.textures[idx].tex_id

            chunk = import_model_as_chunk(path, tex_id=tex_id, scale=scale,
                                           world=self.world)
            chunk.name = Path(path).stem
            self.world.chunks.append(chunk)
            self.refresh()
            self.chunk_list.setCurrentRow(len(self.world.chunks)-1)
        except Exception as ex:
            QMessageBox.critical(self,"Import failed",str(ex))

    def _on_selection_changed(self):
        idxs = [item.data(Qt.ItemDataRole.UserRole)
                for item in self.chunk_list.selectedItems()]
        self.chunks_selected.emit(idxs)

    def _chunk_context(self, pos):
        selected_rows = sorted(
            [item.data(Qt.ItemDataRole.UserRole)
             for item in self.chunk_list.selectedItems()],
            reverse=True)
        if not selected_rows: return
        row = self.chunk_list.currentRow()
        menu = QMenu(self)
        dup  = menu.addAction("Duplicate")
        dele = menu.addAction(f"Delete ({len(selected_rows)})" if len(selected_rows) > 1 else "Delete")
        act  = menu.exec(self.chunk_list.mapToGlobal(pos))
        if act == dup and self.world and len(selected_rows) == 1:
            nc = copy.deepcopy(self.world.chunks[row])
            nc.grid_z += 1; nc.name += "_copy"
            self.world.chunks.insert(row+1, nc)
            self.refresh()
        elif act == dele and self.world:
            for r in selected_rows:
                self.world.chunks.pop(r)
            self.refresh()
            self.chunk_deleted.emit(selected_rows[-1])


# ---------------------------------------------------------------------------
# Main Window
# ---------------------------------------------------------------------------
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.world: WorldFile | None = None
        self.setWindowTitle("Alone — World Editor")
        self.resize(1360, 860)
        self._build_ui()
        self._build_menu()
        self._apply_style()

    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(0,0,0,0)
        root.setSpacing(0)

        h_split = QSplitter(Qt.Orientation.Horizontal)

        self.viewport = Viewport()
        self.viewport.object_clicked.connect(self._on_viewport_object_clicked)
        h_split.addWidget(self.viewport)

        self.inspector = InspectorPanel()
        self.inspector.set_viewport(self.viewport)
        self.inspector.changed.connect(self._on_inspector_changed)
        self.inspector.setMinimumWidth(180)
        h_split.addWidget(self.inspector)
        h_split.setStretchFactor(0, 3)
        h_split.setStretchFactor(1, 1)
        h_split.setSizes([960, 300])

        v_split = QSplitter(Qt.Orientation.Vertical)
        v_split.addWidget(h_split)

        self.obj_selector = ObjectSelector()
        self.obj_selector.chunks_selected.connect(self._on_chunks_selected)
        self.obj_selector.chunk_deleted.connect(self._on_chunk_deleted)
        self.obj_selector.setMinimumHeight(60)
        v_split.addWidget(self.obj_selector)
        v_split.setStretchFactor(0, 4)
        v_split.setStretchFactor(1, 1)
        v_split.setSizes([660, 160])

        root.addWidget(v_split)

        self.status = QStatusBar()
        self.setStatusBar(self.status)
        self.status.showMessage("No world loaded — File > New or Open")

    def _make_action(self, text, slot, shortcut=None, checkable=False):
        act = QAction(text, self)
        act.triggered.connect(slot)
        if shortcut: act.setShortcut(shortcut)
        if checkable: act.setCheckable(True)
        return act

    def _build_menu(self):
        mb = self.menuBar()

        fm = mb.addMenu("File")
        fm.addAction(self._make_action("New World", self._new_world, QKeySequence.StandardKey.New))
        fm.addAction(self._make_action("Open…",    self._open,      QKeySequence.StandardKey.Open))
        fm.addAction(self._make_action("Save",      self._save,      QKeySequence.StandardKey.Save))
        fm.addAction(self._make_action("Save As…",  self._save_as))
        fm.addSeparator()
        fm.addAction(self._make_action("Export .world…", self._export))
        fm.addSeparator()
        fm.addAction(self._make_action("Quit", self.close, QKeySequence.StandardKey.Quit))

        em = mb.addMenu("Edit")
        em.addAction(self._make_action("DS-ify All…", self._dsify))

        vm = mb.addMenu("View")
        self.act_grid = self._make_action("Show Grid", lambda:None, checkable=True)
        self.act_grid.setChecked(True)
        self.act_grid.triggered.connect(
            lambda c: setattr(self.viewport,"grid_visible",c) or self.viewport.update())
        vm.addAction(self.act_grid)
        self.act_wire = self._make_action("Wireframe", lambda:None, checkable=True)
        self.act_wire.triggered.connect(
            lambda c: setattr(self.viewport,"wireframe",c) or self.viewport.update())
        vm.addAction(self.act_wire)
        vm.addAction(self._make_action("Frame All", self._frame_all))

    def _apply_style(self):
        self.setStyleSheet("""
            QMainWindow, QWidget { background:#13151a; color:#c8ccd4; }
            QMenuBar { background:#1a1c22; border-bottom:1px solid #2a2d35; }
            QMenuBar::item:selected { background:#2a2d35; }
            QMenu { background:#1e2028; border:1px solid #2a2d35; }
            QMenu::item:selected { background:#2e3240; }
            QSplitter::handle { background:#2a2d35; }
            QGroupBox {
                border:1px solid #2a2d35; border-radius:4px;
                margin-top:8px; padding-top:4px;
                font-size:11px; color:#7a7f8e;
            }
            QGroupBox::title { subcontrol-origin:margin; left:8px; }
            QPushButton {
                background:#23262e; border:1px solid #33363f;
                border-radius:4px; padding:3px 8px; color:#c8ccd4;
            }
            QPushButton:hover { background:#2e3240; border-color:#4a6fa5; }
            QPushButton:pressed { background:#1e2128; }
            QListWidget {
                background:#1a1c22; border:1px solid #2a2d35; border-radius:3px;
            }
            QListWidget::item:selected { background:#2e3e5c; color:#e8eaf0; }
            QListWidget::item:hover { background:#23262e; }
            QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
                background:#1e2028; border:1px solid #2a2d35;
                border-radius:3px; padding:2px 6px; color:#c8ccd4;
            }
            QTabWidget::pane { border:1px solid #2a2d35; }
            QTabBar::tab {
                background:#1a1c22; border:1px solid #2a2d35;
                padding:4px 10px; color:#7a7f8e;
            }
            QTabBar::tab:selected { background:#23262e; color:#c8ccd4; }
            QStatusBar { background:#1a1c22; border-top:1px solid #2a2d35; font-size:11px; }
            QProgressBar {
                background:#1a1c22; border:1px solid #2a2d35;
                border-radius:3px; text-align:center;
            }
            QProgressBar::chunk { background:#4a6fa5; border-radius:2px; }
            QDialog { background:#13151a; }
            QScrollBar:vertical { background:#1a1c22; width:8px; }
            QScrollBar::handle:vertical { background:#33363f; border-radius:4px; }
        """)

    # ------------------------------------------------------------------
    def _new_world(self):
        self.world = WorldFile()
        self.viewport.set_world(self.world)
        self.obj_selector.set_world(self.world)
        self.inspector.set_world(self.world)
        self.inspector.set_chunks([])
        self.setWindowTitle("Alone — World Editor  [New World]")
        self.status.showMessage("New world created")

    def _open(self):
        path, _ = QFileDialog.getOpenFileName(
            self,"Open World","",
            "Alone World Files (*.svworld *.world);;All Files (*)")
        if not path: return
        try:
            self.world = WorldFile.load(path)
            self.viewport.set_world(self.world)
            self.obj_selector.set_world(self.world)
            self.inspector.set_world(self.world)
            self.inspector.set_chunks([])
            self.setWindowTitle(f"Alone — World Editor  [{Path(path).name}]")
            self.status.showMessage(
                f"Loaded {len(self.world.chunks)} chunks, "
                f"{len(self.world.textures)} textures  —  {path}")
        except Exception as ex:
            QMessageBox.critical(self,"Open failed",str(ex))

    def _save(self):
        if not self.world: return
        if not self.world.path: self._save_as(); return
        # Always save as .svworld regardless of original extension
        path = self.world.path
        if not path.endswith(".svworld"):
            path = str(Path(path).with_suffix(".svworld"))
        try:
            self.world.save(path)
            self.setWindowTitle(f"Alone — World Editor  [{Path(path).name}]")
            self.status.showMessage(f"Saved  —  {path}")
        except Exception as ex:
            QMessageBox.critical(self,"Save failed",str(ex))

    def _save_as(self):
        if not self.world: return
        path, _ = QFileDialog.getSaveFileName(
            self,"Save World As","","Alone Editor Files (*.svworld);;All Files (*)")
        if not path: return
        if not path.endswith(".svworld"): path += ".svworld"
        try:
            self.world.save(path)
            self.setWindowTitle(f"Alone — World Editor  [{Path(path).name}]")
            self.status.showMessage(f"Saved  —  {path}")
        except Exception as ex:
            QMessageBox.critical(self,"Save failed",str(ex))

    def _export(self):
        if not self.world:
            QMessageBox.information(self,"No world","Open or create a world first."); return
        path, _ = QFileDialog.getSaveFileName(
            self,"Export .world Binary","","NDS World Files (*.world);;All Files (*)")
        if not path: return
        if not path.endswith(".world"): path += ".world"
        try:
            # Count how many grid positions have multiple chunks (will be merged)
            from collections import Counter
            grid_counts = Counter((c.grid_x, c.grid_z) for c in self.world.chunks)
            merged = [(k, v) for k, v in grid_counts.items() if v > 1]

            self.world.export_world(path)
            self.status.showMessage(f"Exported  —  {path}")

            details = (
                f"Textures: converted to NDS packed format (ABGR1555)\n"
                f"Bottom faces stripped, s16 clamped\n"
            )
            if merged:
                details += f"\nMerged {len(merged)} grid position(s) with multiple chunks:\n"
                for (gx, gz), n in merged:
                    details += f"  ({gx},{gz}) — {n} chunks → 1\n"
            QMessageBox.information(self, "Export complete",
                f"Written to:\n{path}\n\n{details}")
        except Exception as ex:
            QMessageBox.critical(self,"Export failed",str(ex))

    def _dsify(self):
        if not self.world:
            QMessageBox.information(self,"No world","Open or create a world first."); return
        dlg = DSifyDialog(self.world, self)
        if dlg.exec() == QDialog.DialogCode.Accepted:
            self.viewport._gl_textures.clear()
            self.inspector.palette.refresh()
            self.inspector.floor_painter.invalidate_tex_cache()
            self.obj_selector.refresh()
            self.viewport.update()
            self.status.showMessage("DS-ify complete — review and save")

    def _frame_all(self):
        if self.world and self.world.chunks:
            self.viewport.focus_on_chunk(self.world.chunks[0])
        else:
            self.viewport.cam_target = [0,0,0]
            self.viewport.cam_dist   = 60
            self.viewport.update()

    def _on_viewport_object_clicked(self, idx: int):
        if not self.world or idx < 0 or idx >= len(self.world.chunks):
            return
        mods = QApplication.keyboardModifiers()
        if mods & Qt.KeyboardModifier.ControlModifier:
            current = {
                item.data(Qt.ItemDataRole.UserRole)
                for item in self.obj_selector.chunk_list.selectedItems()
            }
            if idx in current:
                current.discard(idx)
            else:
                current.add(idx)
            indices = sorted(current)
        elif mods & Qt.KeyboardModifier.ShiftModifier:
            current = {
                item.data(Qt.ItemDataRole.UserRole)
                for item in self.obj_selector.chunk_list.selectedItems()
            }
            current.add(idx)
            indices = sorted(current)
        else:
            indices = [idx]
        self.obj_selector.chunk_list.blockSignals(True)
        self.obj_selector.chunk_list.clearSelection()
        for row in range(self.obj_selector.chunk_list.count()):
            item = self.obj_selector.chunk_list.item(row)
            if item.data(Qt.ItemDataRole.UserRole) in indices:
                item.setSelected(True)
        self.obj_selector.chunk_list.blockSignals(False)
        self._on_chunks_selected(indices)

    def _on_chunks_selected(self, indices: list):
        if not self.world:
            self.inspector.set_chunks([])
            self.viewport.set_selected_chunks([])
            return
        # Filter valid indices
        valid = [i for i in indices if 0 <= i < len(self.world.chunks)]
        chunks = [self.world.chunks[i] for i in valid]
        self.inspector.set_chunks(chunks)
        self.viewport.set_selected_chunks(valid)
        if len(chunks) == 1:
            c = chunks[0]
            self.status.showMessage(
                f"Chunk [{c.grid_x},{c.grid_z}]  "
                f"{len(c.vertices)} verts / {c.poly_count()} polys")
        elif chunks:
            total_polys = sum(c.poly_count() for c in chunks)
            self.status.showMessage(
                f"{len(chunks)} chunks selected  —  {total_polys} total polys")
        else:
            self.status.showMessage("No selection")

    def _on_chunk_deleted(self, idx):
        self.inspector.set_chunks([])
        self.viewport.set_selected_chunks([])
        self.viewport.update()

    def _on_inspector_changed(self):
        self.obj_selector.refresh(keep_selection=True)
        self.viewport.update()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main():
    os.environ.setdefault("QT_OPENGL", "desktop")

    fmt = QSurfaceFormat()
    fmt.setDepthBufferSize(24)
    fmt.setStencilBufferSize(8)
    fmt.setVersion(2, 1)
    fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CompatibilityProfile)
    fmt.setRenderableType(QSurfaceFormat.RenderableType.OpenGL)
    QSurfaceFormat.setDefaultFormat(fmt)

    app = QApplication(sys.argv)
    app.setApplicationName("Alone World Editor")

    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
