// altspotter version: prints application version and build metadata.

#include "ops/ops.hpp"

#include <iostream>
#include <string_view>

#ifndef ALTSPOTTER_VERSION
#define ALTSPOTTER_VERSION "0.0.0"
#endif

namespace altspotter::ops {

int cmd_version(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--short") {
        std::cout << ALTSPOTTER_VERSION << "\n";
        return 0;
    }
    // No build timestamp: with incremental builds __DATE__/__TIME__ reflect
    // when this one file was compiled, not when the binary was produced, which
    // is worse than no information at all.
    std::cout << "altspotter " << ALTSPOTTER_VERSION << "\n"
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

}  // namespace altspotter::ops
