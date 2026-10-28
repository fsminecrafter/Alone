#pragma once
#include "core/Rng.h"
#include "survival/Nutrients.h"
#include "survival/Wound.h"
#include <array>
#include <string>
#include <vector>

namespace alone {

// What the player is doing, which sets how much energy they burn.
enum class Activity { Sleeping, Resting, Standing, Walking, Running, Working, Fighting };

// The world around the body, supplied by the game each update.
struct Environment {
    float airTempC = 15.0f;     // effective temperature (indoors, fires already applied)
    float wetness = 0.0f;       // 0..1, how soaked the clothes/skin are getting (rain, swimming)
    float wind = 0.0f;          // m/s
    float clothingClo = 1.0f;   // total clothing insulation in clo
    float waterproof = 0.0f;    // 0..1, how well the outer layer keeps rain out
    float carriedKg = 0.0f;     // weight carried
};

enum class DeathCause {
    None, Dehydration, Starvation, ProteinPoisoning, Hypothermia, Hyperthermia,
    BloodLoss, Sepsis, LiverFailure, Scurvy, Infection, Poisoning, Count
};
const char* deathCauseName(DeathCause c);
const char* deathCauseExplanation(DeathCause c);

// Things the body does that the player notices, shown as messages.
enum class BodyEvent { Vomited, Diarrhea, Collapsed, FellAsleep, Shivering, StoppedShivering, WoundInfected, BleedingStopped };

struct Infection {
    Pathogen type = Pathogen::None;
    float incubationH = 0;   // hours until symptoms start
    float load = 0;          // 0..1 current severity
    float peak = 0;          // how bad it will get
    float immunity = 0;      // 0..1 immune response; clears the infection at 1
    float ageH = 0;          // hours since symptoms started
    bool treated = false;    // on the right medicine
};

struct ToxinDose {
    Toxin type = Toxin::None;
    float amount = 0;        // 0..1+ (1 = typically lethal for amatoxin)
    float ageH = 0;          // hours since ingestion
};

// Full physiological state of the player. All quantities are in real units so
// the model can be checked against reality. Nothing here is shown directly to
// the player; Symptoms.cpp translates it into vague feelings.
struct BodyState {
    // --- composition -----------------------------------------------------
    float leanMassKg = 60.0f;       // everything except fat and glycogen
    float leanBaselineKg = 60.0f;   // healthy lean mass for this person
    float fatMassKg = 12.0f;        // adipose tissue (fat content)
    float liverGlycogenG = 80.0f;   // max ~100 g, feeds the brain between meals
    float muscleGlycogenG = 350.0f; // max ~400 g, feeds working muscles

    // --- digestion -------------------------------------------------------
    Nutrients stomach;              // food waiting to be digested
    float stomachSolidMl = 0;       // volume of solid food in the stomach
    float bloodSugarG = 5.0f;       // absorbed carbohydrate in circulation
    float dietaryFatG = 0;          // absorbed fat in circulation (chylomicrons)
    float aminoPoolG = 20.0f;       // free amino acids from digested protein
    float energyCreditKcal = 0;     // energy from deaminated protein not yet used
    float ketoAdaptation = 0;       // 0..1, adapted to running on fat
    float starvationAdaptation = 0; // 0..1, metabolism slowed by long deficits
    float ammonia = 0;              // 0..1+, from protein beyond the liver's urea capacity
    float vitaminCMg = 1400.0f;     // body pool, scurvy below ~300

    // Rolling 3-day intake (exponential averages, kcal/day) for cravings
    float avgKcalProtein = 300, avgKcalFat = 800, avgKcalCarbs = 1200;
    float energyDebtKcal = 0;       // cumulative deficit beyond glycogen

    // --- water & blood -----------------------------------------------------
    float waterDeficitL = 0;        // litres below ideal hydration (negative = surplus)
    float bloodVolumeL = 5.0f;

    // --- temperature ---------------------------------------------------------
    float coreTempC = 37.0f;
    float feverC = 0;               // current setpoint raise from infection
    float shivering = 0;            // 0..1
    float sweating = 0;             // 0..1
    float wetness = 0;              // 0..1, current dampness of skin/clothes

    // --- rest ------------------------------------------------------------
    float sleepPressure = 0.15f;    // 0..1, need to sleep
    float hoursAwake = 2.0f;
    bool asleep = false;

    // --- damage & illness -----------------------------------------------
    float systemicDamage = 0;       // 0..1 organ damage; death at 1
    float liverDamage = 0;          // 0..1
    float diarrhea = 0;             // 0..1 current severity
    float nauseaExtra = 0;          // short-lived nausea (overeating, motion, disgust)
    float painkiller = 0;           // 0..1 active painkiller effect
    float hallucination = 0;        // 0..1
    std::vector<Wound> wounds;
    std::array<Dressing, (size_t)BodyPart::Count> dressings{};   // one per body part
    float sting = 0;                // 0..1 short sharp pain (disinfectant in a wound)
    std::vector<Infection> infections;
    std::vector<ToxinDose> toxins;

    // --- bookkeeping -----------------------------------------------------
    float lastExpenditureKcalPerMin = 1.2f;
    float minutesAlive = 0;
    float woundAntibioticH = 0;     // hours of antibiotic cover left for wounds
    float urineExtraMl = 0;         // water needed to flush urea this step
    DeathCause worstDamageSource = DeathCause::None;
    DeathCause deathCause = DeathCause::None;
};

// What the body *feels*, 0..1 each. These are the only things the player gets
// to know about (through Symptoms), and they never name a cause.
struct BodySignals {
    float hunger = 0, thirst = 0, full = 0, craving = 0;
    float nausea = 0, cramps = 0, weakness = 0, headache = 0, dizziness = 0;
    float confusion = 0, cold = 0, hot = 0, fever = 0, tired = 0, pain = 0;
    float mouthSore = 0, aches = 0, hallucination = 0, bleeding = 0, skin = 0;
    float swollenGlands = 0;
};

// How well the body can currently perform, used to scale player actions.
struct Capacity {
    float moveSpeed = 1;    // multiplier on walking/running speed
    float strength = 1;     // multiplier on melee damage and carry limit
    float accuracy = 1;     // multiplier on hit chance / aim steadiness
    float endurance = 1;    // multiplier on stamina recovery
    float awareness = 1;    // 0..1, below 1 the screen blurs/sways
};

class Body {
public:
    explicit Body(uint64_t seed = 1);

    // Advance the simulation by `minutes` of game time.
    void update(float minutes, const Environment& env, Activity activity);

    // Swallow food or drink. `solidMl` is the volume of solid food.
    // Returns false if the body refuses (e.g. too nauseous / too full).
    void ingest(const Nutrients& n, float solidMl);
    void expose(Pathogen p, float dose);        // dose 0..1
    void ingestToxin(Toxin t, float amount);
    void addWound(const Wound& w);
    void vomit();

    // Treatments, per body part
    // Puts a dressing on (replacing any old one, which is returned).
    // `dirt` is how dirty the dressing already is (a used rag, a washed bandage).
    Dressing applyDressing(BodyPart part, DressingKind kind, bool sterile, float dirt);
    Dressing removeDressing(BodyPart part);
    // Cleans the wounds on a part. strength 1 = proper disinfectant, less for vodka.
    void disinfect(BodyPart part, float strength = 1.0f);
    // Dirt reaching a body part from outside: soils its dressing, or gets
    // into an open wound when there is none.
    void soil(BodyPart part, float amount);
    const Dressing& dressing(BodyPart part) const { return s_.dressings[(size_t)part]; }
    bool woundedAt(BodyPart part) const;
    float worstSeverityAt(BodyPart part) const;
    float bleedingAt(BodyPart part) const;   // ml/min getting past any dressing
    // 0..1 how much an injured part hampers you (limping, a useless hand)
    float impairment(BodyPart part) const;
    void takePainkiller(float strength = 0.6f);
    void takeAntibiotic(Pathogen against);      // marks matching infections treated
    void takeVitaminC(float mg) { s_.vitaminCMg += mg * 0.7f; }

    void setAsleep(bool a) { s_.asleep = a; if (a) s_.hoursAwake = 0; }

    bool dead() const { return s_.deathCause != DeathCause::None; }
    DeathCause deathCause() const { return s_.deathCause; }
    const BodyState& state() const { return s_; }
    BodyState& mutableState() { return s_; }   // for tests and debugging

    float bodyMassKg() const;
    float bmrKcalPerDay() const;
    float fullness() const;          // 0..1+ (1 = comfortably full)
    float hunger() const;            // 0..1
    float thirst() const;            // 0..1
    Capacity capacity() const;
    float pain() const;              // 0..1
    float totalBleeding() const;     // ml/min
    float dehydrationFraction() const { return s_.waterDeficitL / bodyMassKg(); }
    BodySignals signals() const;

    // Events that happened since the last call (vomiting etc).
    std::vector<BodyEvent> takeEvents();

private:
    void stepMinute(float dt, const Environment& env, Activity activity);
    void digest(float dt);
    void metabolise(float dt, float expenditureKcal, float exertion);
    void processProtein(float dt);
    void hydrate(float dt, float sweatMl);
    float thermoregulate(float dt, const Environment& env, float metabolicKcalPerMin);
    void updateInfections(float dt);
    void updateToxins(float dt);
    void updateWounds(float dt);
    void updateRest(float dt);
    void updateDamage(float dt);
    void checkDeath();
    float immuneStrength() const;
    void emit(BodyEvent e) { events_.push_back(e); }

    BodyState s_;
    Rng rng_;
    std::vector<BodyEvent> events_;
    float vomitCooldown_ = 0;
    float diarrheaTimer_ = 0;
    bool wasShivering_ = false;
};

} // namespace alone
