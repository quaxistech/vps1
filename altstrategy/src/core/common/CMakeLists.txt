add_library(altstrategy_common STATIC
    src/shutdown.cpp
    src/hash.cpp
    src/readiness.cpp)

target_include_directories(altstrategy_common PUBLIC include)
target_link_libraries(altstrategy_common
    PUBLIC Threads::Threads
    PRIVATE OpenSSL::Crypto PkgConfig::SYSTEMD)
