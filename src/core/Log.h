#pragma once
#include <string>

namespace alone {

// Minimal logger: writes to stdout and to alone.log next to the executable.
namespace Log {
void init(const std::string& file);
void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);
}

} // namespace alone
