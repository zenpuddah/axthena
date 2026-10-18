#include "axthena/core.hpp"
#include "cleanup.hpp"
#include <iostream>
#include <unistd.h>

using namespace axthena;
int checks=0;
void check(bool value,const std::string& name){require(value,"TEST: "+name);++checks;std::cout<<"PASS "<<name<<'\n';}
template<class F> void rejects(F f,const std::string& name){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,name);}
int main(){try{
    check(sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 reference");
    auto span=locate_passage("甲🙂é",{{"quote","🙂é"},{"occurrence",1}});check(span["begin"]==3&&span["end"]==9,"UTF-8 byte offsets");
    auto repeated=locate_passage("same; same",{{"quote","same"},{"occurrence",2}});check(repeated["begin"]==6,"Repeated passage occurrence");
    rejects([]{locate_passage("real",{{"quote","invented"},{"occurrence",1}});},"Fabricated passage rejected");
    rejects([]{locate_passage("same same",{{"quote","same"}});},"Missing occurrence rejected");
    rejects([]{locate_passage("e\xcc\x81",{{"quote","é"},{"occurrence",1}});},"Composed accent does not silently match decomposed source bytes");
    check(add_days("2024-02-28",2)=="2024-03-01","Calendar leap day");
    auto horizon=resolve_window({{"wording","next two weeks"},{"calendar_days",14},{"anchor",nullptr}},"2026-10-01",nullptr);
    check(horizon["start"]=="2026-10-01"&&horizon["end"]=="2026-10-15","Publication-anchored fourteen calendar days");
    check(resolve_window({{"wording",nullptr}},"2026-10-01",nullptr)["origin"]=="UNSTATED_HORIZON","No invented fallback");
    check(resolve_window({{"wording","medium term"}},"2026-10-01",nullptr)["origin"]=="SOURCE_HORIZON_UNRESOLVED","Qualitative horizon distinguished");
    auto united=candidate_union(Json::array({{{"id","O1"}},{{"id","O2"}}}),Json::array({{{"id","O2"}},{{"id","O3"}}}));
    check(united.size()==3&&united[1]["intersection"]==true,"Full name/vector union and intersection");
    check(name_similarity("NVIDIA","nvidia")==1,"Case-consistent name retrieval");
    check(cosine({1,0},{0,1})==0,"Actual vector cosine contrast");
    rejects([]{cosine({0,0},{1,0});},"Zero embedding rejected");
    auto scratch=component/"artifacts/test-work"/("native-"+std::to_string(getpid()));fs::create_directories(scratch);
    Graph graph;auto report=graph.put("Report","r1","source",{{"body_text","甲🙂é"},{"published_date","2026-10-01"},{"source_available_at",nullptr},{"source_host","synthetic"}});
    span["report"]=report;auto passage=graph.put("Passage","p1","🙂é",span);graph.link("HAS_PASSAGE",report,passage);
    graph.meta={{"run_id","synthetic-run"},{"policy",{{"max_hops",3},{"max_results",500},{"max_expansions",1000}}},{"artifacts",Json::array()},{"input_reports",1}};
    graph.verify();check(Graph::parse(graph.json()).json()==graph.json(),"Typed graph round trip");
    auto broken=graph;broken.edges.begin()->second.to="missing";rejects([&]{broken.verify();},"Orphan graph edge rejected");
    broken=graph;broken.nodes.at(passage).props["begin"]=1;rejects([&]{broken.verify();},"Corrupted Unicode span rejected");
    broken=graph;broken.nodes.at(passage).props["unit"]="CODE_POINTS";rejects([&]{broken.verify();},"Unsupported persisted offset unit rejected");
    broken=graph;broken.nodes.at(passage).kind="UnknownFutureType";rejects([&]{broken.verify();},"Unknown persisted record kind rejected");
    {Store store(scratch/"store");auto ref=store.retain("exact source bytes");graph.meta["artifacts"].push_back(ref);store.publish("synthetic-run",graph);store.publish("synthetic-run",graph);store.verify();check(store.evidence(ref)=="exact source bytes","Exact evidence and idempotent publication");
      auto revised=graph;revised.meta["changed"]=true;rejects([&]{store.publish("synthetic-run",revised);},"Immutable run overwrite rejected");store.backup(scratch/"restore");}
    {Store reopened(scratch/"store",true);check(reopened.load().json()==graph.json(),"Native restart preserves same graph");rejects([&]{reopened.retain("write");},"Read-only store rejects writes");}
    {Store restored(scratch/"restore",true);restored.verify();check(restored.load().json()==graph.json(),"Full backup/restore equality");}
    auto strict=query(graph,{{"query","Q7"},{"mode","strict"},{"from","2026-10-01"},{"to","2026-10-02"}});
    check(strict["coverage"]["eligible_reports"]==0&&strict["coverage"]["exclusions"].size()==1,"Unknown exact-version availability excludes strict history");
    check(query(graph,{{"query","Q7"},{"mode","declared_date"}})["distinct_reports"]==1,"Explicit declared-date exploration");
    rejects([&]{query(graph,{{"mode","operational_as_of"}});},"Unsupported operational replay rejected");
    cleanup_fixture(scratch);std::cout<<checks<<" native contract checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
