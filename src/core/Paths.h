#pragma once
#include <string>

namespace alone {

// Locates the game's data folder. Checks, in order: an explicit override,
// "data" next to the executable, then the working directory and its parents.
namespace Paths {
void init(const char* argv0, const std::string& overrideDir = "");
const std::string& dataDir();
std::string data(const std::string& relative);   // dataDir()/relative
bool exists(const std::string& path);
}

} // namespace alone
