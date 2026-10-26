// Actual InferenceComponent, private database/broker, hand-authored constant
// models. No trainer, registry promotion, production endpoint or real signal.
#include "altml/components/inference.hpp"
#include "altml/components/inference_logic.hpp"
#include "altml/components/feature_recipe.hpp"
#include "altml/components/trainer_logic.hpp"
#include "altml/components/econ_logic.hpp"
#include "altml/core/hash.hpp"
#include "altml/core/logger.hpp"
#include "altml/core/shutdown.hpp"
#include "altml/core/time.hpp"
#include "alttech/system.pb.h"
#include <pqxx/pqxx>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace altml;
namespace {
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
std::string text(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary); std::ostringstream out; out<<input.rdbuf();
    require(bool(input),"fixture file unavailable"); return out.str();
}
void write(const std::filesystem::path& path,const std::string& bytes) {
    std::ofstream out(path,std::ios::binary); out<<bytes; require(bool(out),"fixture write failed");
}
std::string toy(int width,bool classifier) {
    std::string trees,sizes,names,info;
    for(int i=0;i<width;++i) { names+=(i?" ":"")+std::string("x")+std::to_string(i); info+=(i?" ":"")+std::string("[0:1]"); }
    for(int i=0;i<(classifier?3:1);++i) {
        const auto tree="Tree="+std::to_string(i)+"\nnum_leaves=1\nnum_cat=0\nleaf_value="+
            std::to_string(classifier ? .25*i : .5)+"\nleaf_weight=1\nleaf_count=1\nis_linear=0\nshrinkage=1\n\n";
        trees+=tree; sizes+=(i?" ":"")+std::to_string(tree.size());
    }
    return "tree\nversion=v4\nnum_class="+std::string(classifier?"3":"1")+
        "\nnum_tree_per_iteration="+std::string(classifier?"3":"1")+
        "\nlabel_index=0\nmax_feature_idx="+std::to_string(width-1)+
        "\nobjective="+std::string(classifier?"multiclass num_class:3":"regression")+
        "\nfeature_names="+names+"\nfeature_infos="+info+"\ntree_sizes="+sizes+"\n\n"+trees+"end of trees\n";
}
void seed(pqxx::connection& db,const std::filesystem::path& dir,bool model) {
    pqxx::work tx(db);
    tx.exec("DROP SCHEMA IF EXISTS altml CASCADE; CREATE SCHEMA altml");
    const auto schema=text(std::string(ALTML_SOURCE_DIR)+"/db/schema.sql");
    const auto start=schema.find("CREATE TABLE IF NOT EXISTS altml.feature_vectors (");
    const auto suffix=") PARTITION BY RANGE (bar_open_time);";
    const auto end=schema.find(suffix,start);
    require(start!=std::string::npos && end!=std::string::npos,"archive DDL unavailable");
    tx.exec(schema.substr(start,end+std::string(suffix).size()-start));
    tx.exec("CREATE TABLE altml.fixture_partition PARTITION OF altml.feature_vectors DEFAULT");
    const auto prediction_start=schema.find("CREATE TABLE IF NOT EXISTS altml.model_predictions (");
    const auto prediction_end=schema.find("CREATE TABLE IF NOT EXISTS altml.model_events (",prediction_start);
    require(prediction_start!=std::string::npos && prediction_end!=std::string::npos,"prediction DDL unavailable");
    tx.exec(schema.substr(prediction_start,prediction_end-prediction_start));
    tx.exec("CREATE TABLE altml.fixture_predictions PARTITION OF altml.model_predictions DEFAULT");
    tx.exec("CREATE TABLE altml.models(model_id text,model_version int,objective text,status text,algorithm text);"
            "CREATE TABLE altml.model_artifacts(model_id text,kind text,path text,checksum text);"
            "CREATE TABLE altml.model_feature_compatibility(model_id text,schema_hash text,feature_count int,"
            "base_feature_count int,aligned_sources text[],base_projection text[],recipe_version text,"
            "feature_names_sha256 text,calibration jsonb);"
            "CREATE TABLE altml.fdw_feature_set_versions(id bigint,name text,version int,schema_hash text,frozen bool);"
            "CREATE TABLE altml.fdw_feature_set_members(feature_set_version_id bigint,position int,output_name text);");
    tx.exec("INSERT INTO altml.fdw_feature_set_versions VALUES(1,'talib-core',1,'fixture-schema',true)");
    std::vector<std::string> layout;
    for(const auto* name:recipe::DerivedRecipe::input_names()) layout.emplace_back(name);
    layout.emplace_back("RSI_14_outReal");
    for(size_t i=0;i<layout.size();++i)
        tx.exec_params("INSERT INTO altml.fdw_feature_set_members VALUES(1,$1,$2)",i,layout[i]);
    if(model) {
        std::vector<std::string> names; recipe::append_base_names({"RSI_14_outReal"},names);
        const int width=static_cast<int>(names.size());
        const nlohmann::json calibration={{"temperature",1.0},{"horizon_minutes",15},
            {"calibrator",econ::kCalibratorVersion},{"round_trip_cost_bps",16},
            {"class_means_return",{-.01,0,.01}},{"class_means_mfe_long",{.01,.01,.02}},
            {"class_means_mae_long",{-.02,-.01,-.01}},{"magnitude_source","regression"}};
        tx.exec("INSERT INTO altml.models VALUES('fixture-only',1,'multiclass:econ_class_15m','ACTIVE','lightgbm')");
        tx.exec_params("INSERT INTO altml.model_feature_compatibility VALUES('fixture-only','fixture-schema',"
                       "$1,$1,'{}','{RSI_14_outReal}',$2,$3,$4::jsonb)",width,
                       std::string(recipe::kRecipeVersion),sha256_hex(recipe::joined_names(names)),calibration.dump());
        for(const auto* kind:{"model","model_up","model_down"}) {
            const auto bytes=toy(width,std::string_view(kind)=="model");
            const auto path=dir/(std::string(kind)+".txt"); write(path,bytes);
            tx.exec_params("INSERT INTO altml.model_artifacts VALUES('fixture-only',$1,$2,$3)",
                           kind,path.string(),sha256_hex(bytes));
        }
    }
    tx.commit();
    alttech::v1::Envelope input;
    input.set_schema_version(1); input.set_exchange(alttech::v1::EXCHANGE_BITGET);
    input.set_instrument_id(42); input.set_canonical_pair_id(42); input.set_exchange_symbol("FIXTURE");
    auto* fv=input.mutable_feature_vector(); fv->set_timeframe(alttech::v1::TF_1M);
    fv->set_bar_open_time_ms((timeutil::now_ms()/60'000-1)*60'000);
    fv->set_computed_at_ms(timeutil::now_ms()); fv->set_status(alttech::v1::FEATURE_STATUS_VALID);
    fv->set_feature_set_name(std::string(analytics::kCoreFeatureSetName));
    fv->set_feature_set_version(analytics::kCoreFeatureSetVersion);
    fv->set_schema_hash("fixture-schema"); fv->set_engine_version(std::string(analytics::kFeatureEngineVersion));
    for(size_t i=0;i<layout.size();++i) fv->add_values(100.f);
    fv->set_validity_bitmap(std::string((layout.size()+7)/8,'\xff'));
    write(dir/"original.bin",input.SerializeAsString());
    fv->set_bar_revision(1); fv->set_values(0,200.f);
    write(dir/"revision.bin",input.SerializeAsString());
}
void stream(messaging::NatsClient& client,const char* name,const char* subject) {
    jsStreamConfig config; jsStreamConfig_Init(&config); config.Name=name;
    const char* subjects[]={subject}; config.Subjects=subjects; config.SubjectsLen=1;
    config.Storage=js_FileStorage; config.MaxBytes=8*1024*1024;
    jsStreamInfo* info=nullptr; jsErrCode error;
    require(js_AddStream(&info,client.js(),&config,nullptr,&error)==NATS_OK,"fixture stream failed");
    jsStreamInfo_Destroy(info);
}
struct Consumer { uint64_t floor=0,delivered=0; int64_t pending=0,redelivered=0; };
Consumer consumer(messaging::NatsClient& client) {
    jsConsumerInfo* info=nullptr; jsErrCode error;
    require(js_GetConsumerInfo(&info,client.js(),"AUDIT_FEATURES","audit-input",nullptr,&error)==NATS_OK,"consumer info failed");
    Consumer out{info->AckFloor.Stream,info->Delivered.Consumer,info->NumAckPending,info->NumRedelivered};
    jsConsumerInfo_Destroy(info); return out;
}
uint64_t forecasts(messaging::NatsClient& client) {
    jsStreamInfo* info=nullptr; jsErrCode error;
    require(js_GetStreamInfo(&info,client.js(),"AUDIT_PREDICTIONS",nullptr,&error)==NATS_OK,"forecast info failed");
    const auto count=info->State.Msgs; jsStreamInfo_Destroy(info); return count;
}
}
int main(int argc,char** argv) {
    try {
        require(argc==6,"arguments required");
        const std::string dsn=argv[1],url=argv[2],phase=argv[3];
        const bool model=std::string_view(argv[4])=="model";
        const std::filesystem::path dir=argv[5];
        require(url.starts_with("nats://127.0.0.1:"),"local broker required");
        pqxx::connection db(dsn);
        require(std::string(db.dbname()).starts_with("altml_audit_test_") &&
                std::string(db.hostname()).starts_with("/"),"private test database required");
        auto connected=messaging::NatsClient::connect(url,"archive-consumer-fixture");
        require(bool(connected),"local broker unavailable"); auto& client=**connected;
        log::init("altml-test","fixture","info");
        if(phase=="fail") {
            seed(db,dir,model);
            stream(client,"AUDIT_FEATURES","alttech.feature.>");
            stream(client,"AUDIT_PREDICTIONS","prediction.fixture.>");
            jsConsumerConfig config; jsConsumerConfig_Init(&config);
            config.Durable="audit-input"; config.FilterSubject="alttech.feature.>";
            config.AckPolicy=js_AckExplicit; config.AckWait=60'000'000'000;
            config.MaxAckPending=4096; config.MaxDeliver=-1;
            jsConsumerInfo* info=nullptr; jsErrCode error;
            require(js_AddConsumer(&info,client.js(),"AUDIT_FEATURES",&config,nullptr,&error)==NATS_OK,"fixture durable failed");
            jsConsumerInfo_Destroy(info);
        } else {
            pqxx::work tx(db); tx.exec(text(std::string(ALTML_SOURCE_DIR)+"/db/migrations/0001_original_live_payload.sql")); tx.commit();
        }
        config::RuntimeConfig runtime; runtime.common.postgres_dsn=dsn;
        runtime.project.input_stream="AUDIT_FEATURES"; runtime.project.durable="audit-input";
        runtime.project.model_root=dir.string(); runtime.project.prediction_subject_prefix="prediction.fixture";
        runtime.root=YAML::Load("inference: {serve_shadow_models: false, min_backfill_free_disk_gb: 1000}");
        InferenceComponent component(runtime,client);
        const auto ready=component.init(); require(bool(ready),"actual inference init failed");
        std::thread worker([&]{component.run();});
        struct Join { std::thread& worker; ~Join(){ShutdownToken::instance().request_stop();worker.join();} } join{worker};
        const auto original=text(dir/"original.bin"),revision=text(dir/"revision.bin");
        if(phase=="fail") {
            require(bool(client.js_publish("alttech.feature.fixture",original,"original")),"source publish failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(1600));
            require(bool(client.js_publish("alttech.feature.fixture",revision,"revision")),"revision publish failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(1600));
            const auto state=consumer(client);
            require(state.floor==0 && state.pending==2 && state.delivered>=4 && state.delivered<=10,
                    "archive failure ACKed input or retry was not bounded");
            require(forecasts(client)==0,"forecast escaped failed durability");
        } else {
            bool drained=false;
            for(int i=0;i<100;++i) {
                const auto state=consumer(client);
                if(state.floor==2 && state.pending==0) {drained=true;break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            require(drained,"restart did not recover both source revisions");
            require(bool(client.js_publish("alttech.feature.fixture",original,"forced-redelivery")),"duplicate publish failed");
            for(int i=0;i<50 && consumer(client).floor!=3;++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            require(consumer(client).floor==3 && consumer(client).pending==0,"duplicate was not acknowledged");
            pqxx::read_transaction tx(db);
            const auto rows=tx.exec("SELECT bar_revision,original_live_payload FROM altml.feature_vectors ORDER BY bar_revision");
            require(rows.size()==2,"unexpected archive row count");
            for(size_t i=0;i<2;++i) {
                const pqxx::binarystring bytes(rows[i][1]);
                require(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size())==(i?revision:original),"original live vector lost");
            }
            require(forecasts(client)==(model?2:0),"forecast loss or duplicate publication");
            require(tx.exec("SELECT count(*) FROM altml.model_predictions")[0][0].as<int>()==(model?2:0),
                    "prediction persistence lost or duplicated");
        }
        std::cout<<"PASS actual consumer "<<phase<<" "<<(model?"classifier+two magnitudes":"no model")<<std::endl;
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
