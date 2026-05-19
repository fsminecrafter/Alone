#!/usr/bin/env python3
"""
inspect_world.py  —  Dump the contents of a .world binary file.

Usage:
    python3 inspect_world.py fat:/Alone/world.world
    python3 inspect_world.py world.world

Prints every chunk's grid position so you can see if they are all at (0,0).
Copy the file off the SD card first, then run on PC.
"""

import struct
import sys
from pathlib import Path
from collections import Counter

WORLD_MAGIC   = b"ALWF"
WORLD_VERSION = 1

TEX_FMT_NAMES = {
    1: "RGB32_A3",
    2: "RGB4",
    3: "RGB16",
    4: "RGB256",
    5: "TEXF8 (compressed)",
    6: "RGB8_A5",
    7: "RGB (15-bit, no alpha)",
    8: "RGBA (A1BGR5)",
}

VERTEX_SIZE = struct.calcsize("<6h4B2h")  # 20 bytes


def read_world(path: str):
    data = Path(path).read_bytes()
    offset = 0

    # ── Header ──
    hdr_fmt = "<4sHHLh"
    hdr_size = struct.calcsize(hdr_fmt)
    magic, version, tex_count, chunk_count, unit = struct.unpack_from(hdr_fmt, data, offset)
    offset += hdr_size

    print("=" * 60)
    print(f"Magic   : {magic}")
    print(f"Version : {version}")
    print(f"Textures: {tex_count}")
    print(f"Chunks  : {chunk_count}")
    print(f"Unit    : {unit}  (should be 16)")
    print("=" * 60)

    if magic != WORLD_MAGIC:
        print("ERROR: Not a valid .world file (bad magic)")
        sys.exit(1)
    if version != WORLD_VERSION:
        print(f"ERROR: Unknown version {version}")
        sys.exit(1)

    # ── Textures ──
    tex_hdr_fmt  = "<BBBBL"
    tex_hdr_size = struct.calcsize(tex_hdr_fmt)
    print(f"\n{'─'*60}")
    print("TEXTURES")
    print(f"{'─'*60}")
    total_tex_bytes = 0
    for i in range(tex_count):
        tid, wlog, hlog, fmt, dbytes = struct.unpack_from(tex_hdr_fmt, data, offset)
        offset += tex_hdr_size
        offset += dbytes  # skip pixel data
        total_tex_bytes += dbytes
        fmt_name = TEX_FMT_NAMES.get(fmt, f"unknown({fmt})")
        w, h = 1 << wlog, 1 << hlog
        print(f"  [{i}] id={tid}  {w}×{h}  fmt={fmt_name}  data={dbytes} B")
    print(f"  Total texture data: {total_tex_bytes:,} B")

    # ── Chunks ──
    chunk_hdr_fmt  = "<2h2H"
    chunk_hdr_size = struct.calcsize(chunk_hdr_fmt)

    print(f"\n{'─'*60}")
    print("CHUNKS")
    print(f"{'─'*60}")

    grid_positions = []
    total_verts    = 0
    total_polys    = 0
    bb_chunks      = 0

    for i in range(chunk_count):
        gx, gz, vc, pc = struct.unpack_from(chunk_hdr_fmt, data, offset)
        offset += chunk_hdr_size

        # Count billboard verts (nx == 0x7FFF, 0x7FFE, or 0x7FFD)
        bb_count = 0
        for vi in range(vc):
            vdata = struct.unpack_from("<6h4B2h", data, offset + vi * VERTEX_SIZE)
            nx = vdata[3]
            if nx in (0x7FFF, 0x7FFE, 0x7FFD):
                bb_count += 1

        offset += VERTEX_SIZE * vc
        grid_positions.append((gx, gz))
        total_verts += vc
        total_polys += pc
        if bb_count > 0:
            bb_chunks += 1

        bb_info = f"  [{bb_count} bb verts]" if bb_count else ""
        print(f"  chunk[{i:3d}]  grid=({gx:4d},{gz:4d})  verts={vc:5d}  polys={pc:4d}{bb_info}")

    # ── Summary ──
    print(f"\n{'─'*60}")
    print("SUMMARY")
    print(f"{'─'*60}")
    print(f"  Total verts : {total_verts:,}")
    print(f"  Total polys : {total_polys:,}")
    print(f"  Chunks with billboards: {bb_chunks}")

    dupes = {pos: cnt for pos, cnt in Counter(grid_positions).items() if cnt > 1}
    unique_positions = len(set(grid_positions))
    print(f"  Unique grid positions: {unique_positions} / {chunk_count}")

    if dupes:
        print(f"\n  *** WARNING: {len(dupes)} grid position(s) have MULTIPLE chunks ***")
        print(f"  *** The runtime can only render ONE chunk per grid cell.      ***")
        print(f"  *** These chunks will be invisible (overwritten in the slot): ***")
        for pos, cnt in sorted(dupes.items()):
            print(f"      {pos}  →  {cnt} chunks (only last one is rendered)")
    else:
        print("\n  All grid positions are unique — grid layout looks correct.")

    # Show the bounding box of the world
    if grid_positions:
        xs = [p[0] for p in grid_positions]
        zs = [p[1] for p in grid_positions]
        print(f"\n  Grid X range: {min(xs)} .. {max(xs)}")
        print(f"  Grid Z range: {min(zs)} .. {max(zs)}")
        print(f"  World X range: {min(xs)*16} .. {max(xs)*16 + 16} units")
        print(f"  World Z range: {min(zs)*16} .. {max(zs)*16 + 16} units")

    print(f"\n{'='*60}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 inspect_world.py <path/to/world.world>")
        sys.exit(1)
    read_world(sys.argv[1])
