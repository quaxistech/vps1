// Actual evaluator recovery against a private PostgreSQL database and NATS.
// The fixture contains no production signal, model, account or trade.
#include "altsignalquality/components/evaluator.hpp"
#include "altsignalquality/core/hash.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/shutdown.hpp"
#include "altsignalquality/core/time.hpp"
#include "altsignalquality/data/store.hpp"
#include <pqxx/pqxx>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace altsignalquality;
namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary); std::ostringstream contents; contents << file.rdbuf();
    require(bool(file),"fixture file unavailable"); return contents.str();
}
void write(const std::filesystem::path& path,const std::string& bytes) {
    std::ofstream file(path,std::ios::binary); file << bytes; require(bool(file),"fixture write failed");
}
void seed(pqxx::connection& db,const std::string& dsn,const std::filesystem::path& dir,bool scored) {
    {
        pqxx::work tx(db);
        tx.exec("DROP SCHEMA IF EXISTS altsignalquality CASCADE; DROP SCHEMA IF EXISTS altstrategy CASCADE");
        tx.exec(read(std::string(ALTSQ_SOURCE_DIR)+"/db/migrations/0001_altsignalquality_schema.sql"));
        tx.exec(read(std::string(ALTSQ_SOURCE_DIR)+"/db/migrations/0002_publication_evidence.sql"));
        tx.exec("CREATE SCHEMA altstrategy;"
          "CREATE TABLE altstrategy.trade_signal_proposals(signal_proposal_id text,candidate_id text,canonical_pair_id bigint,"
          "strategy_id text,strategy_version text,parameter_set_id text,side text,proposal_grade text,"
          "recommended_for_execution bool,created_at_ms bigint,valid_until_ms bigint,time_stop_at_ms bigint,context_revision bigint);"
          "CREATE TABLE altstrategy.strategy_candidates(candidate_id text,context_id text,context_revision bigint);"
          "CREATE TABLE altstrategy.market_outcomes(signal_proposal_id text,outcome text,realized_net_return double precision,"
          "resolved_time_ms bigint,mfe double precision,mae double precision,holding_time_ms bigint,is_counterfactual bool);"
          "CREATE TABLE altstrategy.decision_context_snapshots(context_id text,event_time_ms bigint,available_at_ms bigint,"
          "data_quality text,bitget_regime text,global_regime text,venue_mask bigint,missing_venue_mask bigint,"
          "regime_divergence bool,snapshot jsonb);"
          "CREATE TABLE altstrategy.proposal_warnings(signal_proposal_id text,ordinal int);");
        tx.commit();
    }
    data::EvaluationRow row;
    row.signal_proposal_id="fixture-proposal"; row.evaluation_id="fixture-evaluation";
    row.final_signal_id="fixture-final"; row.candidate_id="fixture-candidate";
    row.canonical_pair_id=42; row.symbol="FIXTURE"; row.strategy_id="fixture";
    row.strategy_version="1"; row.side="long"; row.proposal_grade="NORMAL";
    row.context_revision=5; row.signal_created_at_ms=timeutil::now_ms();
    row.evaluated_at_ms=row.signal_created_at_ms+1; row.feature_schema_version="fixture";
    row.feature_schema_hash="fixture-hash"; row.features.values.assign(domain::kFeatureCount,.25f);
    row.features.validity.assign((domain::kFeatureCount+7)/8,0xff);
    row.has_quality_score=scored; row.raw_score=row.quality_score=.77;
    row.decision_threshold=.5; row.decision="ACCEPTED"; row.decision_basis=scored?"MODEL":"COLD_START";
    row.model_id=scored?"original-fixture-model":"";
    altstrategy::v1::Envelope candidate; candidate.set_schema_version(1);
    candidate.set_symbol(row.symbol); candidate.set_trace_id("original-trace");
    auto* p=candidate.mutable_trade_signal_proposal(); p->set_signal_proposal_id(row.signal_proposal_id);
    p->set_candidate_id(row.candidate_id); p->set_canonical_pair_id(row.canonical_pair_id);
    p->set_strategy_id(row.strategy_id); p->set_strategy_version(row.strategy_version);
    p->set_side(altstrategy::v1::SIDE_LONG); p->set_proposal_grade(altstrategy::v1::GRADE_NORMAL);
    p->set_created_at_ms(row.signal_created_at_ms); p->set_entry_price(100);
    p->set_stop_loss_price(99); p->set_take_profit_price(102);
    row.candidate_payload=candidate.SerializeAsString(); row.candidate_sha256=sha256_hex(row.candidate_payload);
    altsignalquality::v1::Envelope publication; publication.set_schema_version(1);
    publication.set_message_id(row.final_signal_id); auto* final=publication.mutable_final_trade_signal();
    final->set_final_signal_id(row.final_signal_id); *final->mutable_proposal()=*p;
    auto* eval=final->mutable_quality(); eval->set_evaluation_id(row.evaluation_id);
    eval->set_signal_proposal_id(row.signal_proposal_id); eval->set_evaluated_at_ms(row.evaluated_at_ms);
    eval->set_decision(altsignalquality::v1::DECISION_ACCEPTED);
    eval->set_basis(scored?altsignalquality::v1::BASIS_MODEL:altsignalquality::v1::BASIS_COLD_START);
    eval->set_model_id(row.model_id); eval->set_model_version(scored?17:0);
    eval->set_has_quality_score(scored); eval->set_quality_score(.77);
    eval->add_reason_codes("original-reason"); final->set_published_at_ms(row.evaluated_at_ms+1);
    row.publication_payload=publication.SerializeAsString();
    persistence::PgPool pool(dsn,2); data::Store store(pool,"altstrategy");
    const auto claimed=store.claim_evaluation(row);
    require(bool(claimed) && !claimed->has_value(),"first claim failed");
    write(dir/"candidate.bin",row.candidate_payload); write(dir/"publication.bin",row.publication_payload);
}
void stream(messaging::NatsClient& client,const char* name,const char* subject) {
    jsStreamConfig cfg; jsStreamConfig_Init(&cfg); cfg.Name=name;
    const char* subjects[]={subject}; cfg.Subjects=subjects; cfg.SubjectsLen=1;
    cfg.Storage=js_FileStorage; cfg.MaxBytes=8*1024*1024;
    jsStreamInfo* info=nullptr; jsErrCode error;
    require(js_AddStream(&info,client.js(),&cfg,nullptr,&error)==NATS_OK,"fixture stream failed");
    jsStreamInfo_Destroy(info);
}
struct Consumer { uint64_t floor=0,delivered=0; int64_t pending=0; };
Consumer consumer(messaging::NatsClient& client) {
    jsConsumerInfo* info=nullptr; jsErrCode error;
    require(js_GetConsumerInfo(&info,client.js(),"AUDIT_CANDIDATES","audit-quality",nullptr,&error)==NATS_OK,"fixture consumer unavailable");
    Consumer out{info->AckFloor.Stream,info->Delivered.Consumer,info->NumAckPending};
    jsConsumerInfo_Destroy(info); return out;
}
void check_publication(messaging::NatsClient& client,pqxx::connection& db,const std::string& expected) {
    jsStreamInfo* info=nullptr; jsErrCode error;
    require(js_GetStreamInfo(&info,client.js(),"AUDIT_FINAL",nullptr,&error)==NATS_OK,"final stream unavailable");
    const auto count=info->State.Msgs; jsStreamInfo_Destroy(info);
    require(count==1,"duplicate or missing final publication");
    natsMsg* message=nullptr;
    require(js_GetLastMsg(&message,client.js(),"AUDIT_FINAL","signalquality.final.FIXTURE.v1",nullptr,&error)==NATS_OK,"final message unavailable");
    const std::string bytes(natsMsg_GetData(message),static_cast<size_t>(natsMsg_GetDataLength(message)));
    natsMsg_Destroy(message); require(bytes==expected,"published bytes differ from first claim");
    pqxx::read_transaction tx(db);
    const auto row=tx.exec("SELECT published,publication_payload FROM altsignalquality.signal_evaluations WHERE signal_proposal_id='fixture-proposal'");
    require(row.size()==1 && row[0][0].as<bool>(),"first claim not marked published");
    const pqxx::binarystring persisted(row[0][1]);
    require(std::string(reinterpret_cast<const char*>(persisted.data()),persisted.size())==expected,"persisted publication changed");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==7,"arguments required");
        const std::string dsn=argv[1],url=argv[2],phase=argv[3],mode=argv[4];
        const bool scored=std::string_view(argv[5])=="scored";
        const std::filesystem::path dir=argv[6];
        require(url.starts_with("nats://127.0.0.1:"),"private broker required");
        pqxx::connection db(dsn);
        require(std::string(db.dbname()).starts_with("altsq_audit_test_") &&
                std::string(db.hostname()).starts_with("/"),"private test database required");
        auto connected=messaging::NatsClient::connect(url,"quality-recovery-fixture");
        require(bool(connected),"private broker unavailable"); auto& client=**connected;
        log::init("altsq-test","fixture","info");
        if(phase=="fail") {
            seed(db,dsn,dir,scored); stream(client,"AUDIT_CANDIDATES","signal.proposed.>");
            jsConsumerConfig cfg; jsConsumerConfig_Init(&cfg); cfg.Durable="audit-quality";
            cfg.FilterSubject="signal.proposed.>"; cfg.AckPolicy=js_AckExplicit;
            cfg.AckWait=60'000'000'000; cfg.MaxDeliver=-1; cfg.MaxAckPending=64;
            jsConsumerInfo* info=nullptr; jsErrCode error;
            require(js_AddConsumer(&info,client.js(),"AUDIT_CANDIDATES",&cfg,nullptr,&error)==NATS_OK,"fixture durable failed");
            jsConsumerInfo_Destroy(info);
        } else stream(client,"AUDIT_FINAL","signalquality.final.>");
        config::RuntimeConfig runtime; runtime.common.postgres_dsn=dsn;
        runtime.wiring.candidate_stream="AUDIT_CANDIDATES"; runtime.wiring.candidate_durable="audit-quality";
        runtime.wiring.model_root=dir.string();
        runtime.root=YAML::Load("evaluator: {fetch_timeout_ms: 100, fetch_batch: 1, publish_evaluations: false}\npolicy: {cold_start: REJECT_ALL}");
        EvaluatorComponent evaluator(runtime,client);
        const auto ready=evaluator.init(); require(bool(ready),"actual evaluator init failed");
        std::thread worker([&]{evaluator.run();});
        struct Join {std::thread& worker; ~Join(){ShutdownToken::instance().request_stop();worker.join();}} join{worker};
        const auto candidate=read(dir/"candidate.bin"), publication=read(dir/"publication.bin");
        if(phase=="fail") {
            require(bool(client.js_publish("signal.proposed.FIXTURE.v1",candidate,"fixture-input")),"candidate publish failed");
            bool pending=false;
            for(int i=0;i<300;++i) {
                const auto state=consumer(client);
                if(state.delivered>=2 && state.floor==0 && state.pending==1) {pending=true;break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            require(pending,"failed publication did not NAK and redeliver the input");
            // Let the second failed handler send its delayed NAK before the
            // restart path stops the component.
            std::this_thread::sleep_for(std::chrono::milliseconds(750));
            pqxx::read_transaction tx(db);
            require(!tx.exec("SELECT published FROM altsignalquality.signal_evaluations WHERE signal_proposal_id='fixture-proposal'")[0][0].as<bool>(),"failed publication was marked successful");
            if(mode=="restart") { std::cout<<"PASS failed publication before restart\n"; return 0; }
            stream(client,"AUDIT_FINAL","signalquality.final.>");
        }
        bool done=false;
        for(int i=0;i<300;++i) {
            const auto state=consumer(client);
            if(state.floor==1 && state.pending==0) {done=true;break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        require(done,"retry did not acknowledge the original candidate");
        check_publication(client,db,publication);
        altstrategy::v1::Envelope conflicting;
        require(conflicting.ParseFromString(candidate),"invalid fixture candidate");
        conflicting.mutable_trade_signal_proposal()->set_entry_price(101);
        require(bool(client.js_publish("signal.proposed.FIXTURE.v1",conflicting.SerializeAsString(),"fixture-conflict")),"conflict publish failed");
        for(int i=0;i<100 && consumer(client).floor<2;++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(consumer(client).floor==2,"conflicting input was not terminally handled");
        check_publication(client,db,publication);
        std::cout<<"PASS actual evaluator "<<mode<<" "<<(scored?"model":"cold-start")<<std::endl;
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
