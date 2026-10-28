#include "client/RenderCommon.h"

namespace alone::rc {

Color groundColour(Ground g) {
    switch (g) {
        case Ground::Grass: return hexColor(0x667f40);
        case Ground::ForestFloor: return hexColor(0x5a5232);
        case Ground::Moss: return hexColor(0x4f6436);
        case Ground::Dirt: return hexColor(0x6b583e);
        case Ground::Sand: return hexColor(0xc4b48a);
        case Ground::TallGrass: return hexColor(0x8a8c50);
        case Ground::FieldSoil: return hexColor(0x5a4430);
        case Ground::Stubble: return hexColor(0xa69658);
        case Ground::Asphalt: return hexColor(0x4a4c50);
        case Ground::Sidewalk: return hexColor(0x9a9893);
        case Ground::Gravel: return hexColor(0x8e8474);
        case Ground::Cobble: return hexColor(0x82796f);
        case Ground::Rail: return hexColor(0x5a5048);
        case Ground::Concrete: return hexColor(0x979590);
        case Ground::WaterShallow: return hexColor(0x3f6e86);
        case Ground::WaterDeep: return hexColor(0x26506c);
        case Ground::FloorWood: return hexColor(0xa27a50);
        case Ground::FloorTile: return hexColor(0xc8c6c0);
        case Ground::FloorConcrete: return hexColor(0x8a8884);
        case Ground::FloorCarpet: return hexColor(0x7e5a58);
        case Ground::Deck: return hexColor(0x8a6c4c);
        default: return MAGENTA;
    }
}

int groundMaterial(Ground g) {
    switch (g) {
        case Ground::Grass: case Ground::TallGrass: case Ground::Stubble: return MGrass;
        case Ground::ForestFloor: case Ground::Moss: return MForest;
        case Ground::Dirt: case Ground::Sand: case Ground::FieldSoil: case Ground::Gravel: case Ground::Rail: return MDirt;
        case Ground::Asphalt: return MAsphalt;
        case Ground::Sidewalk: return MPaving;
        case Ground::Cobble: return MCobble;
        case Ground::Concrete: case Ground::FloorConcrete: return MConcrete;
        case Ground::WaterShallow: case Ground::WaterDeep: return MWater;
        case Ground::FloorWood: return MFloorWood;
        case Ground::FloorTile: return MFloorTile;
        case Ground::FloorCarpet: return MCarpet;
        case Ground::Deck: return MFloorWood;
        default: return MPlain;
    }
}

float leafDensity(Prop p, int doy) {
    float fallStart = 280, fallEnd = 305;
    switch (p) {
        case Prop::Birch: fallStart = 280; fallEnd = 305; break;
        case Prop::Maple: fallStart = 276; fallEnd = 300; break;
        case Prop::Oak: fallStart = 292; fallEnd = 322; break;
        case Prop::Rowan: fallStart = 278; fallEnd = 302; break;
        default: return 1.0f;
    }
    if (doy < 110) return 0.0f;                                     // winter until the leaves come
    if (doy < 140) return (doy - 110) / 30.0f;                       // spring
    if (doy < fallStart) return 1.0f;
    return std::clamp(1.0f - (doy - fallStart) / (fallEnd - fallStart), 0.0f, 1.0f);
}

Color leafColour(Prop p, int doy, uint8_t variant) {
    float turn = std::clamp((doy - 250) / 40.0f, 0.0f, 1.0f);     // mid Sep .. late Oct
    float v = (variant % 32) / 32.0f;
    Color summer = hexColor(0x4a7230);
    Color autumn;
    switch (p) {
        case Prop::Birch: summer = hexColor(0x62903a); autumn = hexColor(0xd8b02c); break;
        case Prop::Maple: autumn = v < 0.5f ? hexColor(0xc8441c) : hexColor(0xe08a20); break;
        case Prop::Oak: autumn = hexColor(0x9a722c); turn *= 0.8f; break;
        case Prop::Rowan: autumn = hexColor(0xc8501e); break;
        default: autumn = summer; break;
    }
    Color c = mixColor(summer, autumn, std::clamp(turn * (0.7f + 0.6f * v), 0.0f, 1.0f));
    return shade(c, 0.88f + 0.24f * v);
}

} // namespace alone::rc
