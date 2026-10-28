// Diet and environment scenarios for the physiology model. These check that
// the model behaves like a human body over days and weeks.
#include "TestFramework.h"
#include "survival/Body.h"
#include <cmath>
#include <cstdio>
#include <functional>

using namespace alone;

namespace {

// Per 100 g, roughly from food composition tables (Livsmedelsverket / USDA)
Nutrients per100(float protein, float fat, float carbs, float water, float vitC = 0, float fiber = 0) {
    Nutrients n; n.protein = protein; n.fat = fat; n.carbs = carbs; n.water = water; n.vitaminC = vitC; n.fiber = fiber;
    return n;
}
const Nutrients kHareCooked = per100(30.0f, 3.5f, 0.0f, 62.0f);    // lean game meat
const Nutrients kButter     = per100(0.7f, 81.0f, 0.6f, 16.0f);
const Nutrients kMixedMeal  = per100(6.0f, 6.0f, 18.0f, 65.0f, 6.0f, 2.0f);  // stew-like

Environment comfy() { Environment e; e.airTempC = 20; e.clothingClo = 1.0f; return e; }

struct Sim {
    Body body{42};
    float day() const { return body.state().minutesAlive / 1440.0f; }
    // Live a day: eat `meal` (grams of food) whenever hungry, drink when thirsty.
    void live(float days, std::function<void(Sim&)> eatHook, bool drink = true, Environment env = comfy()) {
        int steps = (int)(days * 24 * 4);
        for (int i = 0; i < steps && !body.dead(); ++i) {
            float hour = std::fmod(body.state().minutesAlive / 60.0f, 24.0f);
            bool sleeping = hour >= 23 || hour < 7;
            body.setAsleep(sleeping);
            if (!sleeping) {
                eatHook(*this);
                if (drink && body.thirst() > 0.05f) body.ingest(per100(0, 0, 0, 100).scaled(3.0f), 0);
            }
            body.update(15.0f, env, sleeping ? Activity::Sleeping : (i % 3 == 0 ? Activity::Walking : Activity::Standing));
        }
    }
};

void report(const char* name, Sim& s) {
    const auto& st = s.body.state();
    std::printf("    %-22s day %5.1f  dead=%-24s lean %.1f fat %.1f ammonia %.2f water -%.2fL vitC %.0f\n",
                name, s.day(), s.body.dead() ? deathCauseName(s.body.deathCause()) : "no", st.leanMassKg,
                st.fatMassKg, st.ammonia, st.waterDeficitL, st.vitaminCMg);
}

} // namespace

TEST(balanced_diet_is_sustainable) {
    Sim s;
    s.live(30, [](Sim& s) {
        if (s.body.hunger() > 0.45f) s.body.ingest(kMixedMeal.scaled(6.0f), 500);  // 600 g stew, ~660 kcal
    });
    report("balanced", s);
    CHECK(!s.body.dead());
    CHECK(s.body.state().ammonia < 0.2f);
    CHECK(s.body.state().leanMassKg > 58.0f);
}

TEST(rabbit_only_diet_kills) {
    Sim s;
    s.live(60, [](Sim& s) {
        if (s.body.hunger() > 0.45f && s.body.fullness() < 0.6f) s.body.ingest(kHareCooked.scaled(3.5f), 330);
    });
    report("hare only", s);
    CHECK(s.body.dead());
    CHECK(s.body.deathCause() == DeathCause::ProteinPoisoning);
    CHECK(s.day() > 5.0f && s.day() < 40.0f);
}

TEST(hare_with_fat_is_sustainable) {
    Sim s;
    int meal = 0;
    s.live(30, [&](Sim& s) {
        if (s.body.hunger() > 0.45f && s.body.fullness() < 0.6f) {
            s.body.ingest(kHareCooked.scaled(2.0f), 190);
            s.body.ingest(kButter.scaled(0.5f), 50);
            ++meal;
        }
    });
    report("hare + butter", s);
    CHECK(!s.body.dead());
    CHECK(s.body.state().ammonia < 0.3f);
}

TEST(no_water_kills_in_days) {
    Sim s;
    s.live(10, [](Sim& s) {
        if (s.body.hunger() > 0.45f) s.body.ingest(per100(6, 6, 18, 0).scaled(3.0f), 250);
    }, false);
    report("no water", s);
    CHECK(s.body.dead());
    CHECK(s.body.deathCause() == DeathCause::Dehydration);
    CHECK(s.day() > 2.5f && s.day() < 7.0f);
}

TEST(no_food_kills_in_weeks) {
    Sim s;
    s.live(120, [](Sim&) {});
    report("no food", s);
    CHECK(s.body.dead());
    CHECK(s.body.deathCause() == DeathCause::Starvation);
    CHECK(s.day() > 25.0f && s.day() < 90.0f);
}

TEST(death_cap_destroys_liver) {
    Sim s;
    s.body.ingestToxin(Toxin::Amatoxin, 1.0f);
    float worstNauseaDay1 = 0;
    s.live(12, [&](Sim& s) {
        if (s.day() < 1.0f) worstNauseaDay1 = std::max(worstNauseaDay1, s.body.signals().nausea);
        if (s.body.hunger() > 0.45f) s.body.ingest(kMixedMeal.scaled(5.0f), 450);
    });
    report("death cap", s);
    CHECK(s.body.dead());
    CHECK(s.body.deathCause() == DeathCause::LiverFailure);
    CHECK(s.day() > 3.0f);
}

TEST(wet_and_cold_is_dangerous) {
    Sim s;
    Environment env; env.airTempC = 2; env.clothingClo = 0.5f; env.wetness = 1.0f; env.wind = 5;
    for (int i = 0; i < 24 * 4 && !s.body.dead(); ++i) s.body.update(15, env, Activity::Resting);
    report("wet + cold", s);
    CHECK(s.body.dead());
    CHECK(s.body.deathCause() == DeathCause::Hypothermia);
}

TEST(warm_clothes_protect_from_cold) {
    Sim s;
    Environment env; env.airTempC = 2; env.clothingClo = 2.5f; env.waterproof = 0.9f; env.wetness = 0.5f;
    for (int i = 0; i < 12 * 4; ++i) s.body.update(15, env, Activity::Walking);
    report("dressed for cold", s);
    CHECK(!s.body.dead());
    CHECK(s.body.state().coreTempC > 36.0f);
}

TEST(bleeding_wound_bandaged_survives) {
    Sim s;
    Wound w; w.severity = 0.7f; w.bleeding = 60.0f; w.type = WoundType::Laceration;
    s.body.addWound(w);
    s.body.update(3, comfy(), Activity::Standing);
    s.body.applyDressing(BodyPart::Torso, DressingKind::Bandage, true, 0);
    s.body.update(600, comfy(), Activity::Resting);
    report("bandaged wound", s);
    CHECK(!s.body.dead());

    Sim t;
    t.body.addWound(w);
    t.body.update(600, comfy(), Activity::Resting);
    report("untreated wound", t);
    CHECK(t.body.dead());
    CHECK(t.body.deathCause() == DeathCause::BloodLoss);
}

TEST(bandaid_is_useless_on_a_deep_cut) {
    Wound w; w.part = BodyPart::LeftArm; w.severity = 0.7f; w.bleeding = 40.0f; w.type = WoundType::Laceration;
    Sim a, b, c;
    a.body.addWound(w); b.body.addWound(w); c.body.addWound(w);
    a.body.applyDressing(BodyPart::LeftArm, DressingKind::Bandaid, true, 0);
    b.body.applyDressing(BodyPart::LeftArm, DressingKind::Bandage, true, 0);
    c.body.applyDressing(BodyPart::LeftArm, DressingKind::Rag, false, 0.1f);
    std::printf("    bleeding through: bandaid %.1f, bandage %.1f, rag %.1f ml/min\n", a.body.totalBleeding(),
                b.body.totalBleeding(), c.body.totalBleeding());
    CHECK(a.body.totalBleeding() > 30.0f);
    CHECK(b.body.totalBleeding() < 6.0f);
    CHECK(c.body.totalBleeding() < b.body.totalBleeding() * 3.0f);
    // On a scratch, a bandaid works
    Sim d;
    Wound sc; sc.part = BodyPart::RightHand; sc.severity = 0.15f; sc.bleeding = 3.0f;
    d.body.addWound(sc);
    d.body.applyDressing(BodyPart::RightHand, DressingKind::Bandaid, true, 0);
    CHECK(d.body.totalBleeding() < 1.0f);
}

TEST(dirty_dressing_infects_clean_one_protects) {
    Wound w; w.part = BodyPart::LeftLeg; w.severity = 0.5f; w.bleeding = 0; w.contamination = 0.2f;
    int infectedDirty = 0, infectedClean = 0;
    for (int trial = 0; trial < 12; ++trial) {
        Sim dirty, clean;
        dirty.body = Body(100 + trial); clean.body = Body(100 + trial);
        dirty.body.addWound(w); clean.body.addWound(w);
        clean.body.disinfect(BodyPart::LeftLeg);
        clean.body.applyDressing(BodyPart::LeftLeg, DressingKind::Bandage, true, 0);
        dirty.body.applyDressing(BodyPart::LeftLeg, DressingKind::Rag, false, 0.9f);
        for (int i = 0; i < 4 * 24 * 2; ++i) {   // two days
            dirty.body.update(15, comfy(), Activity::Standing);
            clean.body.update(15, comfy(), Activity::Standing);
            dirty.body.soil(BodyPart::LeftLeg, 0.01f);
        }
        if (!dirty.body.state().wounds.empty() && dirty.body.state().wounds[0].infection > 0) ++infectedDirty;
        if (!clean.body.state().wounds.empty() && clean.body.state().wounds[0].infection > 0) ++infectedClean;
    }
    std::printf("    infected after 2 days: %d/12 under a filthy rag, %d/12 under a clean bandage\n", infectedDirty, infectedClean);
    CHECK(infectedDirty > infectedClean + 3);
}

TEST(food_poisoning_resolves) {
    Sim s;
    s.body.expose(Pathogen::FoodPoisoning, 0.6f);
    float worstNausea = 0, worstCramps = 0;
    s.live(10, [&](Sim& s) {
        auto g = s.body.signals();
        worstNausea = std::max(worstNausea, g.nausea);
        worstCramps = std::max(worstCramps, g.cramps);
        if (s.body.hunger() > 0.45f) s.body.ingest(kMixedMeal.scaled(5.0f), 450);
    });
    report("food poisoning", s);
    std::printf("    worst nausea %.2f cramps %.2f\n", worstNausea, worstCramps);
    CHECK(!s.body.dead());
    CHECK(worstNausea > 0.3f);
    CHECK(s.body.state().infections.empty());
}
