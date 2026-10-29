#include "items/Item.h"
#include <algorithm>
#include <cstdio>

namespace alone {

const char* categoryName(ItemCategory c) {
    static const char* n[] = {"Food", "Drink", "Weapon", "Tool", "Medical", "Clothing",
                              "Container", "Ammo", "Material", "Literature", "Misc"};
    return n[(int)c];
}

const char* slotName(ClothingSlot s) {
    static const char* n[] = {"None", "Head", "Torso", "Sweater", "Jacket", "Legs", "Feet", "Hands", "Back"};
    return n[(int)s];
}

float ItemInstance::weightKg() const {
    if (!def) return 0;
    float w = def->weightKg;
    if (def->isEdible() && def->capacityMl <= 0) w *= std::max(0.05f, portion);
    w += liquidMl / 1000.0f;
    return w;
}

float ItemInstance::spoilage() const {
    if (!def || def->spoilHours <= 0) return 0;
    if (def->sealed && !opened) return 0;
    return std::min(1.5f, ageHours / def->spoilHours);
}

std::string ItemInstance::displayName(bool knowsMushrooms) const {
    if (!def) return "?";
    std::string n = (knowsMushrooms && !def->trueName.empty()) ? def->trueName : def->name;
    if (opened && !def->openedName.empty()) n = def->openedName;
    if (def->capacityMl > 0) {
        if (liquidMl <= 0) return "Empty " + n;
        char buf[64];
        if (liquid == 1) std::snprintf(buf, sizeof buf, " (%.1f l petrol)", liquidMl / 1000.0f);
        else if (liquidMl >= 2000) std::snprintf(buf, sizeof buf, " (%.1f l water%s)", liquidMl / 1000.0f, liquidSafe ? "" : ", untreated");
        else std::snprintf(buf, sizeof buf, " (%.0f ml%s)", liquidMl, liquidSafe ? "" : ", untreated");
        return n + buf;
    }
    if (def->isEdible()) {
        float s = spoilage();
        if (s >= 1.0f) n = "Rotten " + n;
        else if (s >= 0.6f) n = "Stale " + n;
        if (def->needsCanOpener && !opened) n += " (sealed)";
        if (portion < 0.99f) {
            char buf[32];
            std::snprintf(buf, sizeof buf, " (%d%% left)", (int)(portion * 100 + 0.5f));
            n += buf;
        }
    }
    if (def->magazine > 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, " [%d/%d]", usesLeft, def->magazine);
        n += buf;
    }
    if (def->uses > 0 && def->category != ItemCategory::Food) {
        char buf[32];
        std::snprintf(buf, sizeof buf, " [%d]", usesLeft);
        n += buf;
    }
    return n;
}

} // namespace alone
