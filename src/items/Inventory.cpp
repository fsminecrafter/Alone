#include "items/Inventory.h"
#include <cmath>

namespace alone {

ItemInstance Inventory::take(int index) {
    if (index < 0 || index >= (int)items_.size()) return {};
    ItemInstance it = items_[index];
    items_.erase(items_.begin() + index);
    return it;
}

float Inventory::totalWeight() const {
    float w = 0;
    for (const auto& it : items_) w += it.weightKg();
    return w;
}

int Inventory::findTag(const std::string& tag) const {
    return find([&](const ItemInstance& it) { return it.def->hasTag(tag); });
}

int Inventory::findId(const std::string& id) const {
    return find([&](const ItemInstance& it) { return it.def->id == id; });
}

int Inventory::find(const std::function<bool(const ItemInstance&)>& pred) const {
    for (int i = 0; i < (int)items_.size(); ++i)
        if (items_[i].def && pred(items_[i])) return i;
    return -1;
}

int Inventory::indexOfUid(uint32_t uid) const {
    for (int i = 0; i < (int)items_.size(); ++i)
        if (items_[i].uid == uid) return i;
    return -1;
}

void Inventory::age(float hours, float temperatureC) {
    // Q10 rule of thumb: spoilage roughly doubles for every 10 degrees warmer (reference 20 C)
    float rate = std::pow(2.0f, (temperatureC - 20.0f) / 10.0f);
    for (auto& it : items_) {
        if (it.def->spoilHours <= 0) continue;
        if (it.def->sealed && !it.opened) continue;
        it.ageHours += hours * rate;
    }
}

} // namespace alone
