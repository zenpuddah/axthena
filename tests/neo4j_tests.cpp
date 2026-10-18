#include "axthena/neo4j.hpp"
#include "cleanup.hpp"
#include <iostream>
#include <unistd.h>
using namespace axthena;
namespace {
int checks=0;
void check(bool value,const std::string& message){require(value,"TEST: "+message);++checks;std::cout<<"PASS "<<message<<std::endl;}
template<class F> void rejects(F f,const char* message){bool threw=false;try{f();}catch(...){threw=true;}check(threw,message);}
}
int main(){try{
    auto scratch=component/"artifacts/test-work"/("neo4j-"+std::to_string(getpid()));Neo4j db;
    rejects([&]{db.execute("CREATE (:ForbiddenReadWrite) RETURN 1 AS result");},"Read access rejects Cypher writes");
    rejects([&]{db.execute("THIS IS NOT CYPHER");},"HTTP success with Cypher errors is rejected");
    {
        Store store(scratch/"store");Graph g;g.meta={{"run_id","fixture"},{"policy",{{"max_hops",3},{"max_expansions",1000},{"max_results",500}}},{"input_reports",3},{"artifacts",Json::array()}};
        auto org=g.put("Organisation","O1","A"),other=g.put("Organisation","O2","B");auto stock=g.put("Stock","NVDA.US","NVDA",{{"symbol","NVDA.US"}});g.link("CONFIGURED_ISSUER",stock,org);
        std::vector<std::string> reports,passages;
        for(int i=0;i<3;++i){std::string body="甲🙂é A expects shares UP if demand persists. Revenue was "+std::to_string(80+i)+" million.";
            auto ref=store.retain(body);g.meta["artifacts"].push_back(ref);Json availability=i==2?Json(nullptr):Json(i==0?"2026-10-01":"2026-10-05");
            auto report=g.put("Report",i,"Report "+std::to_string(i),{{"body_text",body},{"body_sha256",sha256(body)},{"body",ref},{"published_date","2026-10-01"},{"source_available_at",availability},{"source_host",i==0?"a.example":"b.example"},{"processing_status","COMPLETE"}});reports.push_back(report);
            auto span=locate_passage(body,{{"quote",body},{"occurrence",1}});span["report"]=report;auto passage=g.put("Passage",i,body,span);passages.push_back(passage);
        }
        auto idea=g.put("Idea","idea","Qualified outlook",{{"summary","Qualified outlook"}},Json::array({passages[0]}));
        auto expression=g.put("Expression","E","Qualified outlook",{{"report",reports[0]},{"summary","Qualified outlook"},{"kind","OPINION"},{"membership_inputs_reports",Json::array({reports[0]})}},Json::array({passages[0]}));
        g.link("CONTAINS",reports[0],expression);g.link("EXPRESSES",expression,idea);g.link("CONCERNS",expression,org,Json::object(),Json::array({passages[0]}));
        auto judgment=g.put("Judgment","J","UP",{{"report",reports[0]},{"direction","UP"},{"conditions","if demand persists"}},Json::array({passages[0]}));g.link("HAS_JUDGMENT",expression,judgment);g.link("TARGETS",judgment,stock);
        auto claim=g.put("Claim","claim","Revenue",{{"status","REPORTED"}},Json::array({passages[0]}));
        for(int i=0;i<2;++i){auto assertion=g.put("Assertion",i,"Revenue assertion",{{"report",reports[i]}},Json::array({passages[i]}));g.link("ASSERTS",reports[i],assertion);g.link("ABOUT_CLAIM",assertion,claim);
            auto c=g.put("Confirmation",i,"Scoped",{{"claim",claim},{"report",reports[i]},{"stance","ASSERTS"},{"quantity",std::to_string(80+i)},{"unit","million"}},Json::array({passages[i]}));g.link("CONFIRMS",c,claim);
        }
        g.link("DEPENDS_ON",org,other,{{"modality","REPORTED_OCCURRED"}},Json::array({passages[1]}));
        g.verify();store.publish("fixture",g);
        Json requests=Json::array();for(const auto* q:{"Q1","Q2","Q4","Q5","Q7"})for(const auto* mode:{"strict","declared_date"})requests.push_back({{"query",q},{"mode",mode},{"to","2026-10-03"},{"stocks",Json::array({"NVDA.US"})}});
        requests.push_back({{"query","Q3"},{"id",idea},{"mode","strict"},{"to","2026-10-06"}});requests.push_back({{"query","Q6"},{"id",passages[0]},{"mode","strict"}});
        requests.push_back({{"query","Q5"},{"id",claim},{"mode","strict"},{"to","2026-10-06"},{"sources",Json::array({"a.example"})},{"confirmed_only",true}});
        requests.push_back({{"query","Q2"},{"stocks",Json::array({"NVDA.US"})},{"mode","strict"},{"to","2026-10-06"},{"limit",1}});
        for(const auto& request:requests)check(store.read(request)==query(g,request),"Database/reference parity "+request.at("query").get<std::string>()+" "+request.at("mode").get<std::string>());
        auto held=store.read(requests[12]);check(held.at("holds").size()==1&&held.at("nodes").empty(),"Outside-view contradictory confirmation remains held in confirmed-only read");
        check(store.read(requests[2]).at("relevance_paths").empty(),"Later dependency excluded from early source picture");
        auto namespace_id=store.status().at("namespace");auto key=namespace_id.get<std::string>()+"/fixture/"+org;
        db.execute("MATCH (n:AxRecord {key:$key}) SET n.kind='Stock' RETURN n.id AS id",{{"key",key}},false);
        rejects([&]{store.verify();},"Indexed projection tampering is detected");
        db.execute("MATCH (n:AxRecord {key:$key}) SET n.kind='Organisation' RETURN n.id AS id",{{"key",key}},false);store.verify();
        const std::string reverse="MATCH (a)-[e:DEPENDS_ON {store:$store,run:'fixture'}]->(b) WITH a,b,e,properties(e) AS properties DELETE e CREATE (b)-[replacement:DEPENDS_ON]->(a) SET replacement=properties RETURN replacement.id AS id";
        db.execute(reverse,{{"store",namespace_id}},false);
        rejects([&]{store.verify();},"Physical relationship direction must match its retained record");
        rejects([&]{store.read(requests[13]);},"Selective traversal detects changed relationship topology");
        db.execute(reverse,{{"store",namespace_id}},false);store.verify();
        rejects([&]{store.backup(scratch/"store/recursive-backup");},"Recursive backup destination is rejected");
        write_once(scratch/"store/attempts/interrupted/raw.json","preserved incomplete output");
        store.backup(scratch/"backup");Store restored(scratch/"backup",true);check(restored.status().at("namespace")!=namespace_id&&restored.load().json()==g.json(),"Backup restores into independent namespace with identical graph");
        check(restored.read(requests[12])==held,"Restored database preserves held query");
        Store second(scratch/"second");check(!second.contains("fixture"),"Separate store cannot see another namespace's run");
        second.restore(scratch/"backup");check(second.load().json()==g.json(),"Portable backup imports without original namespace access");
        fs::remove(scratch/"backup/attempts/interrupted/raw.json");write_once(scratch/"backup/attempts/interrupted/raw.json","changed incomplete output");
        Store corrupted(scratch/"corrupted");rejects([&]{corrupted.restore(scratch/"backup");},"Restore rejects changed interrupted model evidence before publishing");check(corrupted.runs().empty(),"Corrupt archive publishes no run");
        auto changed=g;changed.nodes.at(org).label="Changed";rejects([&]{store.publish("fixture",changed);},"Completed run cannot be silently overwritten");
        auto attempt=g;attempt.meta["run_id"]="rolled-back";rejects([&]{store.publish("rolled-back",attempt,[]{throw std::runtime_error("injected failure");});},"Publication exception rolls transaction back");check(!store.contains("rolled-back")&&store.load().json()==g.json(),"Failed publication preserves selected completed run");
        auto path=scratch/"store/evidence"/(g.meta["artifacts"][0]["sha256"].get<std::string>()+".bin");fs::remove(path);
        rejects([&]{store.read(requests[11]);},"Database evidence trace rejects missing original bytes");
    }
    cleanup_fixture(scratch);std::cout<<checks<<" Neo4j integration checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
