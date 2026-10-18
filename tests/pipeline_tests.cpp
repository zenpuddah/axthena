#include "axthena/core.hpp"
#include "cleanup.hpp"
#include <iostream>
#include <unistd.h>
using namespace axthena;
namespace {
int checks=0;
void check(bool result,const char* name){require(result,std::string("TEST: ")+name);++checks;std::cout<<"PASS "<<name<<std::endl;}
Json fixture_answer(const std::string& body,const std::string& meaning){
    Json support=Json::array({{{"quote",body},{"occurrence",1}}});
    Json answer={{"schema_version","axthena.extraction.v1"},{"status","COMPLETE"},{"issues",Json::array()},{"omissions",Json::array()},
                 {"organisations",Json::array()},{"people",Json::array()},{"resources",Json::array()},{"ideas",Json::array()},{"events",Json::array()},{"claims",Json::array()},{"relationships",Json::array()}};
    for(const auto& pair:{std::pair{"o1","NVIDIA"},std::pair{"o2","Microsoft"},std::pair{"o3","Synthetic Robotics"}})
        answer["organisations"].push_back({{"key",pair.first},{"name",pair.second},{"aliases",Json::array()},{"description",std::string(pair.second)+" fixture organisation"},{"evidence",support}});
    Json judgments=Json::array();
    for(const auto& pair:{std::pair{"NVDA.US","UP"},std::pair{"MSFT.US","DOWN"}})
        judgments.push_back({{"stock",pair.first},{"direction",pair.second},{"quoted",false},{"conditions","if the chip ships on time"},
                            {"horizon",{{"wording","next two weeks"},{"calendar_days",14},{"anchor",nullptr},{"start",nullptr},{"end",nullptr}}},{"evidence",support}});
    answer["ideas"].push_back({{"key","i1"},{"summary",body},{"fixture_meaning",meaning},{"kind","OPINION"},{"agreement","strong"},{"speaker",nullptr},
                              {"subjects",Json::array({{{"organisation","o1"},{"domain","chips"}},{{"organisation","o2"},{"domain","cloud"}}})},{"judgments",judgments},{"evidence",support}});
    answer["relationships"].push_back({{"from","o3"},{"to","o1"},{"kind","SUBSIDIARY_OF"},{"activity","robotics"},{"description","Synthetic subsidiary relationship"},{"modality","REPORTED_OCCURRED"},{"date","2026-10-01"},{"evidence",support}});
    return answer;
}
}
int main(){try{
    auto scratch=component/"artifacts/test-work"/("pipeline-"+std::to_string(getpid()));
    auto executable=scratch/"client";write_once(executable,read_file(component/"tests/pipeline_fixture.py"));fs::permissions(executable,fs::perms::owner_all);
    auto config=read_json(component/"config/v1.json");config["model"]={{"providerID","fixture"},{"id","frozen-insertion-contract"}};
    std::string body="Synthetic Robotics is a subsidiary of NVIDIA. I expect NVIDIA shares UP and Microsoft shares DOWN over the next two weeks if the chip ships on time, because cheaper computing changes demand.";
    Json imported={{"schema_version","axthena.import.v1"},{"reports",Json::array()},{"artifacts",Json::array()},{"source_change","SYNTHETIC CONTRACT FIXTURE"},
                   {"source_runs",Json::array({{{"run_id","fixture"},{"status","COMPLETE"},{"started_at","2026-10-16"},{"pages",Json::array()},{"plan",{{"scope","synthetic only"}}}}})}};
    {Store store(scratch/"store");
        for(int i=1;i<=3;++i){
            auto wording=i==3?body+" A different developed reason is a distribution partnership.":body;
            auto report=Json{{"number",i},{"report_id","fixture-"+std::to_string(i)},{"title","Synthetic publication "+std::to_string(i)},
                {"url","https://fixture.invalid/"+std::to_string(i)},{"source_host","fixture.invalid"},{"body_text",wording},{"body_sha256",sha256(wording)},
                {"body",store.retain(wording)},{"capture_metadata",store.retain("synthetic")},{"published_date","2026-10-01"},{"source_available_at","2026-10-01"},
                {"collected_at","2026-10-16"},{"collection_run","fixture"},{"author",i==1?"sel":"selene"},{"fixture_extraction",fixture_answer(wording,i==3?"different":"same")}};
            imported["reports"].push_back(report);
        }
        auto graph=prepare(store,imported,config,false,3,executable.string());
        std::map<std::string,int> counts;std::string shared;
        for(const auto& [id,n]:graph.nodes){++counts[n.kind];if(n.kind=="Organisation"&&n.label=="Synthetic Robotics")check(n.props.at("uncertain").get<bool>(),"Completed uncertain identity creation is retained");}
        check(graph.meta.at("reports_failed")==0,"Synthetic preparation finishes all report stages");
        check(counts["Organisation"]==3&&counts["ResolutionAttempt"]==9,"Created organisation is immediately retrieved and reused sequentially");
        check(counts["Report"]==3&&counts["Expression"]==3&&counts["Idea"]==2,"Same insertion shares meaning while different reason stays separate");
        check(counts["Judgment"]==6,"Joint idea retains two separately scoped directions per expression");
        check(counts["Person"]==2,"Distinct sel and selene accounts survive shared meaning");
        std::map<std::string,int> memberships;for(const auto& [id,e]:graph.edges){(void)id;if(e.kind=="EXPRESSES"&&++memberships[e.to]==2)shared=e.to;}
        auto result=query(graph,{{"query","Q3"},{"id",shared},{"mode","strict"},{"to","2026-10-15"}});
        check(result.at("distinct_reports")==2&&result.at("spread_counts")[0].at("distinct_reports")==2,"Two publications count once each despite multiple subjects and judgments");
        for(const auto& n:result.at("nodes"))if(n.at("kind")=="Judgment"){
            check(n.at("props").at("conditions")=="if the chip ships on time"&&!n.at("evidence").empty()&&n.at("props").at("window").at("end")=="2026-10-15","Query preserves qualified direction, publication anchor and exact evidence");
        }
        auto replay=prepare(store,imported,config,true,3,"/missing-client");check(replay.json()==graph.json(),"Reapplying published preparation is byte-equivalent without model execution");
        config["forecast_fallback_days"]=7;
        auto revision=prepare(store,imported,config,true,3,"/missing-client");check(revision.meta.at("run_id")!=graph.meta.at("run_id")&&revision.nodes.size()==graph.nodes.size(),"Changed policy produces new run through retained-response replay without count inflation");
        check(store.load(graph.meta.at("run_id")).json()==graph.json(),"Earlier grouping and forecasts remain recoverable after reprocessing");
        auto first_corpus=apply_corpus(store,imported);
        auto baseline=prepare(store,first_corpus,config,true,0,"/missing-client");
        check(baseline.meta.at("reports_failed")==0,"Initial corpus preparation reuses existing retained answers");
        auto second=imported;second["reports"]=Json::array({imported["reports"][1]});
        for(int i=4;i<=25;++i){auto report=imported["reports"][0];report["number"]=i;report["report_id"]="fixture-"+std::to_string(i);report["title"]="Synthetic publication "+std::to_string(i);second["reports"].push_back(report);}
        auto growing=apply_corpus(store,second);
        auto grown=prepare(store,growing,config,false,0,executable.string());
        check(growing["reports"].size()==25&&grown.meta.at("reports_failed")==0,"Overlapping batches prepare twenty-five unique publications");
        std::map<std::string,int> grown_counts;for(const auto& [id,n]:grown.nodes){(void)id;++grown_counts[n.kind];}
        check(grown_counts["Organisation"]==3&&grown_counts["Idea"]==2,"Organisation and shared meaning identities persist across batch growth");
        check(grown.nodes.contains(shared)&&grown.nodes.at(shared).label==graph.nodes.at(shared).label,"Earlier shared Idea identity remains stable");
        auto receipt=read_json(store.root/"preparations"/grown.meta.at("plan_id").get<std::string>().substr(5)/"published.json");
        check(receipt.at("model_calls")==110&&receipt.at("model_replays")==15,"Only new reports trigger model work across corpus growth");
        check(grown.meta.at("coverage").at("unprocessed_reports")==0,"Coverage reflects actual accumulated corpus processing");
        auto retained=prepare(store,growing,config,true,0,"/missing-client");check(retained.json()==grown.json(),"Whole expanded corpus reapplication uses published immutable run");
        check(store.load(graph.meta.at("run_id")).json()==graph.json(),"Pre-corpus graph remains immutable after recurring use");
        auto view=revise(grown,{{"schema_version","axthena.review-change.v1"},{"kind","view"},{"author","Synthetic owner"},{"reason","Cross-batch retention test"},
            {"name","Retained source view"},{"sources",Json::array({"fixture.invalid"})},{"stocks",Json::array({"NVDA.US"})}});
        Json controls={{"nodes",Json::array()},{"edges",Json::array()},{"artifacts",Json::array()}};std::string view_id;
        for(const auto& [id,n]:view.nodes)if(n.kind=="ViewVersion"){view_id=id;controls["nodes"].push_back(n);}
        store.document("owner_change",view.meta.at("run_id"),controls);
        auto with_view=prepare(store,growing,config,true,0,"/missing-client");
        check(Json(with_view.nodes.at(view_id))==Json(view.nodes.at(view_id)),"Dated owner view survives preparation with original identity/time");
        check(query(with_view,{{"query","Q1"},{"view_id",view_id},{"mode","declared_date"}}).at("distinct_reports")==25,"Retained owner view remains usable on expanded corpus");
        store.verify();
    }
    cleanup_fixture(scratch);std::cout<<checks<<" native insertion checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
