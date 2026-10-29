#pragma once
#include <cstdint>

namespace alone {

// Land-cover classes from the digitised map (terrain.pgm values).
// Must match tools/mapgen/digitize.py.
enum class Zone : uint8_t {
    Unknown = 0, Water, Forest, Woodland, Park, Residential, Field, Meadow, Commercial, Hospital, Paved, Parking,
    ForestDense, Thicket, Marsh, Rock,   // from orienteering maps: slow and difficult ground
    Count
};

// Surface of a 1x1 m tile.
enum class Ground : uint8_t {
    Grass, ForestFloor, Moss, Dirt, Sand, TallGrass, FieldSoil, Stubble,
    Asphalt, Sidewalk, Gravel, Cobble, Rail, Concrete,
    WaterShallow, WaterDeep,
    FloorWood, FloorTile, FloorConcrete, FloorCarpet,
    Deck,                        // wooden piers and jetties over the water
    Void,                        // around the upper floors and basements: nothing
    Count
};

// Things standing on a tile.
enum class Prop : uint8_t {
    None,
    // nature
    Spruce, Pine, Birch, Oak, Maple, Rowan,
    Shrub, Hedge, LingonBush, BlueberryBush, RoseBush, Rock, Boulder, Stump, Reeds,
    // outdoor man-made
    Fence, Lamppost, Bench, Mailbox, Barrier, Car, Bin, HayBale, Woodpile, RainBarrel, Pillar, Crate,
    // furniture
    Fridge, Freezer, Counter, Stove, Sink, Table, Chair, Sofa, Bed, Wardrobe, Dresser,
    Bookshelf, Toilet, Bathtub, BathroomCabinet, Shelf, Cooler, Checkout, Desk, Locker,
    Pew, MedicalCabinet, GunCabinet, Workbench, Altar, HospitalBed,
    Bike, FuelPump,
    // carpentry and the floors of houses
    WoodShelf, DryingRack, StairsUp, StairsDown, BrokenStairs, BoxTrap,
    Count
};

// Edge flags stored on a tile: walls on the tile's north (-y) and west (-x) edges.
enum WallBits : uint8_t {
    WallN = 1, WallW = 2, DoorN = 4, DoorW = 8, WindowN = 16, WindowW = 32,
};

// Road surface markings drawn on top of the ground.
enum class Marking : uint8_t { None, CentreDash, CentreSolid, EdgeLine, LaneDash, Rails, Crossing };

bool groundWalkable(Ground g);
bool propBlocks(Prop p);
bool isTree(Prop p);
bool isFurniture(Prop p);
float propHeight(Prop p);          // metres, for rendering
const char* propName(Prop p);
const char* propContainer(Prop p); // loot container id or nullptr

} // namespace alone
