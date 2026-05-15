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

DS_MAX_TEX_SIZE  = 256
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

        # UV spans the full texture across the whole chunk
        u0, u1 = 0, tex_w * 16
        v0, v1 = 0, tex_h * 16
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
    def bake_floor_to_vertices(self):
        """Replace self.vertices floor section (first 6 verts) with baked floor quad."""
        floor_verts = self.bake_to_vertices(world=None)[:6]
        if len(self.vertices) >= 6:
            self.vertices[:6] = floor_verts
        else:
            self.vertices = floor_verts

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
                for tz, row in enumerate(cd.get("floor", [])):
                    for tx, td2 in enumerate(row):
                        t = chunk.floor[tz][tx]
                        t.tex_id = td2.get("tex_id", NO_TEX)
                        t.r = td2.get("r", 180)
                        t.g = td2.get("g", 180)
                        t.b = td2.get("b", 180)
                # Rebuild floor verts so viewport can draw them (6 verts = 1 quad)
                chunk.bake_floor_to_vertices()
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
        for t in self.textures:
            if t.tex_id == tid:
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
    return chunk


# ---------------------------------------------------------------------------
# OpenGL Viewport
# ---------------------------------------------------------------------------
class Viewport(QOpenGLWidget):
    object_clicked = pyqtSignal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.world: WorldFile | None = None
        self.selected_chunk = -1
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

    def set_world(self, world):
        self.world = world
        self._gl_textures.clear()
        self.update()

    def initializeGL(self):
        glEnable(GL_DEPTH_TEST)
        glEnable(GL_CULL_FACE)
        glCullFace(GL_BACK)
        glEnable(GL_LIGHTING)
        glEnable(GL_LIGHT0)
        glLightfv(GL_LIGHT0, GL_POSITION, [10.0, 20.0, 10.0, 1.0])
        glLightfv(GL_LIGHT0, GL_DIFFUSE,  [0.9,  0.9,  0.85, 1.0])
        glLightfv(GL_LIGHT0, GL_AMBIENT,  [0.3,  0.3,  0.35, 1.0])
        glEnable(GL_COLOR_MATERIAL)
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE)
        glClearColor(0.10, 0.11, 0.13, 1.0)

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
        glViewport(0, 0, w, h)
        glMatrixMode(GL_PROJECTION)
        glLoadIdentity()
        proj = self._perspective_matrix(45.0, w / max(h, 1), 0.5, 2000.0)
        glLoadMatrixf(proj.T)
        glMatrixMode(GL_MODELVIEW)

    def paintGL(self):
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
        glLoadIdentity()
        self._apply_camera()
        if self.grid_visible:
            self._draw_grid()
        if self.world:
            for i, chunk in enumerate(self.world.chunks):
                self._draw_chunk(chunk, selected=(i == self.selected_chunk))

    def _apply_camera(self):
        yaw   = math.radians(self.cam_yaw)
        pitch = math.radians(self.cam_pitch)
        cx = self.cam_target[0] + self.cam_dist * math.cos(pitch) * math.sin(yaw)
        cy = self.cam_target[1] + self.cam_dist * math.sin(pitch)
        cz = self.cam_target[2] + self.cam_dist * math.cos(pitch) * math.cos(yaw)
        mv = self._lookat_matrix([cx,cy,cz], self.cam_target, [0,1,0])
        glLoadMatrixf(mv.T)

    def _draw_grid(self):
        glDisable(GL_LIGHTING)
        glLineWidth(1.0)
        glColor3f(0.22, 0.24, 0.27)
        size = 10 * CHUNK_WORLD_UNIT
        step = CHUNK_WORLD_UNIT
        glBegin(GL_LINES)
        for i in range(-size, size + step, step):
            glVertex3f(i, 0, -size); glVertex3f(i, 0,  size)
            glVertex3f(-size, 0, i); glVertex3f( size, 0, i)
        glEnd()
        glLineWidth(2.0)
        glBegin(GL_LINES)
        glColor3f(0.8,0.2,0.2); glVertex3f(0,0,0); glVertex3f(8,0,0)
        glColor3f(0.2,0.8,0.2); glVertex3f(0,0,0); glVertex3f(0,8,0)
        glColor3f(0.2,0.4,0.9); glVertex3f(0,0,0); glVertex3f(0,0,8)
        glEnd()
        glLineWidth(1.0)
        glEnable(GL_LIGHTING)

    def _get_or_upload_texture(self, tex: DSTexture) -> int:
        if tex.tex_id in self._gl_textures:
            return self._gl_textures[tex.tex_id]
        handle = glGenTextures(1)
        glBindTexture(GL_TEXTURE_2D, handle)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST)
        try:
            arr = np.frombuffer(tex.data, dtype=np.uint8)
            expected = tex.width * tex.height * 4
            if len(arr) >= expected:
                arr = arr[:expected].reshape(tex.height, tex.width, 4)
                # Flip vertically: PIL is top-left origin, OpenGL is bottom-left.
                arr = np.flipud(arr)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                             tex.width, tex.height, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, arr)
        except Exception:
            pass
        self._gl_textures[tex.tex_id] = handle
        return handle

    def invalidate_texture(self, tex_id: int):
        """Force re-upload next frame (call after texture data changes)."""
        if tex_id in self._gl_textures:
            self.makeCurrent()
            glDeleteTextures(1, [self._gl_textures.pop(tex_id)])

    def _draw_chunk(self, chunk: DSChunk, selected=False):
        ox = chunk.grid_x * CHUNK_WORLD_UNIT
        oz = chunk.grid_z * CHUNK_WORLD_UNIT

        if self.wireframe:
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)
            glDisable(GL_LIGHTING)
        else:
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL)
            glEnable(GL_LIGHTING)

        if selected:
            glDisable(GL_LIGHTING)
            glLineWidth(2.5)
            glColor3f(1.0, 0.75, 0.1)
            s = CHUNK_WORLD_UNIT / 2
            glBegin(GL_LINE_LOOP)
            glVertex3f(ox-s, 0.15, oz-s); glVertex3f(ox+s, 0.15, oz-s)
            glVertex3f(ox+s, 0.15, oz+s); glVertex3f(ox-s, 0.15, oz+s)
            glEnd()
            glLineWidth(1.0)
            glEnable(GL_LIGHTING)

        # Draw the floor tiles directly from the floor grid
        self._draw_floor(chunk, ox, oz)

        # Draw any additional (imported model) geometry on top of the floor
        extra_verts = chunk.vertices[6:]   # skip the 6-vert floor quad
        if extra_verts:
            self._draw_vertex_list(extra_verts, ox, oz)

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL)

    def _draw_floor(self, chunk: DSChunk, ox: float, oz: float):
        """Draw the chunk floor as a single quad using the representative tile."""
        half = CHUNK_WORLD_UNIT / 2.0
        tile = chunk.floor[0][0]   # representative tile
        r, g, b = tile.r, tile.g, tile.b

        glNormal3f(0, 1, 0)
        glColor3ub(r, g, b)

        glDisable(GL_TEXTURE_2D)
        if tile.tex_id != NO_TEX and self.world:
            dtex = self.world.texture_by_id(tile.tex_id)
            if dtex:
                h = self._get_or_upload_texture(dtex)
                glBindTexture(GL_TEXTURE_2D, h)
                glEnable(GL_TEXTURE_2D)

        x0, x1 = ox - half, ox + half
        z0, z1 = oz - half, oz + half

        glBegin(GL_TRIANGLES)
        glTexCoord2f(0,0); glVertex3f(x0, 0, z0)
        glTexCoord2f(0,1); glVertex3f(x0, 0, z1)
        glTexCoord2f(1,0); glVertex3f(x1, 0, z0)

        glTexCoord2f(1,0); glVertex3f(x1, 0, z0)
        glTexCoord2f(0,1); glVertex3f(x0, 0, z1)
        glTexCoord2f(1,1); glVertex3f(x1, 0, z1)
        glEnd()

        glDisable(GL_TEXTURE_2D)

    def _draw_vertex_list(self, verts, ox, oz):
        cur_tex = None
        glEnable(GL_TEXTURE_2D)
        glBegin(GL_TRIANGLES)
        for v in verts:
            if v.tex_id != cur_tex:
                glEnd()
                glDisable(GL_TEXTURE_2D)
                if v.tex_id != NO_TEX and self.world:
                    dtex = self.world.texture_by_id(v.tex_id)
                    if dtex:
                        h = self._get_or_upload_texture(dtex)
                        glBindTexture(GL_TEXTURE_2D, h)
                        glEnable(GL_TEXTURE_2D)
                cur_tex = v.tex_id
                glBegin(GL_TRIANGLES)
            glColor3ub(v.r, v.g, v.b)
            if v.nx or v.ny or v.nz:
                glNormal3f(v.nx/FP, v.ny/FP, v.nz/FP)
            glTexCoord2f(v.u/16.0, v.v/16.0)
            glVertex3f(ox + v.x/FP, v.y/FP, oz + v.z/FP)
        glEnd()
        glDisable(GL_TEXTURE_2D)

    # ---- Mouse ----
    def mousePressEvent(self, e):
        self._last_mouse = e.position()
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
        self.world:  WorldFile| None = None
        self.active_tex_id = NO_TEX
        self.active_r, self.active_g, self.active_b = 180, 180, 180
        self._hover = (-1, -1)
        self._tex_pixmaps: dict[int, QPixmap] = {}
        self.setMouseTracking(True)
        self.setMinimumSize(160, 160)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

    def set_chunk(self, chunk, world):
        self.chunk = chunk
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
        if not self.chunk:
            p.fillRect(self.rect(), QColor(30, 32, 40))
            p.setPen(QColor(100, 100, 120))
            p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "No chunk selected")
            return

        # Floor is a single quad — show it as one big swatch
        tile = self.chunk.floor[0][0]
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
        p.drawText(self.rect(), Qt.AlignmentFlag.AlignBottom | Qt.AlignmentFlag.AlignHCenter,
                   f" {label} | RGB({tile.r},{tile.g},{tile.b}) ")

    def mouseMoveEvent(self, e):
        self._hover = self._cell_at(e.position().x(), e.position().y())
        self.update()
        if e.buttons() & Qt.MouseButton.LeftButton:
            self._paint_at(e.position().x(), e.position().y())

    def mousePressEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton:
            self._paint_at(e.position().x(), e.position().y())

    def leaveEvent(self, e):
        self._hover = (-1, -1)
        self.update()

    def _paint_at(self, x, y):
        if not self.chunk:
            return
        # Floor is a single quad — paint all tiles uniformly from floor[0][0]
        for tz in range(FLOOR_TILES):
            for tx in range(FLOOR_TILES):
                tile = self.chunk.floor[tz][tx]
                tile.tex_id = self.active_tex_id
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
# Inspector Panel
# ---------------------------------------------------------------------------
class InspectorPanel(QWidget):
    changed = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._chunk: DSChunk   | None = None
        self._world: WorldFile | None = None
        self._viewport: Viewport | None = None
        self._build_ui()

    def set_viewport(self, vp: Viewport):
        self._viewport = vp

    def _build_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(6, 6, 6, 6)
        layout.setSpacing(4)

        self.title = QLabel("Nothing selected")
        self.title.setStyleSheet("font-weight:bold; font-size:13px; color:#c8ccd4;")
        layout.addWidget(self.title)

        self.tabs = QTabWidget()
        layout.addWidget(self.tabs)

        # ---- Chunk tab ----
        chunk_w = QWidget()
        cf = QFormLayout(chunk_w)
        cf.setContentsMargins(6,6,6,6)

        self.gx_spin = QSpinBox(); self.gx_spin.setRange(-9999,9999)
        self.gz_spin = QSpinBox(); self.gz_spin.setRange(-9999,9999)
        self.chunk_name = QLineEdit()
        self.poly_label = QLabel("0")
        cf.addRow("Grid X:",  self.gx_spin)
        cf.addRow("Grid Z:",  self.gz_spin)
        cf.addRow("Name:",    self.chunk_name)
        cf.addRow("Polys:",   self.poly_label)

        # World-space position offset for the model inside this chunk
        self.wx_spin = QDoubleSpinBox(); self.wx_spin.setRange(-9999,9999); self.wx_spin.setDecimals(3)
        self.wy_spin = QDoubleSpinBox(); self.wy_spin.setRange(-9999,9999); self.wy_spin.setDecimals(3)
        self.wz_spin = QDoubleSpinBox(); self.wz_spin.setRange(-9999,9999); self.wz_spin.setDecimals(3)
        cf.addRow("Model X:", self.wx_spin)
        cf.addRow("Model Y:", self.wy_spin)
        cf.addRow("Model Z:", self.wz_spin)
        apply_btn = QPushButton("Apply")
        apply_btn.clicked.connect(self._apply_chunk)
        cf.addRow("", apply_btn)

        # Apply texture to imported object verts
        self.model_tex_combo = QComboBox()
        cf.addRow("Model Tex:", self.model_tex_combo)
        apply_tex_btn = QPushButton("Apply Texture to Model")
        apply_tex_btn.clicked.connect(self._apply_model_texture)
        cf.addRow("", apply_tex_btn)

        self.tabs.addTab(chunk_w, "Chunk")

        # ---- Floor tab ----
        floor_w = QWidget()
        fl = QVBoxLayout(floor_w)
        fl.setContentsMargins(4,4,4,4)
        fl.setSpacing(4)

        # Active brush row
        brush_row = QHBoxLayout()
        brush_lbl = QLabel("Brush:")
        brush_lbl.setFixedWidth(36)
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

        clear_btn = QPushButton("Clear Floor")
        clear_btn.setFixedHeight(22)
        clear_btn.clicked.connect(self._clear_floor)
        brush_row.addWidget(clear_btn)

        fl.addLayout(brush_row)

        # Texture palette
        self.palette = TexturePalette()
        self.palette.texture_selected.connect(self._on_tex_selected)
        fl.addWidget(self.palette)

        # Painter
        self.floor_painter = FloorPainter()
        self.floor_painter.tile_painted.connect(self._on_tile_painted)
        fl.addWidget(self.floor_painter, 1)

        self.tabs.addTab(floor_w, "Floor")

        # ---- Textures tab ----
        tex_w = QWidget()
        tl = QVBoxLayout(tex_w)
        tl.setContentsMargins(4,4,4,4)
        tl.setSpacing(4)

        import_btn = QPushButton("⊕  Import Texture(s)…")
        import_btn.clicked.connect(self._import_textures)
        tl.addWidget(import_btn)

        self.tex_list_widget = QListWidget()
        self.tex_list_widget.currentRowChanged.connect(self._on_tex_list_select)
        tl.addWidget(self.tex_list_widget, 1)

        # Preview + props
        self.tex_preview = QLabel()
        self.tex_preview.setFixedHeight(80)
        self.tex_preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.tex_preview.setStyleSheet("background:#1a1c20; border:1px solid #333;")
        tl.addWidget(self.tex_preview)

        pf = QFormLayout()
        self.tp_id    = QLabel("-")
        self.tp_size  = QLabel("-")
        self.tp_fmt   = QLabel("-")
        self.tp_bytes = QLabel("-")
        pf.addRow("ID:",     self.tp_id)
        pf.addRow("Size:",   self.tp_size)
        pf.addRow("Format:", self.tp_fmt)
        pf.addRow("Bytes:",  self.tp_bytes)
        tl.addLayout(pf)

        fmt_row = QHBoxLayout()
        self.dsify_fmt = QComboBox()
        for name in LIBNDS_FORMATS:
            self.dsify_fmt.addItem(name, LIBNDS_FORMATS[name])
        self.dsify_fmt.setCurrentIndex(1)   # RGBA
        fmt_row.addWidget(QLabel("DS fmt:"))
        fmt_row.addWidget(self.dsify_fmt)
        tl.addLayout(fmt_row)

        size_row = QHBoxLayout()
        self.dsify_maxsize = QComboBox()
        for s in [8,16,32,64,128,256]:
            self.dsify_maxsize.addItem(f"{s}×{s}", s)
        self.dsify_maxsize.setCurrentIndex(5)
        size_row.addWidget(QLabel("Max px:"))
        size_row.addWidget(self.dsify_maxsize)
        tl.addLayout(size_row)

        dsify_btn = QPushButton("DS-ify selected")
        dsify_btn.clicked.connect(self._dsify_selected)
        tl.addWidget(dsify_btn)

        dsify_all_btn = QPushButton("DS-ify all")
        dsify_all_btn.clicked.connect(self._dsify_all)
        tl.addWidget(dsify_all_btn)

        del_btn = QPushButton("Remove selected")
        del_btn.clicked.connect(self._remove_tex)
        tl.addWidget(del_btn)

        self.tabs.addTab(tex_w, "Textures")

        # ---- Stats tab ----
        stats_w = QWidget()
        sf = QFormLayout(stats_w)
        self.stat_verts = QLabel("0")
        self.stat_polys = QLabel("0")
        sf.addRow("Vertices:",  self.stat_verts)
        sf.addRow("Triangles:", self.stat_polys)
        self.tabs.addTab(stats_w, "Stats")

        layout.addStretch()

    # ------------------------------------------------------------------
    def set_world(self, world: WorldFile | None):
        self._world = world
        self.palette.set_world(world)
        self.floor_painter.world = world
        self._refresh_tex_list()
        self._refresh_model_tex_combo()

    def set_chunk(self, chunk: DSChunk | None):
        self._chunk = chunk
        self.floor_painter.set_chunk(chunk, self._world)
        self._refresh_model_tex_combo()
        if chunk is None:
            self.title.setText("Nothing selected")
            self.poly_label.setText("0")
            return
        self.title.setText(f"Chunk [{chunk.grid_x}, {chunk.grid_z}]")
        self.gx_spin.setValue(chunk.grid_x)
        self.gz_spin.setValue(chunk.grid_z)
        self.chunk_name.setText(chunk.name)
        self.wx_spin.setValue(chunk.world_x)
        self.wy_spin.setValue(chunk.world_y)
        self.wz_spin.setValue(chunk.world_z)
        polys = chunk.poly_count()
        self.poly_label.setText(str(polys))
        self.stat_verts.setText(str(len(chunk.vertices)))
        self.stat_polys.setText(str(polys))

    # ---- Chunk tab ----
    def _apply_chunk(self):
        if not self._chunk: return
        self._chunk.grid_x  = self.gx_spin.value()
        self._chunk.grid_z  = self.gz_spin.value()
        self._chunk.name    = self.chunk_name.text()
        self._chunk.world_x = self.wx_spin.value()
        self._chunk.world_y = self.wy_spin.value()
        self._chunk.world_z = self.wz_spin.value()
        self.changed.emit()

    def _refresh_model_tex_combo(self):
        self.model_tex_combo.clear()
        self.model_tex_combo.addItem("(none)", NO_TEX)
        if self._world:
            for tex in self._world.textures:
                self.model_tex_combo.addItem(f"[{tex.tex_id}] {tex.name}", tex.tex_id)

    def _apply_model_texture(self):
        """Apply selected texture to all object (non-floor) vertices in the chunk."""
        if not self._chunk: return
        tex_id = self.model_tex_combo.currentData()
        obj_verts = self._chunk.vertices[6:]   # first 6 verts are the floor quad
        if not obj_verts:
            QMessageBox.information(self, "No model", "This chunk has no imported model verts.")
            return
        # Recompute UV range based on texture size
        tex_w = tex_h = 1
        if self._world and tex_id != NO_TEX:
            dtex = self._world.texture_by_id(tex_id)
            if dtex:
                tex_w = dtex.width
                tex_h = dtex.height
        # Bounding box for planar UV projection
        xs = [v.x for v in obj_verts]
        zs = [v.z for v in obj_verts]
        min_x, max_x = min(xs), max(xs)
        min_z, max_z = min(zs), max(zs)
        rng_x = max(max_x - min_x, 1)
        rng_z = max(max_z - min_z, 1)
        for v in obj_verts:
            v.tex_id = tex_id
            v.u = clamp_s16(int(((v.x - min_x) / rng_x) * tex_w * 16))
            v.v = clamp_s16(int(((v.z - min_z) / rng_z) * tex_h * 16))
        if self._viewport:
            self._viewport.update()
        self.changed.emit()

    # ---- Floor tab ----
    def _on_tex_selected(self, tex_id: int):
        self.floor_painter.active_tex_id = tex_id

    def _on_tile_painted(self):
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
        if not self._chunk: return
        for tz in range(FLOOR_TILES):
            for tx in range(FLOOR_TILES):
                t = self._chunk.floor[tz][tx]
                t.tex_id = self.floor_painter.active_tex_id
                t.r = self.floor_painter.active_r
                t.g = self.floor_painter.active_g
                t.b = self.floor_painter.active_b
        self.floor_painter.update()
        if self._viewport: self._viewport.update()

    def _clear_floor(self):
        if not self._chunk: return
        for tz in range(FLOOR_TILES):
            for tx in range(FLOOR_TILES):
                self._chunk.floor[tz][tx] = FloorTile()
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
            "Images (*.png *.jpg *.bmp *.tga *.tiff);;All Files (*)")
        fmt      = self.dsify_fmt.currentData()
        max_size = self.dsify_maxsize.currentData()
        added = 0
        for path in paths:
            try:
                img = Image.open(path).convert("RGBA")
                dsimg, raw, nw, nh = dsify_image(img, max_size, fmt)
                tex = DSTexture(
                    tex_id=self._world.new_tex_id(),
                    width=nw, height=nh, fmt=fmt, data=raw,
                    name=Path(path).stem)
                tex.pil_img = dsimg
                self._world.textures.append(tex)
                added += 1
            except Exception as ex:
                QMessageBox.warning(self, "Import failed", str(ex))
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
        self.max_size.setCurrentIndex(5)
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
    chunk_selected = pyqtSignal(int)
    chunk_deleted  = pyqtSignal(int)

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
        self.chunk_list.currentRowChanged.connect(self.chunk_selected)
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

    def refresh(self):
        self.chunk_list.clear(); self.tex_list.clear()
        if not self.world: return
        for c in self.world.chunks:
            self.chunk_list.addItem(
                f"[{c.grid_x},{c.grid_z}] {c.name}  ({c.poly_count()} polys)")
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

    def _chunk_context(self, pos):
        row = self.chunk_list.currentRow()
        if row < 0: return
        menu = QMenu(self)
        dup  = menu.addAction("Duplicate")
        dele = menu.addAction("Delete")
        act  = menu.exec(self.chunk_list.mapToGlobal(pos))
        if act == dup and self.world:
            nc = copy.deepcopy(self.world.chunks[row])
            nc.grid_z += 1; nc.name += "_copy"
            self.world.chunks.insert(row+1, nc)
            self.refresh()
        elif act == dele and self.world:
            self.world.chunks.pop(row)
            self.refresh()
            self.chunk_deleted.emit(row)


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
        h_split.addWidget(self.viewport)

        self.inspector = InspectorPanel()
        self.inspector.set_viewport(self.viewport)
        self.inspector.changed.connect(self._on_inspector_changed)
        self.inspector.setMinimumWidth(240)
        self.inspector.setMaximumWidth(340)
        h_split.addWidget(self.inspector)
        h_split.setStretchFactor(0, 3)
        h_split.setStretchFactor(1, 1)

        v_split = QSplitter(Qt.Orientation.Vertical)
        v_split.addWidget(h_split)

        self.obj_selector = ObjectSelector()
        self.obj_selector.chunk_selected.connect(self._on_chunk_selected)
        self.obj_selector.chunk_deleted.connect(self._on_chunk_deleted)
        self.obj_selector.setMaximumHeight(200)
        v_split.addWidget(self.obj_selector)
        v_split.setStretchFactor(0, 4)
        v_split.setStretchFactor(1, 1)

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
        self.inspector.set_chunk(None)
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
            self.inspector.set_chunk(None)
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

    def _on_chunk_selected(self, idx):
        if not self.world or idx < 0 or idx >= len(self.world.chunks):
            self.inspector.set_chunk(None)
            self.viewport.selected_chunk = -1
            self.viewport.update(); return
        chunk = self.world.chunks[idx]
        self.inspector.set_chunk(chunk)
        self.viewport.selected_chunk = idx
        self.viewport.focus_on_chunk(chunk)
        self.status.showMessage(
            f"Chunk [{chunk.grid_x},{chunk.grid_z}]  "
            f"{len(chunk.vertices)} verts / {chunk.poly_count()} polys")

    def _on_chunk_deleted(self, idx):
        self.inspector.set_chunk(None)
        self.viewport.selected_chunk = -1
        self.viewport.update()

    def _on_inspector_changed(self):
        self.obj_selector.refresh()
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
