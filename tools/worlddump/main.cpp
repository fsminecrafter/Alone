// Renders a top-down image (PPM) of the generated world around a point, for
// checking world generation without starting the game.
//   alone_worlddump <map px x> <map px y> <size m> out.ppm
#include "core/Log.h"
#include "core/Paths.h"
#include "items/ItemRegistry.h"
#include "world/World.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace alone;

static uint32_t groundColour(Ground g) {
    switch (g) {
        case Ground::Grass: return 0x6f9a4a; case Ground::ForestFloor: return 0x4e5a32; case Ground::Moss: return 0x4f6e34;
        case Ground::Dirt: return 0x7a6448; case Ground::Sand: return 0xd8c890; case Ground::TallGrass: return 0x86a456;
        case Ground::FieldSoil: return 0x6e5236; case Ground::Stubble: return 0xb8a466; case Ground::Asphalt: return 0x4a4a4e;
        case Ground::Sidewalk: return 0x9a9a98; case Ground::Gravel: return 0x9a8e7a; case Ground::Cobble: return 0x7a7672;
        case Ground::Rail: return 0x5a5048; case Ground::Concrete: return 0xa8a6a0; case Ground::WaterShallow: return 0x4f8fb0;
        case Ground::WaterDeep: return 0x2f5f8a; case Ground::FloorWood: return 0xb08a5a; case Ground::FloorTile: return 0xd0d0cc;
        case Ground::FloorConcrete: return 0x8a8a88; case Ground::FloorCarpet: return 0x8a5a5a; default: return 0xff00ff;
    }
}

int main(int argc, char** argv) {
    if (argc < 5) { std::printf("usage: alone_worlddump <px> <py> <size_m> out.ppm\n"); return 1; }
    Paths::init(argv[0], getenv("ALONE_DATA") ? getenv("ALONE_DATA") : "");
    MapData map;
    ItemRegistry items;
    if (!map.load(Paths::data("maps/alingsas"))) return 1;
    items.loadDirectory(Paths::data("items"));
    items.loadContainers(Paths::data("loot/containers.ini"));
    World world(map, items, 1234);
    int cx = (int)(atof(argv[1]) * map.metresPerPixel()), cy = (int)(atof(argv[2]) * map.metresPerPixel());
    int size = atoi(argv[3]);
    int scale = size <= 300 ? 3 : 1;
    int W = size * scale;
    std::vector<uint32_t> img((size_t)W * W);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int wx = cx - size / 2 + x, wy = cy - size / 2 + y;
            const Tile& t = world.tile(wx, wy);
            uint32_t c = groundColour(t.ground);
            if (isTree(t.prop)) c = t.prop == Prop::Birch ? 0x8ab060 : 0x1f3f1f;
            else if (isFurniture(t.prop)) c = 0x5a3a2a;
            else if (t.prop == Prop::Hedge) c = 0x2a5a2a;
            else if (t.prop == Prop::Fence) c = 0xe0e0e0;
            else if (t.prop == Prop::Car) c = 0xc03030;
            else if (t.prop != Prop::None) c = 0x506030;
            for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale; ++sx) {
                    uint32_t cc = c;
                    if (scale > 1) {
                        if ((t.walls & WallN) && sy == 0) cc = (t.walls & DoorN) ? 0xffd040 : (t.walls & WindowN) ? 0x80c0ff : 0x202020;
                        if ((t.walls & WallW) && sx == 0) cc = (t.walls & DoorW) ? 0xffd040 : (t.walls & WindowW) ? 0x80c0ff : 0x202020;
                    } else if (t.walls) cc = 0x202020;
                    img[(size_t)(y * scale + sy) * W + x * scale + sx] = cc;
                }
        }
    FILE* f = std::fopen(argv[4], "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", W, W);
    for (uint32_t c : img) { unsigned char rgb[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c}; std::fwrite(rgb, 1, 3, f); }
    std::fclose(f);
    std::printf("%zu buildings, %zu chunks\n", world.gen().allBuildings().size(), world.loadedChunks());
    return 0;
}
