#include "application/DaveApp.h"

#include <cstdio>

int main() {
    dave::application::DaveApp app;
    if (!app.init()) {
        std::fprintf(stderr, "Dave: initialization failed\n");
        return 1;
    }
    app.run();
    return 0;
}
