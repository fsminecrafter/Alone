#pragma once
#include "survival/Body.h"
#include <string>
#include <vector>

namespace alone {

enum class SymptomColour { Warm, Cold, Neutral };

struct SymptomDef {
    std::string id;
    std::string signal;
    float thresholds[4] = {0.2f, 0.4f, 0.6f, 0.8f};
    std::string names[4];
    std::string descriptions[4];
    SymptomColour colour = SymptomColour::Warm;
};

struct ActiveSymptom {
    const SymptomDef* def = nullptr;
    int level = 0;          // 1..4
    float intensity = 0;    // raw signal 0..1
    const std::string& name() const { return def->names[level - 1]; }
    const std::string& description() const { return def->descriptions[level - 1]; }
};

// Turns what the body feels into the vague, cause-free symptoms the player sees.
class Symptoms {
public:
    bool load(const std::string& path);
    const std::vector<SymptomDef>& defs() const { return defs_; }

    // Most severe first.
    std::vector<ActiveSymptom> evaluate(const BodySignals& s) const;

    static float signalValue(const BodySignals& s, const std::string& name);

private:
    std::vector<SymptomDef> defs_;
};

} // namespace alone
