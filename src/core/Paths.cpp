#include "core/Paths.h"
#include "core/Log.h"
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace alone::Paths {

static std::string g_dataDir = "data";

static bool looksLikeDataDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p / "items", ec);
}

void init(const char* argv0, const std::string& overrideDir) {
    std::error_code ec;
    std::vector<fs::path> candidates;
    if (!overrideDir.empty()) candidates.push_back(overrideDir);
    if (argv0) {
        fs::path exe = fs::absolute(fs::path(argv0), ec).parent_path();
        candidates.push_back(exe / "data");
    }
    fs::path cwd = fs::current_path(ec);
    for (int i = 0; i < 4 && !cwd.empty(); ++i) {
        candidates.push_back(cwd / "data");
        cwd = cwd.parent_path();
    }
    for (const auto& c : candidates) {
        if (looksLikeDataDir(c)) {
            g_dataDir = fs::absolute(c, ec).string();
            Log::info("Data folder: %s", g_dataDir.c_str());
            return;
        }
    }
    Log::error("Could not find the data folder (looked next to the executable and in the working directory)");
}

const std::string& dataDir() { return g_dataDir; }

std::string data(const std::string& relative) {
    return (fs::path(g_dataDir) / fs::u8path(relative)).string();
}

bool exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(fs::u8path(path), ec);
}

} // namespace alone::Paths
