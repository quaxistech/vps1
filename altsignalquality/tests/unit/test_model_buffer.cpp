#include "altsignalquality/components/model_runtime.hpp"
#include "altsignalquality/core/verified_artifact.hpp"
#include <LightGBM/c_api.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace altsignalquality;
namespace {
std::string toy(size_t width) {
    const std::string tree="Tree=0\nnum_leaves=1\nnum_cat=0\nleaf_value=0.25\nleaf_weight=1\nleaf_count=1\nis_linear=0\nshrinkage=1\n\n";
    std::string names,infos;
    for(size_t i=0;i<width;++i){if(i){names+=' ';infos+=' ';}names+="f"+std::to_string(i);infos+="[0:1]";}
    return "tree\nversion=v4\nnum_class=1\nnum_tree_per_iteration=1\nlabel_index=0\nmax_feature_idx=" +
        std::to_string(width-1) + "\nobjective=regression\nfeature_names="+names+"\nfeature_infos="+infos+
        "\ntree_sizes="+std::to_string(tree.size())+"\n\n"+tree+"end of trees\n";
}
}
TEST_CASE("quality model loading keeps root, checksum and shape checks with owned bytes") {
    char directory[]="/tmp/altsq-model-buffer-XXXXXX"; REQUIRE(mkdtemp(directory));
    struct Cleanup { std::filesystem::path p; ~Cleanup(){std::filesystem::remove_all(p);} } cleanup{directory};
    const auto root=cleanup.p/"models";std::filesystem::create_directory(root);
    const auto path=root/"model.txt";const auto bytes=toy(domain::kFeatureCount);
    {std::ofstream file(path);file<<bytes;}
    data::ActiveModel row;row.model_id="fixture";row.artifact_path=path;
    row.artifact_checksum=sha256_hex(bytes);row.feature_count=static_cast<int32_t>(domain::kFeatureCount);
    row.feature_schema_version=domain::kFeatureSchemaVersion;row.feature_schema_hash=domain::feature_schema_hash();
    auto loaded=LoadedModel::open(row,root);REQUIRE(loaded);
    domain::FeatureVector features;features.values.assign(domain::kFeatureCount,.5f);
    const auto score=(*loaded)->score(features);REQUIRE(score);CHECK(score->raw_score==Catch::Approx(.25));
    auto bad=row;bad.artifact_checksum=std::string(64,'0');CHECK_FALSE(LoadedModel::open(bad,root));
    bad=row;++bad.feature_count;CHECK_FALSE(LoadedModel::open(bad,root));
    bad=row;bad.feature_schema_hash="wrong";CHECK_FALSE(LoadedModel::open(bad,root));
    const auto outside=cleanup.p/"outside.txt";{std::ofstream file(outside);file<<bytes;}
    bad=row;bad.artifact_path=outside;CHECK_FALSE(LoadedModel::open(bad,root));
    std::filesystem::create_symlink(outside,root/"escape");
    bad=row;bad.artifact_path=root/"escape";CHECK_FALSE(LoadedModel::open(bad,root));
    std::filesystem::create_symlink(path,root/"inside");
    bad=row;bad.artifact_path=root/"inside";CHECK(LoadedModel::open(bad,root));
    const auto verified=verified_model_bytes(path.string(),row.artifact_checksum);REQUIRE(verified);
    {std::ofstream replacement(path);replacement<<"replaced after verification";}
    BoosterHandle booster=nullptr;int iterations=0;
    REQUIRE(LGBM_BoosterLoadModelFromString(verified->c_str(),&iterations,&booster)==0);
    LGBM_BoosterFree(booster);
    CHECK_FALSE(LoadedModel::open(row,root));
}
