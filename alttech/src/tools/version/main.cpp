// Reports the installed AltTech build identity.

#include <iostream>

#ifndef ALTTECH_VERSION
#define ALTTECH_VERSION "0.0.0"
#endif
#ifndef ALTTECH_REVISION
#define ALTTECH_REVISION "source-tree"
#endif

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--short") {
        std::cout << ALTTECH_VERSION << "\n";
        return 0;
    }
    std::cout << "alttech " << ALTTECH_VERSION << " (" << ALTTECH_REVISION << ")\n"
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
