add_executable(altstrategy-version src/main.cpp)
target_compile_definitions(altstrategy-version PRIVATE
    ALTSTRATEGY_VERSION="${PROJECT_VERSION}"
    ALTSTRATEGY_COMMIT="${ALTSTRATEGY_GIT_COMMIT}")
