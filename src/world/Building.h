#pragma once
#include "core/Math.h"
#include "world/Terrain.h"
#include <cstdint>
#include <string>
#include <vector>

namespace alone {

struct IRect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool intersects(const IRect& o) const { return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom(); }
    IRect expanded(int m) const { return {x - m, y - m, w + 2 * m, h + 2 * m}; }
    Vec2 centre() const { return {x + w * 0.5f, y + h * 0.5f}; }
    int area() const { return w * h; }
};

enum class BuildingType {
    House, Cottage, Farmhouse, Barn, Shed, Apartment, Office, Warehouse,
    Grocery, Pharmacy, GasStation, Hardware, Hospital, School, Church, TownHall,
    Police, FireStation, Station, Hostel, Manor, Sports, Shop, Mall, Restaurant, ParkingGarage, Count
};

enum class RoomType { Hall, Living, Kitchen, Bedroom, Bathroom, Storage, ShopFloor, Office, Ward, Classroom, Nave, Lobby, Locker, Dining, Count };

enum class RoofKind { Gable, Flat, Hip };

const char* buildingTypeName(BuildingType t);
const char* roomTypeName(RoomType t);

struct Room {
    IRect rect;
    RoomType type = RoomType::Living;
    std::string loot;            // shop units: what the shelves hold
    std::string label;           // shop units: sign text
};

struct Furniture {
    int x = 0, y = 0;            // world tile
    Prop prop = Prop::None;
    uint8_t facing = 0;          // 0 N, 1 E, 2 S, 3 W (which way it faces into the room)
    std::string container;       // loot table id, empty if none
};

struct WallEdge {
    int x = 0, y = 0;            // tile whose N/W edge this is
    uint8_t bits = 0;            // WallBits
};

struct Building {
    int id = -1;
    BuildingType type = BuildingType::House;
    std::string name;            // for landmarks, e.g. "Christinae kyrka"
    std::string shop;            // speciality of a Shop (clothes, books, ...)
    IRect rect;
    int floors = 1;              // visual storeys (only the ground floor is enterable)
    RoofKind roof = RoofKind::Gable;
    bool roofAlongX = true;      // ridge direction for gable roofs
    uint32_t wallColour = 0x8b2a1e;
    uint32_t roofColour = 0x303030;
    uint32_t trimColour = 0xf0f0f0;
    Ground floor = Ground::FloorWood;
    std::vector<Room> rooms;
    std::vector<WallEdge> edges;
    std::vector<Furniture> furniture;
    Vec2 doorPos;                // outside the front door
    Vec2 pathEnd;                // where the driveway meets the road
    bool hasPath = false;
    IRect lot;                   // garden / plot around the building
    uint8_t lotBorder = 0;       // 0 none, 1 hedge, 2 fence
    bool isOutbuilding = false;  // shed/barn belonging to another building's lot
    bool osm = false;            // a real building from OpenStreetMap
    int levels = 0;              // storeys from OpenStreetMap (0 unknown)
    uint32_t osmColour = 0, osmRoofColour = 0;   // mapped colours (bit 24 set when known)
    int osmRoof = -1;            // mapped roof shape: 0 gable, 1 flat, 2 hip
    bool oldTown = false;        // the wooden houses along Kungsgatan and around the squares
    bool shopFront = false;      // big display windows and an awning on the street side
    std::string sign;            // text over the entrance ("Vimpeln", "Apotek", ...)
    uint32_t seed = 0;
    float floorY = 0;            // height of the ground floor above sea level (level on any slope)

    const Room* roomAt(int x, int y) const;
    const Furniture* furnitureAt(int x, int y) const;
};

} // namespace alone
