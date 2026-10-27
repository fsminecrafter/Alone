#include "core/Log.h"
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace alone::Log {

static FILE* g_file = nullptr;
static std::mutex g_mutex;

void init(const std::string& file) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = fopen(file.c_str(), "w");
}

static void write(const char* level, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(g_mutex);
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, args);
    printf("[%s] %s\n", level, buf);
    fflush(stdout);
    if (g_file) {
        fprintf(g_file, "[%s] %s\n", level, buf);
        fflush(g_file);
    }
}

void info(const char* fmt, ...)  { va_list a; va_start(a, fmt); write("info", fmt, a);  va_end(a); }
void warn(const char* fmt, ...)  { va_list a; va_start(a, fmt); write("warn", fmt, a);  va_end(a); }
void error(const char* fmt, ...) { va_list a; va_start(a, fmt); write("error", fmt, a); va_end(a); }

} // namespace alone::Log
