// altstrategy-version: prints application version, git commit and build metadata.

#include <iostream>

#ifndef ALTSTRATEGY_VERSION
#define ALTSTRATEGY_VERSION "0.0.0"
#endif
#ifndef ALTSTRATEGY_COMMIT
#define ALTSTRATEGY_COMMIT "unknown"
#endif

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--short") {
        std::cout << ALTSTRATEGY_VERSION << "\n";
        return 0;
    }
    std::cout << "altstrategy " << ALTSTRATEGY_VERSION << " (" << ALTSTRATEGY_COMMIT << ")\n"
              << "built: " << __DATE__ << " " << __TIME__ << "\n"
              << "compiler: " <<
#if defined(__clang__)
        "clang " << __clang_major__ << "." << __clang_minor__
#elif defined(__GNUC__)
        "gcc " << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__
#else
        "unknown"
#endif
              << "\n";
    return 0;
}
