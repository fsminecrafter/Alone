// Alone - an isometric survival game set in Alingsås, Sweden.
#include "client/App.h"

#ifdef _WIN32
// Laptops with two GPUs: ask the NVIDIA / AMD drivers for the fast one
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

int main(int argc, char** argv) {
    alone::App app;
    return app.run(argc, argv);
}
