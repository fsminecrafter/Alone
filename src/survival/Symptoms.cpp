#include "survival/Symptoms.h"
#include "core/Config.h"
#include "core/Log.h"
#include <algorithm>
#include <cstdlib>

namespace alone {

float Symptoms::signalValue(const BodySignals& s, const std::string& n) {
    struct Entry { const char* name; float BodySignals::*member; };
    static const Entry table[] = {
        {"hunger", &BodySignals::hunger}, {"thirst", &BodySignals::thirst}, {"full", &BodySignals::full},
        {"craving", &BodySignals::craving}, {"nausea", &BodySignals::nausea}, {"cramps", &BodySignals::cramps},
        {"weakness", &BodySignals::weakness}, {"headache", &BodySignals::headache},
        {"dizziness", &BodySignals::dizziness}, {"confusion", &BodySignals::confusion},
        {"cold", &BodySignals::cold}, {"hot", &BodySignals::hot}, {"fever", &BodySignals::fever},
        {"tired", &BodySignals::tired}, {"pain", &BodySignals::pain}, {"mouthSore", &BodySignals::mouthSore},
        {"aches", &BodySignals::aches}, {"hallucination", &BodySignals::hallucination},
        {"bleeding", &BodySignals::bleeding}, {"skin", &BodySignals::skin},
        {"swollenGlands", &BodySignals::swollenGlands},
    };
    for (const auto& e : table)
        if (n == e.name) return s.*(e.member);
    return 0;
}

bool Symptoms::load(const std::string& path) {
    Config cfg;
    if (!cfg.load(path)) return false;
    defs_.clear();
    for (const auto& sec : cfg.sections()) {
        if (sec.name.empty()) continue;
        SymptomDef d;
        d.id = sec.name;
        d.signal = sec.str("signal", sec.name);
        auto th = sec.list("thresholds");
        for (int i = 0; i < 4 && i < (int)th.size(); ++i) d.thresholds[i] = (float)std::atof(th[i].c_str());
        for (int i = 0; i < 4; ++i) {
            d.names[i] = sec.str("name" + std::to_string(i + 1), d.id);
            d.descriptions[i] = sec.str("desc" + std::to_string(i + 1), "");
        }
        std::string c = sec.str("colour", "warm");
        d.colour = c == "cold" ? SymptomColour::Cold : c == "neutral" ? SymptomColour::Neutral : SymptomColour::Warm;
        defs_.push_back(d);
    }
    Log::info("Loaded %d symptoms", (int)defs_.size());
    return true;
}

std::vector<ActiveSymptom> Symptoms::evaluate(const BodySignals& s) const {
    std::vector<ActiveSymptom> out;
    for (const auto& d : defs_) {
        float v = signalValue(s, d.signal);
        int level = 0;
        for (int i = 0; i < 4; ++i)
            if (v >= d.thresholds[i]) level = i + 1;
        if (level > 0) out.push_back({&d, level, v});
    }
    std::stable_sort(out.begin(), out.end(), [](const ActiveSymptom& a, const ActiveSymptom& b) { return a.level > b.level; });
    return out;
}

} // namespace alone
