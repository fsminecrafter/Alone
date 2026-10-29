// Talking and trading with other people, the journal, names and speech over
// people's heads, and the bike/car dashboard.
#include "client/App.h"
#include "client/Icons.h"
#include "client/Input.h"
#include "client/MeshBuilder.h"
#include "client/Ui.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

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

const char* trustWord(const Survivor& s) {
    if (s.angry) return "Wants you dead";
    if (s.def->disposition == Disposition::Raider) return s.truce ? "Paid off - for now" : "Raider";
    if (s.def->disposition == Disposition::Hostile && !s.admitted) return s.trust > 0 ? "Listening - from a distance" : "Won't let you near";
    if (s.asleep()) return "Asleep";
    if (s.joined) return "Travelling with you";
    return bondName(s.bond());
}

Color bondColour(Bond b) {
    switch (b) {
        case Bond::Enemy: return ui::theme().bad;
        case Bond::Stranger: return ui::theme().dim;
        case Bond::Acquaintance: return ui::theme().text;
        case Bond::Friend: return ui::theme().good;
        case Bond::BestFriend: return Color{120, 200, 170, 255};
        case Bond::Family: return Color{130, 180, 236, 255};
        case Bond::Lover: return Color{232, 120, 150, 255};
        default: return ui::theme().text;
    }
}

// Seven steps from Enemy to Lover, the current one lit, and a bar for
// how far you are towards the next
void drawBondMeter(const Survivor& s, float x, float y, float w, float sc) {
    Bond b = s.bond();
    int n = (int)Bond::Count;
    float pip = (w - (n - 1) * 3 * sc) / n;
    for (int i = 0; i < n; ++i) {
        Rectangle r{x + i * (pip + 3 * sc), y, pip, 6 * sc};
        Color c = i == (int)b ? bondColour(b) : i < (int)b ? Fade(bondColour(b), 0.35f) : Color{50, 50, 52, 255};
        DrawRectangleRounded(r, 0.5f, 3, c);
    }
    if (ui::hovered({x, y - 4 * sc, w, 14 * sc})) {
        std::string body = bondBlurb(b);
        if (b != Bond::Family && b != Bond::Lover && b != Bond::Enemy && s.met) {
            body += "\n\nCloser: talk every day, hear their story, bring gifts, help them.";
            body += "\n\nTwo ways to be closest, and you have to pick one:\n- Family: look out for each other";
            body += s.def->romanceable ? "\n- Lover: flirt with them\nEach one pulls the other down." : "\n(They're not looking for romance.)";
            body += fmt("\n\nFamily %d%%   Romance %d%%   (%d%% and best friends needed)", (int)s.kinship, (int)s.romance, (int)kBondNeeded);
        }
        ui::tooltip(bondName(b), body, bondColour(b));
    }
}

// A small face: skin, hair, and a collar in the colour of their jacket
void drawPortrait(const Survivor& s, Vector2 c, float r) {
    Color skin = hexColor(s.def->skin), hair = hexColor(s.def->hair);
    const ItemInstance& jacket = s.worn[(size_t)ClothingSlot::Jacket];
    const ItemInstance& top = s.worn[(size_t)ClothingSlot::Sweater].def ? s.worn[(size_t)ClothingSlot::Sweater] : s.worn[(size_t)ClothingSlot::Torso];
    Color cloth = jacket.def ? hexColor(jacket.def->colour) : top.def ? hexColor(top.def->colour) : Color{120, 120, 120, 255};
    const ItemInstance& hat = s.worn[(size_t)ClothingSlot::Head];
    DrawCircleV(c, r, Color{30, 32, 34, 255});
    BeginScissorMode((int)(c.x - r), (int)(c.y - r), (int)(2 * r), (int)(2 * r));
    // long hair falls behind the shoulders
    if (s.def->hairStyle == 1 && !hat.def) DrawEllipse((int)c.x, (int)(c.y + r * 0.15f), r * 0.52f, r * 0.62f, hair);
    DrawEllipse((int)c.x, (int)(c.y + r * 1.05f), r * 0.95f, r * 0.6f, cloth);
    DrawEllipse((int)c.x, (int)(c.y - r * 0.08f), r * 0.42f, r * 0.52f, skin);
    if (hat.def) DrawCircleSector({c.x, c.y - r * 0.2f}, r * 0.47f, 180, 360, 16, hexColor(hat.def->colour));
    else if (s.def->hairStyle != 4) DrawCircleSector({c.x, c.y - r * 0.18f}, r * 0.46f, 180, 360, 16, hair);
    if (!hat.def && s.def->hairStyle == 3) DrawCircle((int)c.x, (int)(c.y - r * 0.68f), r * 0.17f, hair);
    DrawCircle((int)(c.x - r * 0.15f), (int)(c.y - r * 0.1f), r * 0.05f, Color{40, 30, 30, 255});
    DrawCircle((int)(c.x + r * 0.15f), (int)(c.y - r * 0.1f), r * 0.05f, Color{40, 30, 30, 255});
    EndScissorMode();
    DrawCircleLinesV(c, r, ui::theme().border);
}

} // namespace

void App::openTalk(int id) {
    Survivor* s = game_.survivor(id);
    if (!s || !game_.canTalk(id)) return;
    talkTo_ = id;
    talkLine_ = game_.greeting(id);
    overlay_ = Overlay::Talk;
    menu_.open = false;
}

void App::drawTalk() {
    Survivor* s = game_.survivor(talkTo_);
    if (!s || !game_.canTalk(talkTo_) || distance(s->pos, game_.player().pos) > game_.talkRange(talkTo_) + 3.0f) { overlay_ = Overlay::None; return; }
    float sc = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    auto topics = game_.talkTopics(talkTo_);
    std::string hint = game_.romanceHint(talkTo_);
    float ph = 168 * sc + topics.size() * 30 * sc + (hint.empty() ? 0 : 24 * sc);
    float pw = std::min(760 * sc, W - 20);
    Rectangle p{W / 2 - pw / 2, H - ph - 20 * sc, pw, ph};
    ui::panel(p, Color{16, 17, 19, 240});
    drawPortrait(*s, {p.x + 56 * sc, p.y + 58 * sc}, 40 * sc);
    float tx = p.x + 112 * sc;
    ui::text(s->def->name, tx, p.y + 12 * sc, 20 * sc, ui::theme().text, true);
    std::string sub = fmt("%d - %s", s->def->age, trustWord(*s));
    ui::text(sub, tx, p.y + 36 * sc, 14 * sc, bondColour(s->bond()));
    drawBondMeter(*s, p.x + p.width - 236 * sc, p.y + 20 * sc, 220 * sc, sc);
    // The two ways to grow closest: family and romance
    if (s->met && s->def->disposition != Disposition::Raider && (s->kinship > 0 || s->romance > 0) && !s->family && !s->lover) {
        float bx = p.x + p.width - 236 * sc, bw = 104 * sc;
        auto bar = [&](float x0, const char* name, float v, Color c) {
            ui::text(name, x0, p.y + 32 * sc, 12 * sc, ui::theme().dim);
            Rectangle r{x0, p.y + 48 * sc, bw, 4 * sc};
            DrawRectangleRec(r, Color{50, 50, 52, 255});
            DrawRectangleRec({r.x, r.y, r.width * std::min(1.0f, v / kBondNeeded), r.height}, c);
        };
        bar(bx, "Family", s->kinship, Color{130, 180, 236, 255});
        if (s->def->romanceable) bar(bx + bw + 12 * sc, "Romance", s->romance, Color{232, 120, 150, 255});
    }
    // what they said
    Rectangle say{tx, p.y + 60 * sc, p.width - (tx - p.x) - 16 * sc, 88 * sc};
    DrawRectangleRounded(say, 0.1f, 4, Color{30, 31, 34, 255});
    ui::paragraph("\xE2\x80\x9C" + talkLine_ + "\xE2\x80\x9D", say.x + 10 * sc, say.y + 8 * sc, say.width - 20 * sc, 15 * sc, ui::theme().text);
    // what you can say
    float y = p.y + 158 * sc;
    for (const auto& [topic, label] : topics) {
        Rectangle r{tx, y, say.width, 26 * sc};
        bool hov = ui::hovered(r);
        if (hov) DrawRectangleRec(r, Color{60, 52, 38, 220});
        ui::text("\xE2\x96\xB8 " + label, r.x + 8 * sc, r.y + 4 * sc, 15 * sc, hov ? ui::theme().accent : ui::theme().text);
        if (hov && input::pressed(MOUSE_BUTTON_LEFT)) {
            if (topic == Topic::Trade) {
                std::string answer = game_.talk(talkTo_, topic);
                if (answer.rfind("Let's", 0) == 0) {
                    tradeMine_.clear();
                    tradeTheirs_.clear();
                    overlay_ = Overlay::Trade;
                } else {
                    talkLine_ = answer;
                }
            } else if (topic == Topic::Bye) {
                game_.talk(talkTo_, topic);
                overlay_ = Overlay::None;
            } else {
                talkLine_ = game_.talk(talkTo_, topic);
            }
            break;
        }
        y += 30 * sc;
    }
    if (!hint.empty())
        ui::text(hint, tx + 8 * sc, p.y + 158 * sc + topics.size() * 30 * sc + 2 * sc, 13 * sc, ui::theme().dim);
    ui::claim(p);
}

void App::drawTrade() {
    Survivor* s = game_.survivor(talkTo_);
    if (!s || !game_.canTalk(talkTo_) || distance(s->pos, game_.player().pos) > game_.talkRange(talkTo_) + 3.0f) { overlay_ = Overlay::None; return; }
    float sc = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    float pw = std::min(1100 * sc, W - 16), ph = std::min(600 * sc, H - 16);
    Rectangle p{W / 2 - pw / 2, H / 2 - ph / 2, pw, ph};
    ui::panel(p, Color{16, 17, 19, 242});
    ui::claim(p);
    ui::text("Trade with " + s->def->name, p.x + 16 * sc, p.y + 12 * sc, 22 * sc, ui::theme().text, true);
    ui::text("Click things to put them on the table.", p.x + 16 * sc, p.y + 40 * sc, 14 * sc, ui::theme().dim);

    float gw = (p.width - 48 * sc) / 2, gh = p.height - 170 * sc;
    Rectangle mineR{p.x + 16 * sc, p.y + 90 * sc, gw, gh}, theirsR{p.x + 32 * sc + gw, p.y + 90 * sc, gw, gh};
    auto grid = [&](Rectangle area, std::vector<ItemInstance>& items, std::vector<uint32_t>& picked, int pane, uint32_t locked, const char* title) {
        ui::text(title, area.x, area.y - 26 * sc, 17 * sc, ui::theme().accent, true);
        DrawRectangleRounded(area, 0.02f, 4, Color{8, 9, 10, 170});
        float tile = 56 * sc, gap = 6 * sc;
        int cols = std::max(1, (int)((area.width - gap) / (tile + gap)));
        int rows = ((int)items.size() + cols - 1) / cols;
        if (ui::hovered(area)) tradeScroll_[pane] -= GetMouseWheelMove() * (tile + gap);
        tradeScroll_[pane] = std::clamp(tradeScroll_[pane], 0.0f, std::max(0.0f, rows * (tile + gap) + gap - area.height));
        BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);
        for (size_t i = 0; i < items.size(); ++i) {
            ItemInstance& it = items[i];
            Rectangle r{area.x + gap + (i % cols) * (tile + gap), area.y + gap + (i / cols) * (tile + gap) - tradeScroll_[pane], tile, tile};
            bool on = std::find(picked.begin(), picked.end(), it.uid) != picked.end();
            bool hov = ui::hovered(r) && ui::hovered(area);
            drawItemTile(r, it, 1, on, hov);
            if (on) DrawRectangleRounded(r, 0.12f, 4, Color{214, 168, 92, 50});
            if (it.uid == locked) DrawRectangleRounded(r, 0.12f, 4, Color{0, 0, 0, 120});
            if (!hov) continue;
            ui::tooltip(game_.itemName(it), fmt("Worth about %d to them.%s", game_.itemValue(it), it.uid == locked ? "\nThey won't part with it." : ""));
            if (input::pressed(MOUSE_BUTTON_LEFT)) {
                if (on) picked.erase(std::find(picked.begin(), picked.end(), it.uid));
                else picked.push_back(it.uid);
            }
        }
        EndScissorMode();
        ui::claim(area);
    };
    grid(mineR, game_.player().inventory.items(), tradeMine_, 0, 0, "You give");
    grid(theirsR, s->inventory.items(), tradeTheirs_, 1, s->joined ? 0 : s->weaponUid, (std::string("They give") + (s->joined ? " (shared)" : "")).c_str());

    int give = 0, take = 0;
    for (uint32_t u : tradeMine_) { int i = game_.player().inventory.indexOfUid(u); if (i >= 0) give += game_.itemValue(game_.player().inventory.items()[i]); }
    for (uint32_t u : tradeTheirs_) { int i = s->inventory.indexOfUid(u); if (i >= 0) take += game_.itemValue(s->inventory.items()[i]); }
    std::string why;
    bool fair = game_.tradeFair(talkTo_, tradeMine_, tradeTheirs_, &why);
    float by = p.y + p.height - 68 * sc;
    ui::text(fmt("You give: %d", give), p.x + 16 * sc, by, 16 * sc, ui::theme().text, true);
    ui::text(fmt("You get: %d", take), p.x + 180 * sc, by, 16 * sc, ui::theme().text, true);
    if (!tradeMine_.empty() && tradeTheirs_.empty()) ui::text("A gift. They'll remember it.", p.x + 340 * sc, by, 15 * sc, ui::theme().good);
    else if (!fair && !why.empty()) ui::text(why, p.x + 340 * sc, by, 15 * sc, ui::theme().bad);
    else if (fair) ui::text("\"Deal.\"", p.x + 340 * sc, by, 15 * sc, ui::theme().good);
    if (ui::button({p.x + p.width - 260 * sc, p.y + p.height - 48 * sc, 120 * sc, 34 * sc}, "Back", true, 16)) overlay_ = Overlay::Talk;
    if (ui::button({p.x + p.width - 132 * sc, p.y + p.height - 48 * sc, 116 * sc, 34 * sc}, "Deal", fair, 16)) {
        bool gift = tradeTheirs_.empty();
        if (game_.trade(talkTo_, tradeMine_, tradeTheirs_)) {
            talkLine_ = gift ? "For me? ...Thank you." : "Pleasure doing business.";
            tradeMine_.clear();
            tradeTheirs_.clear();
            overlay_ = Overlay::Talk;
        }
    }
}

void App::drawJournal() {
    float sc = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    float pw = std::min(900 * sc, W - 16), ph = std::min(620 * sc, H - 16);
    Rectangle p{W / 2 - pw / 2, H / 2 - ph / 2, pw, ph};
    ui::panel(p, Color{24, 22, 18, 245});
    ui::claim(p);
    ui::text("Journal", p.x + 20 * sc, p.y + 14 * sc, 24 * sc, Color{230, 214, 180, 255}, true);
    if (ui::button({p.x + p.width - 44 * sc, p.y + 10 * sc, 32 * sc, 28 * sc}, "X")) { overlay_ = Overlay::None; return; }
    ui::text("\xE2\x98\x85 " + game_.objective(), p.x + 20 * sc, p.y + 50 * sc, 17 * sc, ui::theme().accent, true);

    float y = p.y + 86 * sc;
    BeginScissorMode((int)p.x, (int)y, (int)p.width, (int)(p.height - (y - p.y) - 50 * sc));
    int known = 0;
    for (const auto& sv : game_.survivors()) {
        if (!sv.known && !sv.met) continue;
        ++known;
        Rectangle row{p.x + 16 * sc, y, p.width - 32 * sc, 92 * sc};
        DrawRectangleRounded(row, 0.06f, 4, Color{36, 33, 28, 255});
        if (sv.met) drawPortrait(sv, {row.x + 42 * sc, row.y + 46 * sc}, 30 * sc);
        else {
            DrawCircleV({row.x + 42 * sc, row.y + 46 * sc}, 30 * sc, Color{30, 32, 34, 255});
            ui::textCentered("?", row.x + 42 * sc, row.y + 30 * sc, 28 * sc, ui::theme().dim, true);
        }
        float tx = row.x + 84 * sc;
        bool bad = sv.def->disposition == Disposition::Hostile || sv.def->disposition == Disposition::Raider || sv.angry;
        Color nameC = !sv.alive() ? ui::theme().dim : bad ? ui::theme().bad : sv.joined ? Color{120, 180, 230, 255} : ui::theme().text;
        ui::text(sv.def->name + (sv.alive() ? "" : "  \xE2\x80\x93 dead"), tx, row.y + 8 * sc, 17 * sc, nameC, true);
        std::string where = sv.joined ? "With you" : sv.def->disposition == Disposition::Raider ? "Moves around" : sv.def->homeName;
        std::string status = !sv.met ? (bad ? "Dangerous. Stay away." : "Haven't met them yet.") : trustWord(sv);
        ui::text(where + "  \xC2\xB7  " + status, tx, row.y + 32 * sc, 14 * sc, sv.met && !bad ? bondColour(sv.bond()) : ui::theme().dim);
        if (sv.met && sv.alive() && sv.def->disposition != Disposition::Raider) drawBondMeter(sv, row.x + row.width - 190 * sc, row.y + 14 * sc, 176 * sc, sc);
        std::string note;
        if (sv.met && sv.requestKnown && !sv.requestDone) {
            const ItemDef* d = sv.def->wantItem.rfind("tag:", 0) == 0 ? nullptr : game_.items().find(sv.def->wantItem);
            std::string what = d ? d->name : sv.def->wantItem.substr(4);
            note = fmt("Needs: %s%s", sv.def->wantCount > 1 ? fmt("%d x ", sv.def->wantCount).c_str() : "", what.c_str());
        } else if (sv.requestDone) {
            note = "You helped them.";
        } else if (sv.met) {
            note = sv.def->bio;
        } else {
            note = sv.def->describe.empty() ? (bad ? "Others warned you about them." : "Someone told you about them.") : sv.def->describe;
        }
        ui::paragraph(note, tx, row.y + 54 * sc, row.width - (tx - row.x) - 12 * sc, 14 * sc,
                      sv.requestKnown && !sv.requestDone ? ui::theme().accent : ui::theme().text);
        y += 100 * sc;
    }
    EndScissorMode();
    if (known == 0) ui::paragraph("You haven't met anyone. Last night there was a light in a window a few streets away...", p.x + 20 * sc, y, p.width - 40 * sc, 15 * sc, ui::theme().dim);
    // What you've learned
    const Player& pl = game_.player();
    std::string skills;
    if (pl.knowsCarpentry) skills += "Carpentry. ";
    if (pl.knowsHotwire) skills += "Hotwiring cars. ";
    if (game_.knowsMushrooms()) skills += "Mushrooms. ";
    ui::text("You know: " + (skills.empty() ? std::string("how to survive, barely.") : skills), p.x + 20 * sc, p.y + p.height - 34 * sc, 14 * sc, ui::theme().dim);
}

void App::drawSurvivorLabels() {
    if (state_ != State::Playing) return;
    float sc = ui::scale();
    Vec2 pp = game_.player().pos;
    for (const auto& sv : game_.survivors()) {
        if (!sv.alive() || distance(sv.pos, pp) > 45) continue;
        if (sv.joined && game_.player().vehicle >= 0 && distance(sv.pos, pp) < 1) continue;   // riding along
        Vector2 sp = renderer_.toScreen(sv.pos, 2.15f);
        if (sp.x < -100 || sp.y < -50 || sp.x > GetScreenWidth() + 100 || sp.y > GetScreenHeight() + 50) continue;
        bool hov = distance(hover_, sv.pos) < 0.9f;
        if (sv.speechTimer > 0 && !sv.speech.empty()) {
            float fade = std::min(1.0f, sv.speechTimer);
            auto lines = ui::wrap(sv.speech, 260 * sc, 14 * sc);
            float w = 0;
            for (auto& l : lines) w = std::max(w, ui::measure(l, 14 * sc).x);
            float h = lines.size() * 17 * sc + 10 * sc;
            Rectangle b{sp.x - w / 2 - 8 * sc, sp.y - h - 14 * sc, w + 16 * sc, h};
            bool bad = sv.hostileTo() || sv.def->disposition == Disposition::Hostile;
            DrawRectangleRounded(b, 0.25f, 6, Fade(bad ? Color{70, 22, 18, 255} : Color{238, 234, 222, 255}, 0.92f * fade));
            DrawTriangle({sp.x - 6 * sc, b.y + b.height}, {sp.x, b.y + b.height + 8 * sc}, {sp.x + 6 * sc, b.y + b.height},
                         Fade(bad ? Color{70, 22, 18, 255} : Color{238, 234, 222, 255}, 0.92f * fade));
            float ly = b.y + 5 * sc;
            for (auto& l : lines) {
                ui::textCentered(l, sp.x, ly, 14 * sc, Fade(bad ? Color{250, 220, 210, 255} : Color{30, 30, 30, 255}, fade));
                ly += 17 * sc;
            }
        } else if (hov || sv.joined) {
            std::string name = sv.met ? sv.def->name : "Stranger";
            Color c = sv.hostileTo() ? ui::theme().bad : sv.joined ? Color{140, 196, 236, 255} : sv.met ? bondColour(sv.bond()) : ui::theme().text;
            if (sv.met && sv.bond() == Bond::Lover) name = "\xE2\x99\xA5 " + name;
            Vector2 m = ui::measure(name, 13 * sc, true);
            DrawRectangleRounded({sp.x - m.x / 2 - 5 * sc, sp.y - 20 * sc, m.x + 10 * sc, 18 * sc}, 0.4f, 4, Color{10, 12, 14, 160});
            ui::textCentered(name, sp.x, sp.y - 18 * sc, 13 * sc, c, true);
        }
    }
}

void App::drawVehicleHud() {
    if (state_ != State::Playing) return;
    const Vehicle* v = game_.playerVehicle();
    if (!v) return;
    float sc = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    bool car = v->kind == VehicleKind::Car;
    Rectangle r{W - 250 * sc, H - 158 * sc, 238 * sc, 62 * sc};
    ui::panel(r, Color{10, 12, 14, 190});
    float kmh = std::fabs(v->speed) * 3.6f;
    ui::text(fmt("%.0f", kmh), r.x + 12 * sc, r.y + 6 * sc, 30 * sc, ui::theme().text, true);
    ui::text("km/h", r.x + 12 * sc + ui::measure(fmt("%.0f", kmh), 30 * sc, true).x + 6 * sc, r.y + 20 * sc, 13 * sc, ui::theme().dim);
    if (car) {
        std::string state = v->damage >= 1 ? "Wrecked" : v->running ? "Engine running" : v->canStart() ? "Engine off" : "No key";
        ui::text(state, r.x + 120 * sc, r.y + 8 * sc, 13 * sc, v->running ? ui::theme().good : ui::theme().dim);
        // fuel gauge
        float f = std::min(1.0f, v->fuelL / std::max(1.0f, v->tankL));
        Rectangle g{r.x + 120 * sc, r.y + 30 * sc, 100 * sc, 6 * sc};
        DrawRectangleRec(g, Color{40, 40, 40, 255});
        DrawRectangleRec({g.x, g.y, g.width * f, g.height}, f < 0.1f ? ui::theme().bad : ui::theme().accent);
        ui::text(fmt("Fuel %.1f / %.0f l", v->fuelL, v->tankL), r.x + 120 * sc, r.y + 40 * sc, 12 * sc, ui::theme().dim);
        if (v->damage > 0.05f && v->damage < 1) ui::text(fmt("Damage %d%%", (int)(v->damage * 100)), r.x + 12 * sc, r.y + 42 * sc, 12 * sc, ui::theme().bad);
    } else {
        ui::text("Bicycle", r.x + 120 * sc, r.y + 8 * sc, 14 * sc, ui::theme().text, true);
        ui::text("Quiet, needs no fuel", r.x + 120 * sc, r.y + 30 * sc, 12 * sc, ui::theme().dim);
    }
    ui::text(car ? "W gas  S brake/reverse  A/D steer  F: key / get out" : "W pedal  S brake  A/D steer  F: get off", r.x + 12 * sc, r.y + r.height + 4 * sc, 12 * sc, ui::theme().dim);
}

} // namespace alone
