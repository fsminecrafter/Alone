#include "world/Terrain.h"
#include <cstddef>

namespace alone {

bool groundWalkable(Ground g) {
    return g != Ground::WaterDeep;
}

bool isTree(Prop p) {
    return p >= Prop::Spruce && p <= Prop::Rowan;
}

bool isFurniture(Prop p) {
    return p >= Prop::Fridge;
}

bool propBlocks(Prop p) {
    switch (p) {
        case Prop::None: case Prop::LingonBush: case Prop::BlueberryBush: case Prop::Reeds:
        case Prop::Stump: case Prop::Chair: case Prop::Mailbox: case Prop::Rock:
        case Prop::Lamppost:
            return false;
        default:
            return true;
    }
}

float propHeight(Prop p) {
    switch (p) {
        case Prop::Spruce: return 14; case Prop::Pine: return 16; case Prop::Birch: return 12;
        case Prop::Oak: return 11; case Prop::Maple: return 10; case Prop::Rowan: return 7;
        case Prop::Shrub: return 1.2f; case Prop::Hedge: return 1.4f; case Prop::LingonBush: return 0.2f;
        case Prop::BlueberryBush: return 0.35f; case Prop::RoseBush: return 1.3f; case Prop::Rock: return 0.4f;
        case Prop::Boulder: return 1.6f; case Prop::Stump: return 0.4f; case Prop::Reeds: return 1.4f;
        case Prop::Fence: return 1.0f; case Prop::Lamppost: return 5.0f; case Prop::Bench: return 0.5f;
        case Prop::Mailbox: return 1.1f; case Prop::Barrier: return 0.8f; case Prop::Car: return 1.5f;
        case Prop::Bin: return 1.1f; case Prop::HayBale: return 1.2f; case Prop::Woodpile: return 1.0f;
        case Prop::RainBarrel: return 0.9f; case Prop::Pillar: return 2.6f; case Prop::Crate: return 0.9f;
        case Prop::Fridge: return 1.8f; case Prop::Freezer: return 0.9f; case Prop::Counter: return 0.9f;
        case Prop::Stove: return 0.9f; case Prop::Sink: return 0.9f; case Prop::Table: return 0.75f;
        case Prop::Chair: return 0.9f; case Prop::Sofa: return 0.8f; case Prop::Bed: return 0.5f;
        case Prop::Wardrobe: return 2.0f; case Prop::Dresser: return 1.0f; case Prop::Bookshelf: return 1.9f;
        case Prop::Toilet: return 0.8f; case Prop::Bathtub: return 0.6f; case Prop::BathroomCabinet: return 1.5f;
        case Prop::Shelf: return 1.7f; case Prop::Cooler: return 1.9f; case Prop::Checkout: return 1.0f;
        case Prop::Desk: return 0.75f; case Prop::Locker: return 1.9f; case Prop::Pew: return 0.9f;
        case Prop::MedicalCabinet: return 1.8f; case Prop::GunCabinet: return 1.7f;
        case Prop::Workbench: return 0.9f; case Prop::Altar: return 1.0f; case Prop::HospitalBed: return 0.7f;
        default: return 0;
    }
}

const char* propName(Prop p) {
    static const char* names[] = {
        "", "Spruce", "Pine", "Birch", "Oak", "Maple", "Rowan",
        "Shrub", "Hedge", "Lingonberry shrubs", "Blueberry shrubs", "Wild rose", "Rock", "Boulder", "Stump", "Reeds",
        "Fence", "Street lamp", "Bench", "Mailbox", "Barrier", "Abandoned car", "Rubbish bin", "Hay bale", "Woodpile", "Rain barrel", "Pillar", "Crate",
        "Fridge", "Freezer", "Kitchen cupboard", "Stove", "Sink", "Table", "Chair", "Sofa", "Bed", "Wardrobe", "Chest of drawers",
        "Bookshelf", "Toilet", "Bathtub", "Bathroom cabinet", "Shelf", "Cooler", "Checkout", "Desk", "Locker",
        "Pew", "Medical cabinet", "Gun cabinet", "Workbench", "Altar", "Hospital bed",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == (std::size_t)Prop::Count, "prop names out of sync");
    return names[(int)p];
}

const char* propContainer(Prop p) {
    switch (p) {
        case Prop::Fridge: return "fridge";
        case Prop::Freezer: return "freezer";
        case Prop::Counter: return "kitchen_cupboard";
        case Prop::Wardrobe: return "wardrobe";
        case Prop::Dresser: return "dresser";
        case Prop::Bookshelf: return "bookshelf";
        case Prop::BathroomCabinet: return "bathroom_cabinet";
        case Prop::Desk: return "desk";
        case Prop::Locker: return "locker";
        case Prop::MedicalCabinet: return "hospital_cabinet";
        case Prop::GunCabinet: return "gun_cabinet";
        case Prop::Workbench: return "garage_shelf";
        case Prop::Woodpile: return "woodpile";
        case Prop::Car: return "desk";
        case Prop::Bin: return "rubbish_bin";
        case Prop::Crate: return "crate";
        default: return nullptr;
    }
}

} // namespace alone
