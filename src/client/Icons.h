#pragma once
#include "items/Item.h"
#include "raylib.h"
#include <string>

namespace alone {

// Small procedural icons, so the game needs no image assets.
void drawItemIcon(const ItemDef* def, Rectangle r);
void drawSymptomIcon(const std::string& id, Rectangle r, Color c);

} // namespace alone
