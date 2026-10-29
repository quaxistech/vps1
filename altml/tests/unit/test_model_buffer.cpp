#include "altml/core/verified_artifact.hpp"
#include <LightGBM/c_api.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace altml;
namespace {
std::string toy(double value) {
    const std::string tree = "Tree=0\nnum_leaves=1\nnum_cat=0\nleaf_value=" + std::to_string(value) +
        "\nleaf_weight=1\nleaf_count=1\nis_linear=0\nshrinkage=1\n\n";
    return "tree\nversion=v4\nnum_class=1\nnum_tree_per_iteration=1\nlabel_index=0\nmax_feature_idx=0\n"
           "objective=regression\nfeature_names=x\nfeature_infos=[0:1]\ntree_sizes=" +
           std::to_string(tree.size()) + "\n\n" + tree + "end of trees\n";
}
double predict(const std::string& source, bool file) {
    BoosterHandle booster=nullptr; int iterations=0;
    const int result=file ? LGBM_BoosterCreateFromModelfile(source.c_str(),&iterations,&booster) :
                            LGBM_BoosterLoadModelFromString(source.c_str(),&iterations,&booster);
    REQUIRE(result == 0);
    struct Free { BoosterHandle b; ~Free() { LGBM_BoosterFree(b); } } guard{booster};
    double input=.5, output=0; int64_t length=0;
    REQUIRE(LGBM_BoosterPredictForMat(booster,&input,C_API_DTYPE_FLOAT64,1,1,1,
        C_API_PREDICT_NORMAL,0,-1,"num_threads=1",&length,&output) == 0);
    REQUIRE(length == 1);
    return output;
}
}
TEST_CASE("LightGBM loads the verified bytes across pathname replacement") {
    char directory[]="/tmp/altml-model-buffer-XXXXXX"; REQUIRE(mkdtemp(directory));
    struct Cleanup { std::filesystem::path p; ~Cleanup(){std::filesystem::remove_all(p);} } cleanup{directory};
    const auto path=cleanup.p/"model.txt";
    const auto write=[&](const std::string& bytes){std::ofstream file(path);file<<bytes;};
    const auto original=toy(.25); write(original);
    const auto verified=verified_model_bytes(path.string(),sha256_hex(original)); REQUIRE(verified);
    CHECK(predict(path.string(),true) == Catch::Approx(predict(*verified,false)));
    write(toy(.75));
    CHECK(predict(*verified,false) == Catch::Approx(.25));
    CHECK(predict(path.string(),true) == Catch::Approx(.75));
    CHECK_FALSE(verified_model_bytes(path.string(),sha256_hex(original)));
    const std::string malformed = "not a LightGBM model";
    write(malformed);
    const auto verified_bad = verified_model_bytes(path.string(),sha256_hex(malformed));
    REQUIRE(verified_bad);
    BoosterHandle invalid=nullptr; int iterations=0;
    CHECK(LGBM_BoosterLoadModelFromString(verified_bad->c_str(),&iterations,&invalid) != 0);
    if (invalid != nullptr) LGBM_BoosterFree(invalid);
}
