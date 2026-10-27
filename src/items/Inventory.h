#pragma once
#include "items/Item.h"
#include <functional>
#include <vector>

namespace alone {

class Inventory {
public:
    std::vector<ItemInstance>& items() { return items_; }
    const std::vector<ItemInstance>& items() const { return items_; }

    void add(const ItemInstance& it) { if (it.def) items_.push_back(it); }
    ItemInstance take(int index);
    float totalWeight() const;
    bool empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }

    int findTag(const std::string& tag) const;    // -1 if none
    int findId(const std::string& id) const;
    int find(const std::function<bool(const ItemInstance&)>& pred) const;
    int indexOfUid(uint32_t uid) const;

    // Age food, used by the world clock.
    void age(float hours, float temperatureC);

private:
    std::vector<ItemInstance> items_;
};

} // namespace alone
