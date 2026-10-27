// The physiology model.
//
// Rough sources for the numbers used here (all simplified for a game):
//  - BMR: Katch-McArdle, 370 + 21.6 * lean mass (kg) kcal/day.
//  - Gastric emptying of roughly 2-4 kcal/min, slower for fatty meals.
//  - The liver's urea cycle can only get rid of the nitrogen from about
//    2.5-3.5 g protein per kg body weight per day. Beyond that, amino acids
//    and ammonia build up: nausea, diarrhoea, weakness and eventually death.
//    This is "rabbit starvation" / protein poisoning, seen in people living on
//    lean game (hare, lean venison) without fat or carbohydrate.
//  - Fat can only be released from adipose tissue at a limited rate per kg of
//    fat (Alpert, 2005, ~70 kcal/kg/day; we use a more forgiving 190). Lean
//    people therefore burn muscle sooner when food runs out.
//  - Brain glucose need of ~110 g/day, dropping as the body adapts to ketosis.
//  - Vitamin C body pool of ~1500 mg, scurvy below ~300 mg (1-3 months).
//  - Death from dehydration at roughly 12% body weight of water lost; from
//    blood loss at roughly 40% blood volume lost; from starvation at ~45% lean
//    tissue lost.
//  - Heat balance with clothing insulation in clo (1 clo = 0.155 m2K/W).
#include "survival/Body.h"
#include "core/Math.h"
#include <algorithm>
#include <cmath>

namespace alone {

namespace {
constexpr float kWattsPerKcalMin = 69.78f;
constexpr float kMaxLiverGlycogen = 100.0f;
constexpr float kMaxMuscleGlycogen = 400.0f;
constexpr float kEssentialFatKg = 2.0f;
constexpr float kLeanKcalPerKg = 1000.0f;       // energy released by breaking down 1 kg lean tissue
constexpr float kLeanProteinFraction = 0.2f;     // lean tissue is ~20% protein
constexpr float kAdiposeKcalPerKgPerDay = 190.0f;
constexpr float kMinutesPerDay = 1440.0f;

float activityFactor(Activity a) {
    switch (a) {
        case Activity::Sleeping: return 0.95f;
        case Activity::Resting:  return 1.15f;
        case Activity::Standing: return 1.35f;
        case Activity::Walking:  return 3.0f;
        case Activity::Running:  return 7.5f;
        case Activity::Working:  return 2.2f;
        case Activity::Fighting: return 6.0f;
    }
    return 1.0f;
}

float exertionOf(Activity a) {
    switch (a) {
        case Activity::Running:  return 1.0f;
        case Activity::Fighting: return 0.9f;
        case Activity::Walking:  return 0.35f;
        case Activity::Working:  return 0.3f;
        default: return 0.0f;
    }
}
} // namespace

const char* pathogenName(Pathogen p) {
    switch (p) {
        case Pathogen::FoodPoisoning:  return "food_poisoning";
        case Pathogen::Giardia:        return "giardia";
        case Pathogen::Tularemia:      return "tularemia";
        case Pathogen::Trichinosis:    return "trichinosis";
        case Pathogen::WoundInfection: return "wound_infection";
        default: return "none";
    }
}
const char* toxinName(Toxin t) {
    switch (t) {
        case Toxin::Muscimol: return "muscimol";
        case Toxin::Amatoxin: return "amatoxin";
        default: return "none";
    }
}
Pathogen pathogenFromString(const char* s) {
    for (int i = 0; i < (int)Pathogen::Count; ++i)
        if (std::string(pathogenName((Pathogen)i)) == s) return (Pathogen)i;
    return Pathogen::None;
}
Toxin toxinFromString(const char* s) {
    for (int i = 0; i < (int)Toxin::Count; ++i)
        if (std::string(toxinName((Toxin)i)) == s) return (Toxin)i;
    return Toxin::None;
}

const char* bodyPartName(BodyPart p) {
    static const char* names[] = {"head", "torso", "left arm", "right arm", "left hand",
                                  "right hand", "left leg", "right leg", "left foot", "right foot"};
    return names[(int)p];
}
const char* woundTypeName(WoundType t) {
    static const char* names[] = {"scratch", "laceration", "bite", "puncture", "burn"};
    return names[(int)t];
}

const char* deathCauseName(DeathCause c) {
    switch (c) {
        case DeathCause::Dehydration:      return "Dehydration";
        case DeathCause::Starvation:       return "Starvation";
        case DeathCause::ProteinPoisoning: return "Protein poisoning (\"rabbit starvation\")";
        case DeathCause::Hypothermia:      return "Hypothermia";
        case DeathCause::Hyperthermia:     return "Heatstroke";
        case DeathCause::BloodLoss:        return "Blood loss";
        case DeathCause::Sepsis:           return "Sepsis from an infected wound";
        case DeathCause::LiverFailure:     return "Liver failure";
        case DeathCause::Scurvy:           return "Scurvy";
        case DeathCause::Infection:        return "Infectious disease";
        case DeathCause::Poisoning:        return "Poisoning";
        default: return "Unknown";
    }
}

const char* deathCauseExplanation(DeathCause c) {
    switch (c) {
        case DeathCause::Dehydration:
            return "Your body ran out of water. Diarrhoea, vomiting, sweating and a high-protein diet all "
                   "increase how much you need to drink.";
        case DeathCause::Starvation:
            return "Without enough energy your body consumed its fat and then its own muscle and organs.";
        case DeathCause::ProteinPoisoning:
            return "You lived on lean protein. The liver can only process so much protein a day; without "
                   "enough fat or carbohydrate, ammonia built up in your blood. Lean game like hare "
                   "cannot sustain you on its own.";
        case DeathCause::Hypothermia:
            return "Your core temperature fell too low. Wet clothes, wind and resting in the cold are deadly.";
        case DeathCause::Hyperthermia:
            return "Your core temperature rose too high.";
        case DeathCause::BloodLoss:
            return "You lost too much blood. Bandaging wounds quickly stops most bleeding.";
        case DeathCause::Sepsis:
            return "An untreated wound became infected and the infection spread. Clean and disinfect wounds.";
        case DeathCause::LiverFailure:
            return "Something you ate destroyed your liver. Some deadly mushrooms look a lot like edible ones, "
                   "and the symptoms can seem to pass before the real damage shows.";
        case DeathCause::Scurvy:
            return "Months without vitamin C. Berries, fresh liver, potatoes and vegetables prevent scurvy.";
        case DeathCause::Infection:
            return "An infection overwhelmed your weakened body.";
        case DeathCause::Poisoning:
            return "You ate something poisonous.";
        default: return "";
    }
}

Body::Body(uint64_t seed) : rng_(seed) {}

float Body::bodyMassKg() const {
    // glycogen is stored with ~3 g water per g
    float glyKg = (s_.liverGlycogenG + s_.muscleGlycogenG) * 4.0f / 1000.0f;
    return s_.leanMassKg + s_.fatMassKg + glyKg - std::max(0.0f, s_.waterDeficitL) * 0.5f;
}

float Body::bmrKcalPerDay() const {
    float bmr = 370.0f + 21.6f * s_.leanMassKg;
    bmr *= 1.0f - 0.2f * s_.starvationAdaptation;
    bmr *= 1.0f + 0.12f * std::max(0.0f, s_.feverC);
    if (s_.asleep) bmr *= 0.92f;
    return bmr;
}

float Body::fullness() const {
    // ~1 litre of food and drink feels comfortably full
    return (s_.stomachSolidMl + s_.stomach.water) / 1000.0f;
}

float Body::hunger() const {
    float fuel = s_.stomach.kcal() + s_.bloodSugarG * 4.0f + s_.dietaryFatG * 9.0f + s_.aminoPoolG * 1.2f;
    float h = 0.5f * (1.0f - clamp01(fuel / 600.0f)) + 0.8f * (1.0f - s_.liverGlycogenG / kMaxLiverGlycogen) - 0.12f;
    // A full stomach mutes hunger, whatever is in it
    h -= 0.5f * ramp(fullness(), 0.4f, 1.0f);
    // "Fat hunger": on a lean-meat diet you stay hungry however much you eat.
    float totalKcal = std::max(300.0f, s_.avgKcalProtein + s_.avgKcalFat + s_.avgKcalCarbs);
    float leanDiet = ramp(s_.avgKcalProtein / totalKcal, 0.3f, 0.55f) * (1.0f - ramp(s_.avgKcalFat / totalKcal, 0.1f, 0.25f));
    h += 0.35f * leanDiet;
    return clamp01(h);
}

float Body::thirst() const {
    return ramp(s_.waterDeficitL, 0.3f, 5.0f);
}

float Body::totalBleeding() const {
    float b = 0;
    for (const auto& w : s_.wounds) b += w.bandaged && w.bandageDirt < 0.8f ? w.bleeding * 0.1f : w.bleeding;
    return b;
}

float Body::pain() const {
    float p = 0;
    for (const auto& w : s_.wounds) p += w.severity * (1.0f - w.healed) * 0.7f + w.infection * 0.4f;
    for (const auto& inf : s_.infections)
        if (inf.type == Pathogen::Trichinosis && inf.ageH > 48) p += inf.load * 0.5f;
    p += s_.diarrhea * 0.3f;
    return clamp01(p - s_.painkiller * 0.6f);
}

float Body::immuneStrength() const {
    float lean = s_.leanMassKg / s_.leanBaselineKg;
    float f = 1.0f;
    f *= lerp(0.4f, 1.0f, ramp(lean, 0.7f, 0.95f));
    f *= lerp(0.5f, 1.0f, ramp(s_.vitaminCMg, 100.0f, 500.0f));
    f *= lerp(0.6f, 1.0f, 1.0f - ramp(s_.sleepPressure, 0.6f, 1.0f));
    f *= lerp(0.6f, 1.0f, ramp(s_.coreTempC, 34.5f, 36.5f));
    f *= 1.0f - 0.4f * ramp(dehydrationFraction(), 0.03f, 0.08f);
    return f;
}

std::vector<BodyEvent> Body::takeEvents() {
    std::vector<BodyEvent> e;
    e.swap(events_);
    return e;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void Body::ingest(const Nutrients& n, float solidMl) {
    s_.stomach += n;
    s_.stomachSolidMl += solidMl;
    // Forcing food down while nauseous makes it worse
    BodySignals sig = signals();
    if (sig.nausea > 0.4f && n.kcal() > 50) s_.nauseaExtra = std::min(1.0f, s_.nauseaExtra + 0.15f);
}

void Body::expose(Pathogen p, float dose) {
    if (p == Pathogen::None || dose <= 0) return;
    for (auto& inf : s_.infections) {
        if (inf.type == p) {               // reinfection: worse, but no new incubation
            inf.peak = std::min(1.0f, inf.peak + dose * 0.3f);
            return;
        }
    }
    Infection inf;
    inf.type = p;
    inf.peak = std::clamp(dose * rng_.range(0.6f, 1.2f), 0.1f, 1.0f);
    switch (p) {
        case Pathogen::FoodPoisoning: inf.incubationH = rng_.range(4.0f, 48.0f); break;
        case Pathogen::Giardia:       inf.incubationH = rng_.range(72.0f, 240.0f); break;
        case Pathogen::Tularemia:     inf.incubationH = rng_.range(48.0f, 140.0f); break;
        case Pathogen::Trichinosis:   inf.incubationH = rng_.range(24.0f, 72.0f); break;
        default:                      inf.incubationH = rng_.range(12.0f, 48.0f); break;
    }
    s_.infections.push_back(inf);
}

void Body::ingestToxin(Toxin t, float amount) {
    if (t == Toxin::None || amount <= 0) return;
    s_.toxins.push_back({t, amount, 0});
}

void Body::addWound(const Wound& w) {
    s_.wounds.push_back(w);
}

void Body::vomit() {
    // Loses what's in the stomach plus gastric fluid
    s_.waterDeficitL += 0.25f + s_.stomach.water / 1000.0f;
    s_.stomach = Nutrients{};
    s_.stomachSolidMl = 0;
    s_.nauseaExtra = std::max(0.0f, s_.nauseaExtra - 0.5f);
    vomitCooldown_ = 25.0f;
    emit(BodyEvent::Vomited);
}

void Body::bandageWound(int index, bool sterile) {
    if (index < 0 || index >= (int)s_.wounds.size()) return;
    Wound& w = s_.wounds[index];
    w.bandaged = true;
    w.bandageDirt = sterile ? 0.0f : 0.4f;
    if (!sterile) w.contamination = std::min(1.0f, w.contamination + 0.1f);
}

void Body::disinfectWound(int index) {
    if (index < 0 || index >= (int)s_.wounds.size()) return;
    Wound& w = s_.wounds[index];
    w.disinfected = true;
    w.contamination *= 0.1f;
    w.infection = std::max(0.0f, w.infection - 0.15f);
}

void Body::takePainkiller(float strength) {
    s_.painkiller = std::min(1.0f, s_.painkiller + strength);
}

void Body::takeAntibiotic(Pathogen against) {
    for (auto& inf : s_.infections)
        if (inf.type == against) inf.treated = true;
    if (against == Pathogen::WoundInfection) s_.woundAntibioticH = 72.0f;
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void Body::update(float minutes, const Environment& env, Activity activity) {
    while (minutes > 0 && !dead()) {
        float dt = std::min(1.0f, minutes);
        stepMinute(dt, env, activity);
        minutes -= dt;
    }
}

void Body::stepMinute(float dt, const Environment& env, Activity activity) {
    s_.minutesAlive += dt;
    if (s_.asleep) activity = Activity::Sleeping;

    float bmrPerMin = bmrKcalPerDay() / kMinutesPerDay;
    float factor = activityFactor(activity);
    float exertion = exertionOf(activity);
    if (exertion > 0) factor *= 1.0f + 0.8f * env.carriedKg / std::max(40.0f, bodyMassKg());
    float metabolic = bmrPerMin * factor;

    float shiverKcal = thermoregulate(dt, env, metabolic);
    float expenditure = metabolic + shiverKcal;
    s_.lastExpenditureKcalPerMin = expenditure;

    float before = s_.stomach.kcal();
    digest(dt);
    float digested = before - s_.stomach.kcal();
    s_.energyDebtKcal = std::max(0.0f, s_.energyDebtKcal + expenditure * dt - digested);

    processProtein(dt);
    metabolise(dt, expenditure * dt, std::min(1.0f, exertion + s_.shivering * 0.5f));

    float sweatMl = s_.sweating * 7.5f * dt;   // up to ~450 ml/h
    hydrate(dt, sweatMl);
    updateInfections(dt);
    updateToxins(dt);
    updateWounds(dt);
    updateRest(dt);

    // Nausea can make you throw up
    vomitCooldown_ = std::max(0.0f, vomitCooldown_ - dt);
    s_.nauseaExtra = std::max(0.0f, s_.nauseaExtra - dt / 240.0f);
    BodySignals sig = signals();
    if (vomitCooldown_ <= 0 && sig.nausea > 0.65f && rng_.chance((sig.nausea - 0.6f) * 0.025f * dt))
        vomit();
    // Loose bowels
    if (s_.diarrhea > 0.3f) {
        diarrheaTimer_ -= dt * s_.diarrhea;
        if (diarrheaTimer_ <= 0) {
            diarrheaTimer_ = rng_.range(40.0f, 120.0f);
            s_.waterDeficitL += 0.1f * s_.diarrhea;
            emit(BodyEvent::Diarrhea);
        }
    }

    s_.painkiller = std::max(0.0f, s_.painkiller - dt / 300.0f);
    s_.woundAntibioticH = std::max(0.0f, s_.woundAntibioticH - dt / 60.0f);

    updateDamage(dt);
    checkDeath();
}

void Body::digest(float dt) {
    Nutrients& st = s_.stomach;
    float kcal = st.kcal();
    float moveFrac = 1.0f;
    if (kcal > 1.0f) {
        float fatShare = st.fat * 9.0f / kcal;
        float rate = 3.2f * (1.0f - 0.35f * fatShare);  // kcal/min leaving the stomach
        moveFrac = std::min(1.0f, rate * dt / kcal);
    }
    float waterMoved = std::min(st.water, (20.0f + 0.02f * st.water) * dt);

    Nutrients moved = st.scaled(moveFrac);
    moved.water = waterMoved;
    st -= moved;
    st.water = std::max(0.0f, st.water);
    s_.stomachSolidMl *= (1.0f - moveFrac);
    if (st.kcal() < 0.5f) { st.protein = st.fat = st.carbs = st.fiber = st.vitaminC = 0; s_.stomachSolidMl = 0; }

    // Absorption in the gut. Diarrhoea rushes food through before it's absorbed.
    float eff = 1.0f - 0.7f * s_.diarrhea;
    float carbs = moved.carbs * eff;
    float fat = moved.fat * eff * 0.95f;
    float protein = moved.protein * eff * 0.9f;
    s_.bloodSugarG += carbs;
    s_.dietaryFatG += fat;
    s_.aminoPoolG += protein;
    s_.waterDeficitL -= moved.water * (1.0f - 0.3f * s_.diarrhea) / 1000.0f;  // water still gets through
    if (s_.vitaminCMg < 2000.0f) s_.vitaminCMg += moved.vitaminC * eff * 0.8f;

    // 3-day rolling intake, for cravings
    float k = dt / (3.0f * kMinutesPerDay);
    auto roll = [&](float& avg, float kcalNow) { avg += (kcalNow / dt * kMinutesPerDay - avg) * k; };
    roll(s_.avgKcalProtein, protein * 4.0f);
    roll(s_.avgKcalFat, fat * 9.0f);
    roll(s_.avgKcalCarbs, carbs * 4.0f);
}

void Body::processProtein(float dt) {
    // Daily protein losses that must be replaced (skin, gut lining, enzymes)
    float obligatory = 25.0f / kMinutesPerDay * dt;
    float fromPool = std::min(s_.aminoPoolG, obligatory);
    s_.aminoPoolG -= fromPool;
    s_.leanMassKg -= (obligatory - fromPool) / kLeanProteinFraction / 1000.0f;

    // Rebuild lost muscle when protein is available
    if (s_.leanMassKg < s_.leanBaselineKg && s_.aminoPoolG > 5.0f && s_.energyDebtKcal < 4000.0f) {
        float r = std::min(s_.aminoPoolG - 5.0f, 40.0f / kMinutesPerDay * dt);
        s_.aminoPoolG -= r;
        s_.leanMassKg += r / kLeanProteinFraction / 1000.0f;
    }

    // The urea cycle: deaminate amino acids, limited by liver capacity
    float capPerMin = 3.0f * bodyMassKg() / kMinutesPerDay * (1.0f - 0.6f * s_.liverDamage);
    float d = std::min(std::max(0.0f, s_.aminoPoolG - 10.0f), capPerMin * dt);
    s_.aminoPoolG -= d;
    s_.bloodSugarG += d * 0.56f;                 // gluconeogenesis
    s_.energyCreditKcal += d * (4.0f - 0.56f * 4.0f);
    s_.urineExtraMl += d * 7.0f;                 // urea must be flushed out with water

    // Protein the liver can't keep up with: ammonia builds up
    if (s_.aminoPoolG > 60.0f) {
        float ex = (s_.aminoPoolG - 60.0f) * 0.003f * dt;
        s_.aminoPoolG -= ex;
        s_.ammonia += ex * 0.0018f;
    } else {
        s_.ammonia = std::max(0.0f, s_.ammonia - 0.15f / kMinutesPerDay * dt * (1.0f - 0.7f * s_.liverDamage));
    }
}

void Body::metabolise(float dt, float need, float exertion) {
    float keto = s_.ketoAdaptation;

    float use = std::min(s_.energyCreditKcal, need);
    s_.energyCreditKcal -= use;
    need -= use;

    float carbFrac = std::clamp(0.35f + 0.45f * exertion, 0.0f, 0.9f) * (1.0f - 0.6f * keto);
    float carbNeed = need * carbFrac / 4.0f;
    float fatNeed = need * (1.0f - carbFrac) / 9.0f;

    auto take = [](float& pool, float& want) {
        float t = std::min(pool, want);
        pool -= t;
        want -= t;
    };
    take(s_.bloodSugarG, carbNeed);
    if (exertion > 0.3f) {
        take(s_.muscleGlycogenG, carbNeed);
        take(s_.liverGlycogenG, carbNeed);
    } else {
        take(s_.liverGlycogenG, carbNeed);
        float muscleShare = carbNeed * 0.5f;     // muscles can use their own store at rest too
        float before = muscleShare;
        take(s_.muscleGlycogenG, muscleShare);
        carbNeed -= (before - muscleShare);
    }
    if (carbNeed > 0) {
        // The brain needs some glucose no matter what; it has to come from protein.
        float brainMin = (110.0f - 90.0f * keto) / kMinutesPerDay * dt;
        float fromProtein = std::min(carbNeed, brainMin);
        float proteinG = fromProtein * 1.75f;
        float poolUse = std::min(s_.aminoPoolG, proteinG);
        s_.aminoPoolG -= poolUse;
        s_.leanMassKg -= (proteinG - poolUse) / kLeanProteinFraction / 1000.0f;
        s_.urineExtraMl += proteinG * 7.0f;
        fatNeed += (carbNeed - fromProtein) * 4.0f / 9.0f;
    }

    // Fat: dietary fat first, then body fat (limited release rate)
    take(s_.dietaryFatG, fatNeed);
    float adiposeCap = kAdiposeKcalPerKgPerDay * s_.fatMassKg / 9.0f / kMinutesPerDay * dt;
    float available = std::max(0.0f, (s_.fatMassKg - kEssentialFatKg) * 1000.0f);
    float fromAdipose = std::min({fatNeed, adiposeCap, available});
    s_.fatMassKg -= fromAdipose / 1000.0f;
    fatNeed -= fromAdipose;

    // Whatever is left must come from protein: first free amino acids, then muscle
    float remainingKcal = fatNeed * 9.0f;
    if (remainingKcal > 0) {
        float poolKcal = std::min(s_.aminoPoolG * 4.0f, remainingKcal);
        s_.aminoPoolG -= poolKcal / 4.0f;
        remainingKcal -= poolKcal;
        s_.leanMassKg -= remainingKcal / kLeanKcalPerKg;
        s_.urineExtraMl += remainingKcal / 4.0f * kLeanProteinFraction * 7.0f;
    }

    // Store surplus
    if (s_.bloodSugarG > 5.0f) {
        float spare = s_.bloodSugarG - 5.0f;
        float toLiver = std::min({spare, 0.2f * dt, kMaxLiverGlycogen - s_.liverGlycogenG});
        s_.liverGlycogenG += std::max(0.0f, toLiver);
        spare -= std::max(0.0f, toLiver);
        float toMuscle = std::min({spare, 0.5f * dt, kMaxMuscleGlycogen - s_.muscleGlycogenG});
        s_.muscleGlycogenG += std::max(0.0f, toMuscle);
        spare -= std::max(0.0f, toMuscle);
        s_.bloodSugarG = 5.0f + spare;
        if (spare > 40.0f) {                       // de novo lipogenesis
            float conv = std::min(spare - 40.0f, 0.3f * dt);
            s_.bloodSugarG -= conv;
            s_.fatMassKg += conv * 0.35f / 1000.0f;
        }
    }
    if (s_.dietaryFatG > 0) {
        float store = s_.dietaryFatG * std::min(1.0f, 0.004f * dt);  // ~ quarter per hour
        s_.dietaryFatG -= store;
        s_.fatMassKg += store / 1000.0f;
    }

    // Adaptation
    if (s_.liverGlycogenG < 25.0f && s_.bloodSugarG < 8.0f)
        s_.ketoAdaptation = std::min(1.0f, s_.ketoAdaptation + dt / (3.0f * kMinutesPerDay));
    else if (s_.liverGlycogenG > 60.0f)
        s_.ketoAdaptation = std::max(0.0f, s_.ketoAdaptation - dt / (1.5f * kMinutesPerDay));
    if (s_.energyDebtKcal > 3000.0f)
        s_.starvationAdaptation = std::min(1.0f, s_.starvationAdaptation + dt / (7.0f * kMinutesPerDay));
    else
        s_.starvationAdaptation = std::max(0.0f, s_.starvationAdaptation - dt / (3.0f * kMinutesPerDay));
}

void Body::hydrate(float dt, float sweatMl) {
    float lossMl = (1400.0f / kMinutesPerDay) * dt;  // breathing, skin, minimum urine
    lossMl += s_.urineExtraMl;
    s_.urineExtraMl = 0;
    lossMl += sweatMl;
    lossMl += s_.diarrhea * 3.0f * dt;                // up to ~4 L/day
    s_.waterDeficitL += lossMl / 1000.0f;

    if (s_.waterDeficitL < 0)  // kidneys get rid of extra water
        s_.waterDeficitL = std::min(0.0f, s_.waterDeficitL + 15.0f / 1000.0f * dt);
    s_.waterDeficitL = std::max(-0.6f, s_.waterDeficitL);

    if (s_.bloodVolumeL < 5.0f && s_.waterDeficitL < 2.5f) {
        float regen = std::min(5.0f - s_.bloodVolumeL, 0.0007f * dt);
        s_.bloodVolumeL += regen;
        s_.waterDeficitL += regen;
    }
}

float Body::thermoregulate(float dt, const Environment& env, float metabolic) {
    // Clothes get wet in rain, dry slowly
    float wetIn = env.wetness * (1.0f - env.waterproof) * 0.05f;
    float dryOut = 0.004f + 0.0006f * std::max(0.0f, env.airTempC) + 0.003f * s_.sweating * 0.0f;
    s_.wetness = clamp01(s_.wetness + (wetIn - dryOut * (1.0f - env.wetness)) * dt);

    // Fever follows active infections
    float feverTarget = 0;
    for (const auto& inf : s_.infections) {
        if (inf.incubationH > 0) continue;
        switch (inf.type) {
            case Pathogen::FoodPoisoning: feverTarget += inf.load * 1.2f; break;
            case Pathogen::Giardia:       feverTarget += inf.load * 0.2f; break;
            case Pathogen::Tularemia:     feverTarget += inf.load * 2.6f; break;
            case Pathogen::Trichinosis:   feverTarget += inf.ageH > 48 ? inf.load * 1.6f : 0.0f; break;
            default: break;
        }
    }
    float worstWound = 0;
    for (const auto& w : s_.wounds) worstWound = std::max(worstWound, w.infection);
    feverTarget += worstWound * 2.2f;
    feverTarget = std::min(feverTarget, 3.5f);
    s_.feverC += std::clamp(feverTarget - s_.feverC, -0.02f * dt, 0.02f * dt);

    float setpoint = 37.0f + s_.feverC;
    float e = s_.coreTempC - setpoint;

    float rTissue = e < 0 ? lerp(0.04f, 0.12f, ramp(-e, 0.0f, 0.3f)) : lerp(0.04f, 0.025f, ramp(e, 0.0f, 0.5f));
    float clo = env.clothingClo * (1.0f - 0.6f * s_.wetness);
    float rClo = clo * 0.155f;
    float rAir = 0.11f / (1.0f + 0.35f * env.wind);
    float conductW = 1.8f * (s_.coreTempC - env.airTempC) / (rTissue + rClo + rAir);
    float wetEvapW = s_.wetness * 60.0f * (env.airTempC < 30 ? 1.0f : 0.5f);
    float respW = 0.12f * metabolic * kWattsPerKcalMin;

    float shiver = clamp01((-e - 0.2f) / 1.0f);
    if (s_.coreTempC < 32.0f) shiver *= ramp(s_.coreTempC, 30.0f, 32.0f);  // the body gives up
    if (s_.asleep) shiver *= 0.7f;
    s_.shivering = shiver;
    float shiverKcal = shiver * 3.0f * bmrKcalPerDay() / kMinutesPerDay;

    s_.sweating = clamp01((e - 0.1f) / 0.6f);
    float sweatW = s_.sweating * 450.0f;

    float producedW = (metabolic + shiverKcal) * kWattsPerKcalMin * 0.9f;
    float netW = producedW - conductW - wetEvapW - respW - sweatW;
    s_.coreTempC += netW * 60.0f * dt / (3500.0f * bodyMassKg());

    bool shiveringNow = shiver > 0.25f;
    if (shiveringNow && !wasShivering_) emit(BodyEvent::Shivering);
    if (!shiveringNow && wasShivering_ && s_.coreTempC < 32.0f) emit(BodyEvent::StoppedShivering);
    wasShivering_ = shiveringNow;
    return shiverKcal;
}

void Body::updateInfections(float dt) {
    float h = dt / 60.0f;
    float immune = immuneStrength();
    float diarrheaTarget = 0;
    for (auto& inf : s_.infections) {
        if (inf.incubationH > 0) { inf.incubationH -= h; continue; }
        inf.ageH += h;
        float rate = 1.0f / 60.0f;   // immunity per hour
        switch (inf.type) {
            case Pathogen::FoodPoisoning: rate = 1.0f / 60.0f; break;
            case Pathogen::Giardia:       rate = 1.0f / 500.0f; break;
            case Pathogen::Tularemia:     rate = 1.0f / 300.0f; break;
            case Pathogen::Trichinosis:   rate = 1.0f / 600.0f; break;
            default: break;
        }
        float treat = inf.treated ? 8.0f : 1.0f;
        inf.immunity += h * rate * immune * treat;
        if (inf.immunity < 0.5f) inf.load += (inf.peak - inf.load) * std::min(1.0f, 0.15f * h);
        else inf.load -= h * rate * 2.0f * immune * treat * std::max(0.3f, inf.peak);
        inf.load = clamp01(inf.load);

        switch (inf.type) {
            case Pathogen::FoodPoisoning: diarrheaTarget = std::max(diarrheaTarget, inf.load); break;
            case Pathogen::Giardia:       diarrheaTarget = std::max(diarrheaTarget, inf.load * 0.7f); break;
            case Pathogen::Trichinosis:
                if (inf.ageH < 48) diarrheaTarget = std::max(diarrheaTarget, inf.load * 0.6f);
                break;
            default: break;
        }
    }
    s_.infections.erase(std::remove_if(s_.infections.begin(), s_.infections.end(),
                                       [](const Infection& i) { return i.immunity >= 1.0f || (i.immunity > 0.5f && i.load <= 0.0f); }),
                        s_.infections.end());

    diarrheaTarget = std::max(diarrheaTarget, ramp(s_.ammonia, 0.25f, 0.8f) * 0.8f);
    for (const auto& t : s_.toxins)
        if (t.type == Toxin::Amatoxin && t.ageH > 8 && t.ageH < 48) diarrheaTarget = std::max(diarrheaTarget, std::min(1.0f, t.amount * 1.2f));
    s_.diarrhea += (diarrheaTarget - s_.diarrhea) * std::min(1.0f, 0.02f * dt);
}

void Body::updateToxins(float dt) {
    float h = dt / 60.0f;
    s_.hallucination = 0;
    for (auto& t : s_.toxins) {
        t.ageH += h;
        if (t.type == Toxin::Amatoxin) {
            // Latent for most of a day, gut phase, false recovery, then the liver goes.
            if (t.ageH > 24.0f && t.ageH < 168.0f)
                s_.liverDamage += t.amount * 0.25f / 24.0f * h;
        } else if (t.type == Toxin::Muscimol) {
            float effect = t.amount * ramp(t.ageH, 0.5f, 1.5f) * (1.0f - ramp(t.ageH, 6.0f, 10.0f));
            s_.hallucination = std::max(s_.hallucination, clamp01(effect));
        }
    }
    s_.liverDamage = std::min(1.0f, s_.liverDamage);
    s_.toxins.erase(std::remove_if(s_.toxins.begin(), s_.toxins.end(),
                                   [](const ToxinDose& t) { return t.ageH > 200.0f; }),
                    s_.toxins.end());
    if (s_.hoursAwake > 60.0f) s_.hallucination = std::max(s_.hallucination, ramp(s_.hoursAwake, 60.0f, 90.0f));
}

void Body::updateWounds(float dt) {
    float h = dt / 60.0f;
    float immune = immuneStrength();
    // Healing needs protein, energy and vitamin C
    float nutrition = ramp(s_.vitaminCMg, 50.0f, 400.0f) * lerp(0.4f, 1.0f, 1.0f - ramp(s_.energyDebtKcal, 5000.0f, 30000.0f));
    float bleedTotal = 0;
    for (auto& w : s_.wounds) {
        float clot = 0.04f * (1.0f - 0.8f * w.severity) * ramp(s_.vitaminCMg, 30.0f, 200.0f);
        w.bleeding *= std::exp(-clot * dt);
        if (w.bleeding < 0.05f) w.bleeding = 0;
        bool bandageWorks = w.bandaged && w.bandageDirt < 0.8f;
        bleedTotal += bandageWorks ? w.bleeding * 0.1f : w.bleeding;

        if (w.bandaged) w.bandageDirt = std::min(1.0f, w.bandageDirt + h / 48.0f);
        float contamination = w.contamination + (w.bandaged && w.bandageDirt > 0.7f ? 0.3f : 0.0f);
        if (w.infection <= 0 && !w.disinfected && rng_.chance(contamination * 0.02f * h)) {
            w.infection = 0.05f;
            emit(BodyEvent::WoundInfected);
        }
        if (w.infection > 0) {
            float grow = 0.012f * (1.0f - 0.7f * immune);
            float fight = 0.006f * immune + (s_.woundAntibioticH > 0 ? 0.03f : 0.0f) + (w.disinfected ? 0.004f : 0.0f);
            w.infection = clamp01(w.infection + (grow - fight) * h);
            if (w.infection <= 0.001f) w.infection = 0;
        }
        float healRate = 1.0f / (24.0f * (1.0f + w.severity * 10.0f)) * nutrition * (w.infection > 0.2f ? 0.2f : 1.0f);
        if (w.bleeding <= 0) w.healed += healRate * h;
    }
    s_.bloodVolumeL -= bleedTotal * dt / 1000.0f;
    s_.wounds.erase(std::remove_if(s_.wounds.begin(), s_.wounds.end(),
                                   [](const Wound& w) { return w.healed >= 1.0f && w.infection <= 0; }),
                    s_.wounds.end());
}

void Body::updateRest(float dt) {
    if (s_.asleep) {
        float quality = 1.0f - 0.6f * s_.shivering - 0.4f * pain();
        s_.sleepPressure = std::max(0.0f, s_.sleepPressure - dt / (7.0f * 60.0f) * std::max(0.2f, quality));
    } else {
        s_.sleepPressure = std::min(1.0f, s_.sleepPressure + dt / (17.0f * 60.0f));
        s_.hoursAwake += dt / 60.0f;
        if (s_.sleepPressure >= 1.0f && s_.hoursAwake > 40.0f && rng_.chance(0.002f * dt)) {
            emit(BodyEvent::FellAsleep);
        }
    }
}

void Body::updateDamage(float dt) {
    float perDay = dt / kMinutesPerDay;
    struct Src { DeathCause cause; float rate; };
    float leanRatio = s_.leanMassKg / s_.leanBaselineKg;
    float maxWoundInf = 0;
    for (const auto& w : s_.wounds) maxWoundInf = std::max(maxWoundInf, w.infection);
    float tularemia = 0;
    for (const auto& inf : s_.infections) if (inf.type == Pathogen::Tularemia) tularemia = inf.load;
    Src sources[] = {
        {DeathCause::ProteinPoisoning, std::max(0.0f, s_.ammonia - 0.45f) * 0.8f},
        {DeathCause::Scurvy,           0.04f * ramp(80.0f - s_.vitaminCMg, 0.0f, 80.0f)},
        {DeathCause::Starvation,       0.15f * ramp(0.7f - leanRatio, 0.0f, 0.15f)},
        {DeathCause::Dehydration,      dehydrationFraction() > 0.08f ? 0.5f : 0.0f},
        {DeathCause::LiverFailure,     std::max(0.0f, s_.liverDamage - 0.5f) * 0.3f},
        {DeathCause::Sepsis,           std::max(0.0f, maxWoundInf - 0.7f) * 1.5f},
        {DeathCause::Infection,        std::max(0.0f, tularemia - 0.6f) * 0.1f},
        {DeathCause::Hypothermia,      s_.coreTempC < 33.0f ? 0.3f : 0.0f},
        {DeathCause::Hyperthermia,     s_.coreTempC > 40.5f ? 0.5f : 0.0f},
    };
    float total = 0, worst = 0;
    for (const auto& s : sources) {
        total += s.rate;
        if (s.rate > worst) { worst = s.rate; s_.worstDamageSource = s.cause; }
    }
    if (total > 0) {
        s_.systemicDamage += total * perDay;
    } else {
        float recovery = 0.04f * immuneStrength();
        s_.systemicDamage = std::max(0.0f, s_.systemicDamage - recovery * perDay);
    }
}

void Body::checkDeath() {
    if (dead()) return;
    DeathCause c = DeathCause::None;
    if (dehydrationFraction() >= 0.12f) c = DeathCause::Dehydration;
    else if (s_.leanMassKg <= 0.55f * s_.leanBaselineKg) c = DeathCause::Starvation;
    else if (s_.ammonia >= 1.0f) c = DeathCause::ProteinPoisoning;
    else if (s_.coreTempC < 28.0f) c = DeathCause::Hypothermia;
    else if (s_.coreTempC > 42.5f) c = DeathCause::Hyperthermia;
    else if (s_.bloodVolumeL <= 3.0f) c = DeathCause::BloodLoss;
    else if (s_.liverDamage >= 1.0f) c = DeathCause::LiverFailure;
    else if (s_.systemicDamage >= 1.0f) c = s_.worstDamageSource != DeathCause::None ? s_.worstDamageSource : DeathCause::Infection;
    s_.deathCause = c;
}

// ---------------------------------------------------------------------------
// What it feels like
// ---------------------------------------------------------------------------

BodySignals Body::signals() const {
    BodySignals g;
    const BodyState& s = s_;
    float dehyd = dehydrationFraction();
    float leanRatio = s.leanMassKg / s.leanBaselineKg;
    float full = fullness();

    float foodPoison = 0, giardia = 0, tularemia = 0, trichGut = 0, trichMuscle = 0;
    for (const auto& inf : s.infections) {
        if (inf.incubationH > 0) continue;
        switch (inf.type) {
            case Pathogen::FoodPoisoning: foodPoison = std::max(foodPoison, inf.load); break;
            case Pathogen::Giardia:       giardia = std::max(giardia, inf.load); break;
            case Pathogen::Tularemia:     tularemia = std::max(tularemia, inf.load); break;
            case Pathogen::Trichinosis:
                if (inf.ageH < 48) trichGut = std::max(trichGut, inf.load);
                else trichMuscle = std::max(trichMuscle, inf.load);
                break;
            default: break;
        }
    }
    float amaGut = 0, muscimol = 0;
    for (const auto& t : s.toxins) {
        if (t.type == Toxin::Amatoxin && t.ageH > 8 && t.ageH < 48) amaGut = std::max(amaGut, std::min(0.85f, t.amount * 1.2f));
        if (t.type == Toxin::Muscimol)
            muscimol = std::max(muscimol, clamp01(t.amount * ramp(t.ageH, 0.5f, 1.5f) * (1.0f - ramp(t.ageH, 6.0f, 10.0f))));
    }
    float lowSugar = (s.liverGlycogenG < 8.0f && s.bloodSugarG < 2.0f) ? (1.0f - std::min(1.0f, s.ketoAdaptation * 1.5f)) : 0.0f;
    float ketoFlu = (s.ketoAdaptation > 0.05f && s.ketoAdaptation < 0.6f && s.liverGlycogenG < 25.0f) ? 0.35f : 0.0f;
    float bloodLoss = ramp(5.0f - s.bloodVolumeL, 0.4f, 1.8f);

    g.hunger = hunger();
    g.thirst = thirst();
    g.full = ramp(full, 0.8f, 1.8f);

    float totalKcal = std::max(300.0f, s.avgKcalProtein + s.avgKcalFat + s.avgKcalCarbs);
    float proteinShare = s.avgKcalProtein / totalKcal;
    float fatShare = s.avgKcalFat / totalKcal;
    g.craving = ramp(proteinShare, 0.3f, 0.55f) * (1.0f - ramp(fatShare, 0.1f, 0.25f));
    g.craving = std::max(g.craving, 0.6f * ramp(4.0f - s.fatMassKg, 0.0f, 2.0f));

    g.nausea = std::max({ramp(s.ammonia, 0.12f, 0.9f), foodPoison * 0.9f, amaGut, muscimol * 0.8f,
                         giardia * 0.4f, trichGut * 0.6f, ramp(full, 1.4f, 2.4f), s.nauseaExtra,
                         ramp(dehyd, 0.04f, 0.09f) * 0.5f, ramp(s.liverDamage, 0.2f, 0.8f) * 0.8f,
                         tularemia * 0.3f});
    g.cramps = std::max({s.diarrhea, giardia * 0.6f, ramp(full, 1.6f, 2.6f) * 0.4f});
    g.weakness = std::max({ramp(leanRatio, 0.95f, 0.6f), lowSugar * 0.5f, ketoFlu,
                           ramp(s.vitaminCMg, 300.0f, 50.0f) * 0.6f, ramp(dehyd, 0.03f, 0.09f),
                           bloodLoss, std::max({foodPoison, tularemia, giardia * 0.5f}) * 0.6f,
                           ramp(s.ammonia, 0.2f, 0.9f) * 0.8f, ramp(4.0f - s.fatMassKg, 0.0f, 2.0f) * 0.4f,
                           ramp(s.liverDamage, 0.3f, 0.9f)});
    g.headache = std::max({ramp(dehyd, 0.015f, 0.06f), ketoFlu, ramp(s.hoursAwake, 20.0f, 48.0f) * 0.6f,
                           ramp(s.feverC, 0.5f, 2.5f), ramp(s.ammonia, 0.2f, 0.9f) * 0.6f, muscimol * 0.5f,
                           tularemia * 0.8f});
    g.dizziness = std::max({bloodLoss, ramp(dehyd, 0.04f, 0.1f), lowSugar * 0.6f, muscimol,
                            ramp(s.coreTempC, 35.0f, 32.0f) * 0.5f});
    g.confusion = std::max({ramp(s.ammonia, 0.5f, 1.0f), ramp(s.coreTempC, 34.5f, 31.0f),
                            ramp(s.bloodVolumeL, 3.8f, 3.0f), ramp(s.hoursAwake, 40.0f, 72.0f),
                            muscimol * 0.8f, ramp(dehyd, 0.07f, 0.11f), ramp(s.liverDamage, 0.6f, 1.0f),
                            ramp(s.feverC, 2.0f, 3.5f)});
    g.cold = std::max(ramp(s.coreTempC, 36.75f, 33.0f), s.shivering * 0.8f);
    g.hot = s.feverC < 0.3f ? ramp(s.coreTempC, 37.4f, 40.0f) : 0.0f;
    g.fever = ramp(s.feverC, 0.3f, 2.5f);
    g.tired = std::max(ramp(s.sleepPressure, 0.5f, 1.0f), muscimol * 0.5f);
    g.pain = pain();
    g.mouthSore = ramp(s.vitaminCMg, 300.0f, 80.0f);
    g.aches = std::max({trichMuscle, tularemia * 0.6f, ramp(s.vitaminCMg, 200.0f, 40.0f) * 0.7f, g.fever * 0.4f});
    g.hallucination = s.hallucination;
    g.bleeding = ramp(totalBleeding(), 0.3f, 25.0f);
    g.skin = ramp(s.liverDamage, 0.35f, 0.9f);
    g.swollenGlands = tularemia;
    return g;
}

Capacity Body::capacity() const {
    BodySignals g = signals();
    Capacity c;
    c.moveSpeed = 1.0f - 0.5f * g.weakness - 0.3f * g.pain - 0.25f * g.dizziness - 0.2f * g.tired - 0.2f * ramp(s_.coreTempC, 35.5f, 32.0f);
    c.moveSpeed = std::max(0.3f, c.moveSpeed);
    c.strength = std::max(0.2f, 1.0f - 0.6f * g.weakness - 0.3f * g.pain);
    c.accuracy = std::max(0.2f, 1.0f - 0.4f * g.dizziness - 0.3f * g.cold - 0.3f * g.confusion - 0.2f * g.tired);
    c.endurance = std::max(0.2f, 1.0f - 0.6f * g.weakness - 0.3f * ramp(s_.liverGlycogenG + s_.muscleGlycogenG, 120.0f, 10.0f));
    c.awareness = std::max(0.2f, 1.0f - 0.7f * std::max({g.confusion, g.dizziness * 0.8f, g.hallucination}));
    return c;
}

} // namespace alone
