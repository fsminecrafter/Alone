#include "core/Config.h"
#include "core/Log.h"
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace alone {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    if (!trim(cur).empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string Config::Section::str(const std::string& key, const std::string& def) const {
    auto it = values.find(key);
    return it == values.end() ? def : it->second;
}
float Config::Section::num(const std::string& key, float def) const {
    auto it = values.find(key);
    return it == values.end() ? def : (float)std::atof(it->second.c_str());
}
int Config::Section::integer(const std::string& key, int def) const {
    auto it = values.find(key);
    return it == values.end() ? def : std::atoi(it->second.c_str());
}
bool Config::Section::flag(const std::string& key, bool def) const {
    auto it = values.find(key);
    if (it == values.end()) return def;
    const std::string& v = it->second;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}
std::vector<std::string> Config::Section::list(const std::string& key) const {
    auto it = values.find(key);
    if (it == values.end() || it->second.empty()) return {};
    return split(it->second, ',');
}

bool Config::load(const std::string& path) {
    std::string text;
    if (!readTextFile(path, text)) {
        Log::warn("Could not read %s", path.c_str());
        return false;
    }
    return parse(text, path);
}

bool Config::parse(const std::string& text, const std::string& sourceName) {
    std::istringstream in(text);
    std::string line;
    Section* cur = &getOrAdd("");
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[') {
            size_t close = t.find(']');
            if (close == std::string::npos) {
                Log::warn("%s:%d: missing ']'", sourceName.c_str(), lineNo);
                continue;
            }
            cur = &getOrAdd(trim(t.substr(1, close - 1)));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            Log::warn("%s:%d: expected key = value", sourceName.c_str(), lineNo);
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string value = trim(t.substr(eq + 1));
        if (!cur->values.count(key)) cur->keyOrder.push_back(key);
        cur->values[key] = value;
    }
    return true;
}

bool Config::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    for (const auto& s : sections_) {
        if (s.values.empty()) continue;
        if (!s.name.empty()) f << "[" << s.name << "]\n";
        for (const auto& k : s.keyOrder) f << k << " = " << s.values.at(k) << "\n";
        f << "\n";
    }
    return true;
}

const Config::Section* Config::find(const std::string& name) const {
    for (const auto& s : sections_)
        if (s.name == name) return &s;
    return nullptr;
}

Config::Section& Config::getOrAdd(const std::string& name) {
    for (auto& s : sections_)
        if (s.name == name) return s;
    sections_.push_back(Section{name, {}, {}});
    return sections_.back();
}

} // namespace alone
