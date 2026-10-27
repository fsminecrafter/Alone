#pragma once

namespace alone {

// What a piece of food or drink contains. Values are absolute amounts for
// the portion, not per 100 g.
struct Nutrients {
    float protein = 0;   // g
    float fat = 0;       // g
    float carbs = 0;     // g (digestible)
    float fiber = 0;     // g
    float water = 0;     // ml
    float vitaminC = 0;  // mg

    float kcal() const { return protein * 4.0f + fat * 9.0f + carbs * 4.0f; }
    Nutrients scaled(float f) const {
        return {protein * f, fat * f, carbs * f, fiber * f, water * f, vitaminC * f};
    }
    Nutrients& operator+=(const Nutrients& o) {
        protein += o.protein; fat += o.fat; carbs += o.carbs;
        fiber += o.fiber; water += o.water; vitaminC += o.vitaminC;
        return *this;
    }
    Nutrients& operator-=(const Nutrients& o) {
        protein -= o.protein; fat -= o.fat; carbs -= o.carbs;
        fiber -= o.fiber; water -= o.water; vitaminC -= o.vitaminC;
        return *this;
    }
};

// Germs that can make the player sick. The player is never told which one.
enum class Pathogen {
    None,
    FoodPoisoning,   // Salmonella / Campylobacter / S. aureus: raw or spoiled food
    Giardia,         // untreated lake/river water
    Tularemia,       // "harpest": handling or eating undercooked hare
    Trichinosis,     // undercooked wild boar
    WoundInfection,  // dirty, untreated wounds
    Count
};

// Poisons, mostly from mushrooms.
enum class Toxin {
    None,
    Muscimol,   // fly agaric (flugsvamp)
    Amatoxin,   // death cap / destroying angel (lömsk / vit flugsvamp)
    Count
};

const char* pathogenName(Pathogen p);
const char* toxinName(Toxin t);
Pathogen pathogenFromString(const char* s);
Toxin toxinFromString(const char* s);

} // namespace alone
