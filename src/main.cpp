#include "app/Application.h"
#include <cstdio>

int main(int argc, char* argv[]) {
    // Redirect stderr to log file for debugging
    FILE* logFile = freopen("hrtf_log.txt", "w", stderr);
    if (logFile) {
        setvbuf(stderr, nullptr, _IONBF, 0); // unbuffered
        fprintf(stderr, "=== HRTF Spatializer Log ===\n");
    }

    Application app;

    if (!app.init(argc, argv)) {
        fprintf(stderr, "Failed to initialize application\n");
        return 1;
    }

    app.run();
    app.shutdown();

    fprintf(stderr, "=== Clean shutdown ===\n");
    return 0;
}
