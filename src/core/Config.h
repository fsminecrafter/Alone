#pragma once
#include <map>
#include <string>
#include <vector>

namespace alone {

// INI-style data files:
//
//   # comment
//   [section.name]
//   key = value
//
// Sections keep file order. Used for items, loot tables, maps and settings.
class Config {
public:
    struct Section {
        std::string name;
        std::map<std::string, std::string> values;
        std::vector<std::string> keyOrder;

        bool has(const std::string& key) const { return values.count(key) != 0; }
        std::string str(const std::string& key, const std::string& def = "") const;
        float num(const std::string& key, float def = 0) const;
        int integer(const std::string& key, int def = 0) const;
        bool flag(const std::string& key, bool def = false) const;
        std::vector<std::string> list(const std::string& key) const;   // comma separated
    };

    bool load(const std::string& path);
    bool parse(const std::string& text, const std::string& sourceName = "<memory>");
    bool save(const std::string& path) const;

    const std::vector<Section>& sections() const { return sections_; }
    const Section* find(const std::string& name) const;
    Section& getOrAdd(const std::string& name);

private:
    std::vector<Section> sections_;
};

std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
bool readTextFile(const std::string& path, std::string& out);

} // namespace alone
