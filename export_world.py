#!/usr/bin/env python3
"""
Export worldtemplate.svworld to world.world binary format.
Run this whenever you update the .svworld file to regenerate the game's world.
"""
import sys
from pathlib import Path

# Add the project root to sys.path so we can import editor
project_root = Path(__file__).parent
sys.path.insert(0, str(project_root))

from editor import WorldFile

def main():
    svworld_path = project_root / "worldtemplate.svworld"
    world_path = project_root / "world.world"
    
    print(f"Loading {svworld_path}...")
    world = WorldFile.load(str(svworld_path))
    
    print(f"Exporting to {world_path}...")
    world.export_world(str(world_path))
    
    print(f"✓ Successfully exported {world_path}")
    print(f"  Audio tracks: {len(world.audio_tracks)}")
    print(f"  Audio emitters: {len(world.audio_emitters)}")
    for i, track in enumerate(world.audio_tracks):
        print(f"    [{i}] {track.filename} (vol={track.volume})")

if __name__ == "__main__":
    main()
