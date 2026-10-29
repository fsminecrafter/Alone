// The inventory (a grid of item tiles, a figure for worn clothes and a strip
// with what you can do with the selected item) and the build menu.
#include "client/App.h"
#include "client/Icons.h"
#include "client/Input.h"
#include "client/MeshBuilder.h"
#include "client/Ui.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>

namespace alone {

namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list a;
    va_start(a, f);
    vsnprintf(buf, sizeof buf, f, a);
    va_end(a);
    return buf;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Inventory tabs
const char* kTabs[] = {"All", "Food", "Medical", "Gear", "Clothes", "Other"};
int tabOf(const ItemDef& d) {
    switch (d.category) {
        case ItemCategory::Food: case ItemCategory::Drink: return 1;
        case ItemCategory::Medical: return 2;
        case ItemCategory::Weapon: case ItemCategory::Tool: case ItemCategory::Ammo: return 3;
        case ItemCategory::Clothing: case ItemCategory::Container: return 4;
        default: return d.capacityMl > 0 ? 1 : 5;
    }
}

Color tileTint(const ItemDef& d) {
    switch (tabOf(d)) {
        case 1: return Color{46, 52, 36, 255};
        case 2: return Color{58, 38, 40, 255};
        case 3: return Color{40, 44, 52, 255};
        case 4: return Color{50, 44, 56, 255};
        default: return Color{42, 42, 42, 255};
    }
}

std::string describe(Game& game, const ItemInstance& it) {
    const ItemDef& d = *it.def;
    std::string body = d.description;
    auto add = [&](const std::string& line) { body += (body.empty() ? "" : "\n") + line; };
    add(fmt("Weight: %.2f kg", it.weightKg()));
    if (d.isEdible() && d.capacityMl <= 0) {
        float sp = it.spoilage();
        if (d.spoilHours > 0) add(sp >= 1 ? "It smells rotten." : sp > 0.6f ? "It doesn't smell quite right." : sp > 0.3f ? "Still fine." : "Fresh.");
        if (d.raw) add("Raw.");
        if (d.hasTag("labelled")) {
            Nutrients n = d.nutrients.scaled(it.portion);
            add(fmt("Label: %.0f kcal - fat %.0f g, carbohydrate %.0f g, protein %.0f g", n.kcal(), n.fat, n.carbs, n.protein));
        }
    }
    if (d.capacityMl > 0) add(fmt("Holds %.0f of %.0f ml%s", it.liquidMl, d.capacityMl, it.liquidMl > 0 && !it.liquidSafe ? " (untreated)" : ""));
    if (d.category == ItemCategory::Clothing && d.clo > 0) {
        std::string warmth = d.clo >= 0.8f ? "very warm" : d.clo >= 0.4f ? "warm" : d.clo >= 0.15f ? "a little warm" : "barely warm";
        add("It's " + warmth + (d.waterproof > 0.8f ? " and waterproof." : d.waterproof > 0.4f ? " and keeps some rain out." : "."));
    }
    if (d.category == ItemCategory::Weapon) {
        std::string kind = d.firearm ? (d.noise > 100 ? "Loud. " : "Quiet. ") : "";
        add(kind + (d.damage >= 40 ? "Deadly." : d.damage >= 15 ? "Hits hard." : d.damage >= 9 ? "A decent weapon." : "Better than your fists."));
        if (d.firearm) {
            const ItemDef* a = game.items().find(d.ammo);
            if (a) add("Takes " + lower(a->name) + ".");
        }
    }
    if (d.carryBonusKg > 0) add(fmt("Lets you carry %.0f kg more.", d.carryBonusKg));
    if (d.hasTag("gascan") && it.liquidMl > 0 && it.liquid == 1) add("It sloshes and smells of petrol.");
    if (d.uses > 0 && d.category != ItemCategory::Food) add(fmt("%d left.", it.usesLeft));
    return body;
}

// 0..1 for the little bar on a tile (-1: none)
float tileBar(const ItemInstance& it) {
    const ItemDef& d = *it.def;
    if (d.capacityMl > 0) return it.liquidMl / d.capacityMl;
    if (d.isEdible() && it.portion < 0.999f) return it.portion;
    if (d.uses > 1 && d.category != ItemCategory::Ammo) return it.usesLeft / (float)d.uses;
    if (it.condition < 0.98f) return it.condition;
    return -1;
}

} // namespace

void App::drawItemTooltip(const ItemInstance& it) {
    ui::tooltip(game_.itemName(it), describe(game_, it));
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

std::vector<App::ItemStack> App::stacksOf(const std::vector<ItemRef>& refs, int tab, int sort) {
    std::vector<ItemStack> out;
    std::map<std::string, size_t> byKey;
    for (const auto& ref : refs) {
        ItemInstance* it = game_.resolve(ref);
        if (!it || !it->def) continue;
        if (tab > 0 && tabOf(*it->def) != tab) continue;
        // identical things share a tile: same item in the same state
        bool held = ref.loc == ItemLoc::Player && it->uid == game_.player().weaponUid;
        std::string key = fmt("%s|%d|%d|%d%d|%d|%d|%d|%d|%u", it->def->id.c_str(), (int)(it->portion * 100), (int)it->liquidMl, (int)it->liquidSafe, (int)it->liquid,
                              (int)it->opened, it->usesLeft, (int)(it->spoilage() * 4), (int)(it->condition * 5), held ? it->uid : 0u);
        auto f = byKey.find(key);
        if (f == byKey.end()) {
            byKey[key] = out.size();
            out.push_back({{ref}, it});
        } else {
            out[f->second].refs.push_back(ref);
        }
    }
    auto name = [&](const ItemStack& s) { return game_.itemName(*s.top); };
    if (sort == 1) std::stable_sort(out.begin(), out.end(), [&](const ItemStack& a, const ItemStack& b) { return name(a) < name(b); });
    else if (sort == 2)
        std::stable_sort(out.begin(), out.end(), [&](const ItemStack& a, const ItemStack& b) {
            return a.top->weightKg() * a.refs.size() > b.top->weightKg() * b.refs.size();
        });
    else
        std::stable_sort(out.begin(), out.end(), [&](const ItemStack& a, const ItemStack& b) {
            int ta = tabOf(*a.top->def), tb = tabOf(*b.top->def);
            return ta != tb ? ta < tb : name(a) < name(b);
        });
    return out;
}

void App::moveStack(const ItemStack& st, ItemLoc to, bool all) {
    for (const auto& r : st.refs) {
        game_.transfer(r, to);
        if (!all) break;
    }
    sound_.play({Sfx::Click, game_.player().pos}, game_.player().pos, settings_.sfxVolume * 0.6f);
}

void App::drawItemTile(Rectangle r, const ItemInstance& it, int count, bool selected, bool hov) {
    float s = ui::scale();
    Color bg = tileTint(*it.def);
    if (hov) bg = shade(bg, 1.35f);
    DrawRectangleRounded(r, 0.12f, 4, bg);
    if (selected) DrawRectangleRoundedLinesEx(r, 0.12f, 4, 2.0f * s, ui::theme().accent);
    else DrawRectangleRoundedLinesEx(r, 0.12f, 4, 1.0f, Color{80, 80, 80, 120});
    float pad = r.width * 0.16f;
    drawItemIcon(it.def, {r.x + pad, r.y + pad * 0.8f, r.width - 2 * pad, r.height - 2 * pad});
    if (it.rotten()) DrawRectangleRounded(r, 0.12f, 4, Color{90, 110, 40, 90});
    if (count > 1) {
        std::string c = "\xC3\x97" + std::to_string(count);   // ×3
        Vector2 m = ui::measure(c, 13 * s, true);
        ui::text(c, r.x + r.width - m.x - 4 * s, r.y + r.height - 17 * s, 13 * s, ui::theme().text, true);
    }
    if (it.uid == game_.player().weaponUid) {
        DrawRectangleRounded({r.x + 3 * s, r.y + 3 * s, 15 * s, 15 * s}, 0.3f, 4, ui::theme().accent);
        ui::textCentered("E", r.x + 10.5f * s, r.y + 3.5f * s, 12 * s, Color{20, 20, 20, 255}, true);
    }
    float bar = tileBar(it);
    if (bar >= 0) {
        Rectangle b{r.x + 5 * s, r.y + r.height - 6 * s, r.width - 10 * s, 3 * s};
        DrawRectangleRec(b, Color{20, 20, 20, 200});
        Color bc = bar > 0.5f ? ui::theme().good : bar > 0.2f ? ui::theme().accent : ui::theme().bad;
        if (it.def->capacityMl > 0) bc = it.liquid == 1 ? Color{220, 150, 40, 255} : it.liquidSafe ? Color{90, 150, 220, 255} : Color{140, 130, 90, 255};
        DrawRectangleRec({b.x, b.y, b.width * std::clamp(bar, 0.0f, 1.0f), b.height}, bc);
    }
}

void App::drawItemGrid(Rectangle area, std::vector<ItemStack>& stacks, int pane, ItemLoc target) {
    float s = ui::scale();
    paneRect_[pane] = area;
    DrawRectangleRounded(area, 0.02f, 4, Color{8, 9, 10, 170});
    float tile = 58 * s, gap = 6 * s;
    int cols = std::max(1, (int)((area.width - gap) / (tile + gap)));
    float used = cols * (tile + gap) - gap;
    float x0 = area.x + (area.width - used) / 2;
    int rows = ((int)stacks.size() + cols - 1) / cols;
    float contentH = rows * (tile + gap) + gap;
    if (ui::hovered(area) && !menu_.open) scroll_[pane] -= GetMouseWheelMove() * (tile + gap);
    scroll_[pane] = std::clamp(scroll_[pane], 0.0f, std::max(0.0f, contentH - area.height));
    BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    for (size_t i = 0; i < stacks.size(); ++i) {
        ItemStack& st = stacks[i];
        int cx = (int)i % cols, cy = (int)i / cols;
        Rectangle r{x0 + cx * (tile + gap), area.y + gap + cy * (tile + gap) - scroll_[pane], tile, tile};
        if (r.y > area.y + area.height || r.y + tile < area.y) continue;
        bool hov = ui::hovered(r) && ui::hovered(area) && !drag_.active && !menu_.open;
        bool sel = false;
        for (const auto& ref : st.refs) sel |= ref.uid == selected_.uid;
        bool dragging = drag_.active && drag_.stack.refs.size() && drag_.stack.refs[0].uid == st.refs[0].uid;
        if (dragging) { DrawRectangleRounded(r, 0.12f, 4, Color{30, 30, 30, 200}); continue; }
        drawItemTile(r, *st.top, (int)st.refs.size(), sel, hov);
        if (!hov) continue;
        drawItemTooltip(*st.top);
        if (input::pressed(MOUSE_BUTTON_LEFT)) {
            double now = GetTime();
            if (shift) {
                moveStack(st, target, true);   // shift-click: the whole stack across
                drag_ = Drag{};
            } else if (lastClickUid_ == st.refs[0].uid && now - lastClickTime_ < 0.35) {
                moveStack(st, target, false);
                lastClickUid_ = 0;
                drag_ = Drag{};
            } else {
                selected_ = st.refs[0];
                lastClickUid_ = st.refs[0].uid;
                lastClickTime_ = now;
                drag_.stack = st;
                drag_.armed = true;
                drag_.active = false;
                drag_.start = GetMousePosition();
                drag_.fromPane = pane;
            }
        }
        if (input::pressed(MOUSE_BUTTON_RIGHT)) { selected_ = st.refs[0]; openItemMenu(st.refs[0], GetMousePosition()); }
    }
    EndScissorMode();
    if (stacks.empty()) ui::textCentered(invTab_ == 0 ? "Nothing here." : "Nothing of that kind.", area.x + area.width / 2, area.y + 20 * s, 15 * s, ui::theme().dim);
    ui::claim(area);
}

void App::drawPaperDoll(Rectangle a) {
    float s = ui::scale();
    Player& pl = game_.player();
    dollRect_ = a;
    DrawRectangleRounded(a, 0.03f, 4, Color{8, 9, 10, 170});
    // A faint figure behind the slots
    float cx = a.x + a.width / 2, top = a.y + 14 * s;
    Color fig{60, 62, 66, 120};
    DrawCircle((int)cx, (int)(top + 30 * s), 22 * s, fig);
    DrawRectangleRounded({cx - 30 * s, top + 56 * s, 60 * s, 110 * s}, 0.3f, 6, fig);
    DrawRectangleRounded({cx - 46 * s, top + 60 * s, 14 * s, 96 * s}, 0.5f, 6, fig);
    DrawRectangleRounded({cx + 32 * s, top + 60 * s, 14 * s, 96 * s}, 0.5f, 6, fig);
    DrawRectangleRounded({cx - 27 * s, top + 160 * s, 24 * s, 130 * s}, 0.4f, 6, fig);
    DrawRectangleRounded({cx + 3 * s, top + 160 * s, 24 * s, 130 * s}, 0.4f, 6, fig);

    float t = 52 * s;
    struct Slot { ClothingSlot slot; float x, y; };
    float lx = a.x + 10 * s, rx = a.x + a.width - 10 * s - t, mx = cx - t / 2;
    const Slot slots[] = {
        {ClothingSlot::Head, mx, top + 4 * s},
        {ClothingSlot::Torso, lx, top + 62 * s}, {ClothingSlot::Sweater, lx, top + 130 * s}, {ClothingSlot::Jacket, lx, top + 198 * s},
        {ClothingSlot::Hands, rx, top + 62 * s}, {ClothingSlot::Back, rx, top + 130 * s},
        {ClothingSlot::Legs, mx, top + 200 * s}, {ClothingSlot::Feet, mx, top + 270 * s},
    };
    bool draggingClothes = false;
    if (drag_.active && !drag_.stack.refs.empty())
        if (ItemInstance* it = game_.resolve(drag_.stack.refs[0])) draggingClothes = it->def->category == ItemCategory::Clothing;
    for (const auto& sl : slots) {
        Rectangle r{sl.x, sl.y, t, t};
        const ItemInstance& it = pl.worn[(size_t)sl.slot];
        bool hov = ui::hovered(r) && !menu_.open && !drag_.active;
        if (it.def) {
            bool sel = selected_.uid == it.uid;
            drawItemTile(r, it, 1, sel, hov);
            ItemRef ref{ItemLoc::Worn, 0, 0, 0, it.uid};
            if (hov) {
                drawItemTooltip(it);
                if (input::pressed(MOUSE_BUTTON_LEFT)) {
                    double now = GetTime();
                    if (lastClickUid_ == it.uid && now - lastClickTime_ < 0.35) { game_.transfer(ref, ItemLoc::Player); lastClickUid_ = 0; }
                    else {
                        selected_ = ref; lastClickUid_ = it.uid; lastClickTime_ = now;
                        drag_.stack = ItemStack{{ref}, const_cast<ItemInstance*>(&it)};
                        drag_.armed = true; drag_.active = false; drag_.start = GetMousePosition(); drag_.fromPane = 2;
                    }
                }
                if (input::pressed(MOUSE_BUTTON_RIGHT)) { selected_ = ref; openItemMenu(ref, GetMousePosition()); }
            }
        } else {
            DrawRectangleRounded(r, 0.12f, 4, draggingClothes ? Color{60, 54, 36, 220} : Color{24, 25, 27, 220});
            DrawRectangleRoundedLinesEx(r, 0.12f, 4, 1.0f, Color{70, 70, 70, 160});
            ui::textCentered(slotName(sl.slot), r.x + t / 2, r.y + t / 2 - 7 * s, 11 * s, ui::theme().dim);
        }
    }
    // What's in your hand
    Rectangle hand{rx, top + 198 * s, t, t};
    if (const ItemInstance* w = game_.weapon()) {
        drawItemTile(hand, *w, 1, selected_.uid == w->uid, ui::hovered(hand));
        if (ui::hovered(hand) && !menu_.open) {
            drawItemTooltip(*w);
            ItemRef ref{ItemLoc::Player, 0, 0, 0, w->uid};
            if (input::pressed(MOUSE_BUTTON_LEFT)) selected_ = ref;
            if (input::pressed(MOUSE_BUTTON_RIGHT)) openItemMenu(ref, GetMousePosition());
        }
    } else {
        DrawRectangleRounded(hand, 0.12f, 4, Color{24, 25, 27, 220});
        DrawRectangleRoundedLinesEx(hand, 0.12f, 4, 1.0f, Color{70, 70, 70, 160});
        ui::textCentered("Hand", hand.x + t / 2, hand.y + t / 2 - 7 * s, 11 * s, ui::theme().dim);
    }

    // Weight and warmth under the figure
    float y = top + 336 * s;
    float carried = game_.carriedKg(), cap = game_.carryCapacityKg();
    ui::text(fmt("Weight  %.1f / %.0f kg", carried, cap), a.x + 12 * s, y, 14 * s, carried > cap ? ui::theme().bad : ui::theme().text, true);
    Rectangle bar{a.x + 12 * s, y + 20 * s, a.width - 24 * s, 7 * s};
    DrawRectangleRounded(bar, 0.5f, 4, Color{30, 30, 30, 255});
    float f = std::min(1.0f, carried / std::max(1.0f, cap));
    Color bc = f < 0.7f ? ui::theme().good : f < 1.0f ? ui::theme().accent : ui::theme().bad;
    DrawRectangleRounded({bar.x, bar.y, bar.width * f, bar.height}, 0.5f, 4, bc);
    float clo = 0, wp = 0;
    for (const auto& c : pl.worn)
        if (c.def) { clo += c.def->clo; if (c.def->slot == ClothingSlot::Jacket) wp = std::max(wp, c.def->waterproof); }
    std::string warm = clo >= 2.2f ? "Dressed for winter" : clo >= 1.4f ? "Warmly dressed" : clo >= 0.8f ? "Dressed for autumn" : "Lightly dressed";
    ui::text(warm + (wp > 0.7f ? ", rainproof" : wp > 0.3f ? ", showerproof" : ""), a.x + 12 * s, y + 34 * s, 13 * s, ui::theme().dim);
    ui::claim(a);
}

void App::drawItemDetails(Rectangle a) {
    float s = ui::scale();
    DrawRectangleRounded(a, 0.05f, 4, Color{22, 23, 25, 230});
    ItemInstance* it = selected_.uid ? game_.resolve(selected_) : nullptr;
    if (!it) {
        ui::paragraph("Click an item to see what you can do with it. Double-click moves one, shift-click moves the whole stack, "
                      "or drag it across. Drag clothes onto the figure to put them on. Right-click for everything else.",
                      a.x + 14 * s, a.y + 14 * s, a.width - 28 * s, 14 * s, ui::theme().dim);
        ui::claim(a);
        return;
    }
    Rectangle icon{a.x + 10 * s, a.y + 10 * s, 64 * s, 64 * s};
    drawItemTile(icon, *it, 1, false, false);
    float tx = icon.x + icon.width + 12 * s;
    ui::text(game_.itemName(*it), tx, a.y + 8 * s, 18 * s, ui::theme().text, true);
    std::string desc = describe(game_, *it);
    std::replace(desc.begin(), desc.end(), '\n', ' ');
    float textW = std::min(a.width * 0.45f, a.width - (tx - a.x) - 10 * s);
    BeginScissorMode((int)tx, (int)(a.y + 30 * s), (int)textW, (int)(a.height - 34 * s));
    ui::paragraph(desc, tx, a.y + 32 * s, textW, 13 * s, ui::theme().dim, 1.2f);
    EndScissorMode();

    // What you can do, as buttons
    auto opts = game_.actionsForItem(selected_);
    float bx = tx + textW + 14 * s, by = a.y + 10 * s, right = a.x + a.width - 10 * s;
    for (auto& o : opts) {
        float w = ui::measure(o.label, 14 * s).x + 22 * s;
        if (bx + w > right) { bx = tx + textW + 14 * s; by += 32 * s; }
        if (by + 26 * s > a.y + a.height) break;
        Rectangle b{bx, by, w, 26 * s};
        if (ui::button(b, o.label, o.enabled, 14)) {
            ActionOption copy = o;
            game_.perform(copy);
        }
        if (!o.enabled && ui::hovered(b) && !o.reason.empty()) ui::tooltip("", o.reason);
        bx += w + 8 * s;
    }
    ui::claim(a);
}

void App::drawInventory() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    float pw = std::min(1220 * s, W - 16), ph = std::min(660 * s, H - 16);
    Rectangle p{W / 2 - pw / 2, H / 2 - ph / 2, pw, ph};
    ui::panel(p, Color{16, 17, 19, 240});
    ui::claim(p);
    Player& pl = game_.player();
    float pad = 16 * s, dollW = 236 * s, detailsH = 96 * s;
    ui::text("Inventory", p.x + pad, p.y + 12 * s, 22 * s, ui::theme().text, true);
    if (ui::button({p.x + p.width - 44 * s, p.y + 10 * s, 32 * s, 28 * s}, "X")) { overlay_ = Overlay::None; return; }

    // The other side: an open container in reach, otherwise the ground
    bool container = false;
    if (game_.hasOpenContainer()) {
        Vec2 cpos{game_.openContainerX() + 0.5f, game_.openContainerY() + 0.5f};
        if (distance(cpos, pl.pos) < 2.6f) container = true;
        else game_.closeContainer();
    }
    std::vector<ItemRef> mine, other;
    for (const auto& it : pl.inventory.items()) mine.push_back({ItemLoc::Player, 0, 0, 0, it.uid});
    std::string otherTitle = "On the ground";
    if (container) {
        int cx = game_.openContainerX(), cy = game_.openContainerY();
        ContainerState& cs = game_.world().container(cx, cy);
        const ContainerDef* cd = game_.items().container(cs.kind);
        otherTitle = cd ? cd->name : "Container";
        for (const auto& it : cs.items) other.push_back({ItemLoc::Container, cx, cy, 0, it.uid});
    } else {
        for (auto& [key, idx] : game_.world().groundItemsNear(pl.pos, 1.6f)) {
            auto* st = game_.world().groundStack(key);
            if (st && idx < (int)st->size()) other.push_back({ItemLoc::Ground, 0, 0, key, (*st)[idx].uid});
        }
    }
    ItemLoc otherLoc = container ? ItemLoc::Container : ItemLoc::Ground;
    paneLoc_[0] = ItemLoc::Player;
    paneLoc_[1] = otherLoc;

    // Columns: figure | yours | theirs
    float top = p.y + 50 * s;
    float gridH = p.height - (top - p.y) - detailsH - 2 * pad - 34 * s;
    Rectangle doll{p.x + pad, top, dollW, p.height - (top - p.y) - pad};
    float gw = (p.width - dollW - 4 * pad) / 2;
    Rectangle ga{doll.x + dollW + pad, top + 34 * s, gw, gridH};
    Rectangle gb{ga.x + gw + pad, top + 34 * s, gw, gridH};
    Rectangle det{ga.x, ga.y + gridH + pad, gw * 2 + pad, detailsH};

    drawPaperDoll(doll);

    // Tabs and sorting over your grid
    float tx = ga.x;
    for (int i = 0; i < 6; ++i) {
        float w = ui::measure(kTabs[i], 13 * s).x + 16 * s;
        Rectangle r{tx, top, w, 26 * s};
        bool on = invTab_ == i;
        bool hov = ui::hovered(r);
        DrawRectangleRounded(r, 0.3f, 4, on ? Color{90, 72, 40, 255} : hov ? Color{50, 50, 52, 255} : Color{32, 33, 35, 255});
        ui::textCentered(kTabs[i], r.x + w / 2, r.y + 5 * s, 13 * s, on ? ui::theme().text : ui::theme().dim, on);
        if (hov && input::pressed(MOUSE_BUTTON_LEFT)) { invTab_ = i; scroll_[0] = 0; }
        tx += w + 4 * s;
    }
    static const char* sorts[] = {"Type", "Name", "Weight"};
    std::string st = std::string("Sort: ") + sorts[invSort_];
    float sw = ui::measure(st, 13 * s).x + 16 * s;
    if (tx + sw < ga.x + ga.width + 1 && ui::button({ga.x + ga.width - sw, top, sw, 26 * s}, st, true, 13)) invSort_ = (invSort_ + 1) % 3;

    auto myStacks = stacksOf(mine, invTab_, invSort_);
    auto otherStacks = stacksOf(other, 0, invSort_);
    drawItemGrid(ga, myStacks, 0, otherLoc);

    // Their side, framed like the piece of furniture it is
    if (container) {
        Rectangle frame{gb.x - 4 * s, gb.y - 38 * s, gb.width + 8 * s, gb.height + 42 * s};
        DrawRectangleRounded(frame, 0.02f, 4, Color{34, 28, 18, 220});
        DrawRectangleRoundedLinesEx(frame, 0.02f, 4, 2.0f, Color{150, 118, 64, 255});
    }
    ui::text(otherTitle, gb.x, top + 4 * s, 17 * s, ui::theme().accent, true);
    if (!other.empty() && ui::button({gb.x + gb.width - 96 * s, top, 96 * s, 26 * s}, "Take all", true, 13)) {
        for (const auto& r : other) game_.transfer(r, ItemLoc::Player);
        sound_.play({Sfx::Click, pl.pos}, pl.pos, settings_.sfxVolume * 0.6f);
    }
    drawItemGrid(gb, otherStacks, 1, ItemLoc::Player);
    drawItemDetails(det);

    // Dragging a stack: onto the other grid, or clothes onto the figure
    if (drag_.armed) {
        Vector2 m = GetMousePosition();
        if (!drag_.active && Vector2Distance(m, drag_.start) > 8 * s) drag_.active = true;
        ItemInstance* it = drag_.stack.refs.empty() ? nullptr : game_.resolve(drag_.stack.refs[0]);
        if (drag_.active && it) {
            drawItemTile({m.x - 26 * s, m.y - 26 * s, 52 * s, 52 * s}, *it, (int)drag_.stack.refs.size(), true, false);
        }
        if (input::released(MOUSE_BUTTON_LEFT)) {
            if (drag_.active && it) {
                bool all = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) || drag_.stack.refs.size() == 1 ? true : false;
                ItemLoc from = drag_.stack.refs[0].loc;
                if (CheckCollisionPointRec(m, dollRect_) && it->def->category == ItemCategory::Clothing && from != ItemLoc::Worn) {
                    for (auto& o : game_.actionsForItem(drag_.stack.refs[0]))
                        if (o.kind == ActionKind::Wear && o.enabled) { game_.perform(o); break; }
                    // from the ground or a cupboard: pick it up first, then put it on
                    if (from != ItemLoc::Player && game_.transfer(drag_.stack.refs[0], ItemLoc::Player)) {
                        ItemRef mineRef{ItemLoc::Player, 0, 0, 0, drag_.stack.refs[0].uid};
                        for (auto& o : game_.actionsForItem(mineRef))
                            if (o.kind == ActionKind::Wear && o.enabled) { game_.perform(o); break; }
                    }
                } else if (from == ItemLoc::Worn) {
                    if (CheckCollisionPointRec(m, paneRect_[0])) game_.transfer(drag_.stack.refs[0], ItemLoc::Player);
                    else if (CheckCollisionPointRec(m, paneRect_[1])) game_.transfer(drag_.stack.refs[0], paneLoc_[1]);
                } else {
                    for (int pane = 0; pane < 2; ++pane)
                        if (pane != drag_.fromPane && CheckCollisionPointRec(m, paneRect_[pane]))
                            moveStack(drag_.stack, paneLoc_[pane], all || drag_.stack.refs.size() == 1);
                }
            }
            drag_ = Drag{};
        }
    }
}

// ---------------------------------------------------------------------------
// Carpentry
// ---------------------------------------------------------------------------

namespace {

// A small picture of each thing you can build
void drawBuildIcon(BuildKind k, Rectangle r) {
    float x = r.x, y = r.y, w = r.width, h = r.height;
    Color wood{168, 128, 82, 255}, dark{104, 76, 46, 255}, light{200, 164, 112, 255}, glass{120, 170, 210, 255}, metal{110, 116, 122, 255};
    auto planks = [&](Rectangle q, bool vertical) {
        DrawRectangleRec(q, wood);
        int n = 4;
        for (int i = 1; i < n; ++i) {
            if (vertical) DrawLineEx({q.x + q.width * i / n, q.y}, {q.x + q.width * i / n, q.y + q.height}, 1.5f, dark);
            else DrawLineEx({q.x, q.y + q.height * i / n}, {q.x + q.width, q.y + q.height * i / n}, 1.5f, dark);
        }
        DrawRectangleLinesEx(q, 1.5f, dark);
    };
    switch (k) {
        case BuildKind::Wall: planks({x + w * 0.15f, y + h * 0.15f, w * 0.7f, h * 0.7f}, true); break;
        case BuildKind::Door:
            planks({x + w * 0.15f, y + h * 0.12f, w * 0.7f, h * 0.76f}, true);
            DrawRectangleRec({x + w * 0.32f, y + h * 0.24f, w * 0.36f, h * 0.64f}, dark);
            DrawRectangleRec({x + w * 0.35f, y + h * 0.27f, w * 0.3f, h * 0.61f}, light);
            DrawCircle((int)(x + w * 0.6f), (int)(y + h * 0.58f), w * 0.03f, metal);
            break;
        case BuildKind::Window:
            planks({x + w * 0.15f, y + h * 0.15f, w * 0.7f, h * 0.7f}, true);
            DrawRectangleRec({x + w * 0.32f, y + h * 0.3f, w * 0.36f, h * 0.32f}, light);
            DrawRectangleRec({x + w * 0.35f, y + h * 0.33f, w * 0.3f, h * 0.26f}, glass);
            DrawLineEx({x + w * 0.5f, y + h * 0.33f}, {x + w * 0.5f, y + h * 0.59f}, 2, light);
            break;
        case BuildKind::Floor: {
            Vector2 a{x + w * 0.1f, y + h * 0.62f}, b{x + w * 0.5f, y + h * 0.4f}, c{x + w * 0.9f, y + h * 0.62f}, d{x + w * 0.5f, y + h * 0.84f};
            DrawTriangle(a, d, c, wood);
            DrawTriangle(a, c, b, wood);
            for (int i = 1; i < 4; ++i) {
                float t = i / 4.0f;
                DrawLineEx({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}, {d.x + (c.x - d.x) * t, d.y + (c.y - d.y) * t}, 1.5f, dark);
            }
            break;
        }
        case BuildKind::Roof:
            DrawTriangle({x + w * 0.5f, y + h * 0.18f}, {x + w * 0.08f, y + h * 0.62f}, {x + w * 0.92f, y + h * 0.62f}, dark);
            DrawTriangle({x + w * 0.5f, y + h * 0.26f}, {x + w * 0.2f, y + h * 0.58f}, {x + w * 0.8f, y + h * 0.58f}, wood);
            DrawRectangleRec({x + w * 0.24f, y + h * 0.6f, w * 0.52f, h * 0.24f}, Color{70, 70, 74, 255});
            break;
        case BuildKind::Crate:
            planks({x + w * 0.18f, y + h * 0.3f, w * 0.64f, h * 0.5f}, false);
            DrawRectangleRec({x + w * 0.14f, y + h * 0.24f, w * 0.72f, h * 0.1f}, dark);
            break;
        case BuildKind::RainCollector:
            DrawTriangle({x + w * 0.15f, y + h * 0.2f}, {x + w * 0.5f, y + h * 0.42f}, {x + w * 0.85f, y + h * 0.2f}, Color{60, 100, 150, 255});
            DrawRectangleRounded({x + w * 0.3f, y + h * 0.42f, w * 0.4f, h * 0.44f}, 0.2f, 4, Color{50, 90, 60, 255});
            DrawLineEx({x + w * 0.3f, y + h * 0.55f}, {x + w * 0.7f, y + h * 0.55f}, 2, Color{30, 60, 40, 255});
            break;
        case BuildKind::Bed:
            DrawRectangleRec({x + w * 0.1f, y + h * 0.52f, w * 0.8f, h * 0.16f}, wood);
            DrawRectangleRec({x + w * 0.12f, y + h * 0.44f, w * 0.76f, h * 0.1f}, Color{200, 200, 196, 255});
            DrawRectangleRec({x + w * 0.14f, y + h * 0.38f, w * 0.2f, h * 0.08f}, Color{230, 230, 226, 255});
            DrawRectangleRec({x + w * 0.1f, y + h * 0.3f, w * 0.06f, h * 0.5f}, dark);
            DrawRectangleRec({x + w * 0.84f, y + h * 0.45f, w * 0.06f, h * 0.35f}, dark);
            break;
        case BuildKind::Fence:
            for (int i = 0; i < 4; ++i) DrawRectangleRec({x + w * (0.14f + i * 0.2f), y + h * 0.28f, w * 0.08f, h * 0.56f}, wood);
            DrawRectangleRec({x + w * 0.1f, y + h * 0.4f, w * 0.8f, h * 0.06f}, dark);
            DrawRectangleRec({x + w * 0.1f, y + h * 0.66f, w * 0.8f, h * 0.06f}, dark);
            break;
        default: break;
    }
}

struct BuildGroup { const char* name; std::vector<BuildKind> kinds; };
const std::vector<BuildGroup>& buildGroups() {
    static const std::vector<BuildGroup> g = {
        {"Walls & roof", {BuildKind::Wall, BuildKind::Door, BuildKind::Window, BuildKind::Floor, BuildKind::Roof}},
        {"Furniture", {BuildKind::Bed, BuildKind::Crate}},
        {"Outdoors", {BuildKind::Fence, BuildKind::RainCollector}},
    };
    return g;
}

} // namespace

void App::drawBuildMenu() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    float pw = std::min(940 * s, W - 20), ph = std::min(560 * s, H - 20);
    Rectangle p{W / 2 - pw / 2, H / 2 - ph / 2, pw, ph};
    ui::panel(p, Color{20, 19, 17, 242});
    ui::claim(p);
    ui::text("Build", p.x + 20 * s, p.y + 14 * s, 24 * s, ui::theme().text, true);
    if (ui::button({p.x + p.width - 44 * s, p.y + 10 * s, 32 * s, 28 * s}, "X")) { overlay_ = Overlay::None; return; }

    const Player& pl = game_.player();
    int planks = 0, nails = 0;
    for (const auto& it : pl.inventory.items()) {
        if (it.def->hasTag("plank")) ++planks;
        if (it.def->hasTag("nails")) nails += it.usesLeft;
    }
    bool hammer = pl.inventory.findTag("hammer") >= 0 || pl.inventory.findId("hammer") >= 0 || pl.inventory.findId("sledgehammer") >= 0;
    bool saw = pl.inventory.findTag("saw") >= 0;
    bool tarp = pl.inventory.findTag("tarp") >= 0;

    // What you're carrying, as chips across the top
    float cx = p.x + 110 * s;
    auto chip = [&](const std::string& label, bool ok) {
        float w = ui::measure(label, 13 * s).x + 18 * s;
        Rectangle r{cx, p.y + 16 * s, w, 24 * s};
        DrawRectangleRounded(r, 0.5f, 6, ok ? Color{42, 60, 38, 255} : Color{64, 34, 30, 255});
        ui::textCentered(label, r.x + w / 2, r.y + 4 * s, 13 * s, ok ? ui::theme().good : ui::theme().bad);
        cx += w + 6 * s;
    };
    chip(fmt("%d planks", planks), planks > 0);
    chip(fmt("%d nails", nails), nails > 0);
    chip(hammer ? "Hammer \xE2\x9C\x93" : "No hammer", hammer);
    chip(saw ? "Saw \xE2\x9C\x93" : "No saw", saw);
    chip(pl.knowsCarpentry ? "Carpentry \xE2\x9C\x93" : "No carpentry book", pl.knowsCarpentry);

    // Left: groups of cards
    float listW = p.width * 0.6f;
    Rectangle list{p.x + 16 * s, p.y + 56 * s, listW, p.height - 72 * s};
    float y = list.y;
    float card = 104 * s, gap = 10 * s;
    int cols = std::max(1, (int)((list.width + gap) / (card + gap)));
    for (const auto& g : buildGroups()) {
        ui::text(g.name, list.x, y, 15 * s, ui::theme().accent, true);
        y += 24 * s;
        int i = 0;
        for (BuildKind k : g.kinds) {
            const Game::BuildRecipe* r = Game::recipe(k);
            if (!r) continue;
            Rectangle c{list.x + (i % cols) * (card + gap), y + (i / cols) * (card + 24 * s + gap), card, card + 24 * s};
            std::string why;
            bool ok = game_.canAffordBuild(k, &why);
            bool hov = ui::hovered(c);
            bool picked = buildPick_ == (int)k;
            DrawRectangleRounded(c, 0.08f, 4, picked ? Color{70, 58, 38, 255} : hov ? Color{50, 46, 40, 255} : Color{32, 31, 28, 255});
            if (picked) DrawRectangleRoundedLinesEx(c, 0.08f, 4, 2.0f * s, ui::theme().accent);
            drawBuildIcon(k, {c.x + 14 * s, c.y + 6 * s, card - 28 * s, card - 28 * s});
            if (!ok) DrawRectangleRounded({c.x, c.y, c.width, card - 16 * s}, 0.08f, 4, Color{20, 19, 17, 110});
            auto lines = ui::wrap(r->name, card - 10 * s, 13 * s);
            float ly = c.y + card - 18 * s;
            for (size_t li = 0; li < lines.size() && li < 2; ++li, ly += 15 * s)
                ui::textCentered(lines[li], c.x + card / 2, ly, 13 * s, ok ? ui::theme().text : ui::theme().dim, picked);
            DrawCircle((int)(c.x + c.width - 10 * s), (int)(c.y + 10 * s), 4 * s, ok ? ui::theme().good : ui::theme().bad);
            if (hov && input::pressed(MOUSE_BUTTON_LEFT)) {
                double now = GetTime();
                if (buildPick_ == (int)k && now - lastClickTime_ < 0.35 && ok) { buildKind_ = (int)k; overlay_ = Overlay::None; }
                buildPick_ = (int)k;
                lastClickTime_ = now;
            }
            ++i;
        }
        int rows = (i + cols - 1) / cols;
        y += rows * (card + 24 * s + gap) + 6 * s;
    }

    // Right: the picked recipe
    Rectangle d{p.x + listW + 32 * s, p.y + 56 * s, p.width - listW - 48 * s, p.height - 72 * s};
    DrawRectangleRounded(d, 0.04f, 4, Color{28, 27, 24, 255});
    const Game::BuildRecipe* r = Game::recipe((BuildKind)buildPick_);
    if (!r) return;
    std::string why;
    bool ok = game_.canAffordBuild(r->kind, &why);
    drawBuildIcon(r->kind, {d.x + d.width / 2 - 50 * s, d.y + 10 * s, 100 * s, 100 * s});
    ui::textCentered(r->name, d.x + d.width / 2, d.y + 112 * s, 20 * s, ui::theme().text, true);
    float dy = d.y + 140 * s;
    dy = ui::paragraph(r->description, d.x + 14 * s, dy, d.width - 28 * s, 14 * s, ui::theme().dim) + 10 * s;
    // Checklist: have / need
    auto row = [&](const std::string& label, const std::string& value, bool good) {
        ui::text(std::string(good ? "\xE2\x9C\x93  " : "\xE2\x9C\x97  ") + label, d.x + 14 * s, dy, 15 * s, good ? ui::theme().good : ui::theme().bad);
        if (!value.empty()) {
            Vector2 m = ui::measure(value, 15 * s);
            ui::text(value, d.x + d.width - 14 * s - m.x, dy, 15 * s, good ? ui::theme().text : ui::theme().bad);
        }
        dy += 22 * s;
    };
    row("Planks", fmt("%d / %d", planks, r->planks), planks >= r->planks);
    row("Nails", fmt("%d / %d", nails, r->nails), nails >= r->nails);
    row("Hammer", "", hammer);
    if (r->saw) row("Saw", "", saw);
    if (r->tarp) row("Tarpaulin", "", tarp);
    if (r->book) row("Know how (carpentry book)", "", pl.knowsCarpentry);
    dy += 6 * s;
    Rectangle b{d.x + 14 * s, dy, d.width - 28 * s, 34 * s};
    if (ui::button(b, ok ? "Place it" : "Can't build this yet", ok, 16)) { buildKind_ = (int)r->kind; overlay_ = Overlay::None; }
    if (!ok && ui::hovered(b)) ui::tooltip("", why + ".");
    ui::paragraph("Left-click to place, right-click to stop. Walls snap to the tile edge nearest the pointer. "
                  "Planks: hardware stores and garages, or saw a log. Right-click something you built to take it apart.",
                  d.x + 14 * s, d.y + d.height - 66 * s, d.width - 28 * s, 12 * s, ui::theme().dim);
}

} // namespace alone
