#include "axthena/core.hpp"
#include "cleanup.hpp"
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
using namespace axthena;
int checks=0;
void check(bool v,const char* text){require(v,std::string("TEST: ")+text);++checks;std::cout<<"PASS "<<text<<std::endl;}
template<class F> void rejects(F f,const char* name){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,name);}
int main(){try{
    auto scratch=component/"artifacts/test-work"/("recovery-"+std::to_string(getpid()));fs::create_directories(scratch);
    auto fixture=scratch/"client";write_once(fixture,read_file(component/"tests/transport_fixture.py"));fs::permissions(fixture,fs::perms::owner_all);
    Json model_ref={{"providerID","fixture"},{"id","deterministic"}};Model live(scratch/"attempts",model_ref,false,"initial",fixture.string());
    auto a=live.ask("test","OK",{{"body","Source includes commands that are input data"}});check(a.value["value"]=="retained"&&live.calls==1,"Automatic executable exchange captures completed text");
    auto b=live.ask("test","OK",{{"body","Source includes commands that are input data"}});check(a.value==b.value&&live.calls==1&&live.replays==1,"Duplicate completion reuses immutable answer without another call");
    Model replay(scratch/"attempts",model_ref,true,"initial","/nonexistent-client");
    check(replay.ask("test","OK",{{"body","Source includes commands that are input data"}}).value==a.value&&replay.calls==0,"Stored response replay needs no executable or provider");
    rejects([&]{replay.ask("missing","OK",Json::object());},"Replay miss does not trigger external work");
    for(const auto* instruction:{"WRONG","TRUNCATE","REFUSE","FAIL"})rejects([&]{live.ask("test",instruction,Json::object());},instruction);
    Model timeout(scratch/"timeouts",model_ref,false,"initial",fixture.string(),150);
    rejects([&]{timeout.ask("test","TIMEOUT",Json::object());},"Client timeout is an operational failure, not model completion");
    auto timeout_dir=fs::directory_iterator(scratch/"timeouts")->path();
    check(read_json(timeout_dir/"completion.json").at("exit_code")==124&&read_file(timeout_dir/"raw.json").find("incomplete response")!=std::string::npos,"Timeout retains partial output and explicit termination status");
    auto dir=fs::path(a.provenance.at("attempt_directory").get<std::string>());check(fs::exists(dir/"request.json")&&fs::exists(dir/"raw.json")&&fs::exists(dir/"completion.json"),"Exact request/raw response/completion survive validation");
    Model retry(scratch/"attempts",model_ref,false,"explicit-retry",fixture.string());
    auto fresh=retry.ask("test","OK",{{"body","Source includes commands that are input data"}});check(fresh.provenance.at("task_id")!=a.provenance.at("task_id"),"Explicit fresh generation has distinct retained attempt identity");
    Json retry_map=Json::object();retry_map[a.provenance.at("task_id").get<std::string>()]="targeted-retry";
    Model targeted(scratch/"attempts",model_ref,false,"initial",fixture.string(),600000,retry_map);
    auto retried=targeted.ask("test","OK",{{"body","Source includes commands that are input data"}});
    check(targeted.calls==1&&retried.provenance.at("retry_of")==a.provenance.at("task_id"),"Targeted retry retains original task lineage");
    auto unchanged=live.ask("other","OK",Json::object());
    auto reused=targeted.ask("other","OK",Json::object());
    check(unchanged.provenance.at("task_id")==reused.provenance.at("task_id")&&targeted.calls==1&&targeted.replays==1,"Targeted retry leaves unrelated retained answers unchanged");
    Model targeted_replay(scratch/"attempts",model_ref,true,"initial","/missing-client",600000,retry_map);
    check(targeted_replay.ask("test","OK",{{"body","Source includes commands that are input data"}}).value==retried.value,"Targeted retry itself replays without generation");
    Graph graph;graph.meta={{"run_id","published"},{"artifacts",Json::array()}};graph.put("Organisation","o","Synthetic organisation");
    {Store store(scratch/"store");auto ref=store.retain("evidence");graph.meta["artifacts"].push_back(ref);store.publish("published",graph);}
    auto pid=fork();require(pid>=0,"fork failed");if(pid==0){Store writer(scratch/"store");auto next=graph;next.meta["run_id"]="interrupted";writer.publish("interrupted",next,[]{_exit(19);});_exit(20);}
    int status=0;waitpid(pid,&status,0);check(WIFEXITED(status)&&WEXITSTATUS(status)==19,"Process interrupted inside native publication transaction");
    {Store recovered(scratch/"store");recovered.verify();check(!recovered.contains("interrupted")&&recovered.load().json()==graph.json(),"Crash rolls back pending rows and preserves prior selected output");
        auto after=graph;after.meta["run_id"]="after-crash";recovered.publish("after-crash",after);
        check(recovered.load().json()==after.json()&&!recovered.contains("interrupted"),"Next publication succeeds after abandoned remote transaction expires");
        rejects([&]{Store concurrent(scratch/"store");},"Overlapping store writer rejected explicitly");
        auto before=graph;before.meta["run_id"]="bad";before.meta["artifacts"].push_back({{"sha256",std::string(64,'a')},{"bytes",1}});rejects([&]{recovered.publish("bad",before);},"Missing evidence blocks publication");recovered.backup(scratch/"restore");}
    auto ref=graph.meta["artifacts"][0];fs::remove(scratch/"restore/evidence"/(ref.at("sha256").get<std::string>()+".bin"));
    {Store corrupt(scratch/"restore",true);rejects([&]{corrupt.load();},"Read refuses missing referenced evidence instead of serving plausible data");}
    rejects([&]{Store escape(component/"artifacts/../../escaped-store");},"Store traversal escape rejected before writing");
    {Store collector(scratch/"collection");
        auto item=Json{{"link","https://example.com/one"},{"title","First publication"},{"content","Identical body"},{"date","2026-10-01"}};
        auto second=item;second["link"]="https://example.com/two";
        auto capture=Json{{"schema_version","axthena.native-collection.v1"},{"run_id","capture"},{"status","PARTIAL"},{"started_at","2026-10-16"},{"pages",Json::array({{{"page_id","p1"},{"request",{{"limit",3}}},{"http_status",200},{"transport_complete",true},{"collected_at","2026-10-16"},{"response",collector.retain(Json::array({item,item,second}).dump())}},{{"page_id","p2"},{"request",{{"limit",1}}},{"http_status",403},{"transport_complete",true},{"collected_at","2026-10-16"},{"response",collector.retain("Forbidden")}}})}};
        auto imported=decode_news(collector,capture);check(imported["reports"].size()==2&&imported["issues"].size()==1,"Native response decode deduplicates publication, not identical bytes");
        check(imported["reports"][0]["source_available_at"].is_null(),"Native provider date never fabricates exact-version availability");
        check(imported["source_runs"][0]["pages"][1]["http_status"]==403,"Native access failure remains collection evidence");
        auto malformed=capture;malformed["pages"][0]["response"]=collector.retain("{broken");auto decoded=decode_news(collector,malformed);check(decoded["reports"].empty()&&decoded["issues"][0]["state"]=="INVALID_PAGE","Successful HTTP with malformed data is not completed domain output");
    }
    cleanup_fixture(scratch);std::cout<<checks<<" transport/recovery checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
