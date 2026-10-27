#include "TestFramework.h"
#include "core/Log.h"
#include "core/Paths.h"
#include <cstdio>

int main(int argc, char** argv) {
    alone::Paths::init(nullptr, ALONE_DATA_DIR);
    int ran = 0;
    for (auto& c : testing::registry()) {
        if (argc > 1 && std::string(argv[1]) != c.name) continue;
        int before = testing::failures();
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        std::printf("[ %s ] %s\n", testing::failures() == before ? " OK " : "FAIL", c.name);
        ++ran;
    }
    std::printf("\n%d tests, %d failed checks\n", ran, testing::failures());
    return testing::failures() == 0 ? 0 : 1;
}
