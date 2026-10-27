#pragma once
#include "core/Rng.h"
#include "items/Item.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace alone {

struct LootEntry { std::string itemId; float weight = 1; int minCount = 1, maxCount = 1; };

struct ContainerDef {
    std::string id;
    std::string name;
    float searchSeconds = 2.0f;
    int minRolls = 0, maxRolls = 3;
    float emptyChance = 0.2f;
    std::vector<LootEntry> loot;
    float totalWeight = 0;
};

// All item and container definitions, loaded from data/items/*.ini and
// data/loot/containers.ini.
class ItemRegistry {
public:
    bool loadDirectory(const std::string& itemsDir);
    bool loadContainers(const std::string& path);

    const ItemDef* find(const std::string& id) const;
    const ContainerDef* container(const std::string& id) const;
    const std::vector<std::unique_ptr<ItemDef>>& all() const { return items_; }

    // Create a fresh instance of an item with randomised state.
    ItemInstance create(const std::string& id, Rng& rng) const;
    ItemInstance create(const ItemDef* def, Rng& rng) const;

    // Roll the loot for a container. `seed` makes it repeatable.
    std::vector<ItemInstance> rollLoot(const std::string& containerId, uint32_t seed) const;

private:
    std::vector<std::unique_ptr<ItemDef>> items_;
    std::unordered_map<std::string, const ItemDef*> byId_;
    std::unordered_map<std::string, ContainerDef> containers_;
    mutable uint32_t nextUid_ = 1;
};

} // namespace alone
