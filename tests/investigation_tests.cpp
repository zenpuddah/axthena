#include "axthena/investigation.hpp"
#include "cleanup.hpp"
#include <iostream>
#include <unistd.h>
using namespace axthena;
namespace {
int count=0;
void check(bool value,const char* name){require(value,std::string("TEST: ")+name);++count;std::cout<<"PASS "<<name<<std::endl;}
template<class F> void rejects(F f,const char* name){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,name);}
}
int main(){try{
    Json candidates=Json::array({{{"id","A"}},{{"id","B"}}});
    Json decision={{"selected_id","A"},{"uncertain",false},{"reason","Same activity"},{"comparisons",Json::array({{{"id","A"},{"decision","SAME"},{"reason","Equivalent"}},{{"id","B"},{"decision","RELATED"},{"reason","Broader"}}})}};
    check(domain_selection(decision,candidates)=="A","Equivalent candidate reuses identity");
    auto invalid=decision;invalid["selected_id"]="B";rejects([&]{domain_selection(invalid,candidates);},"Related broader category cannot merge");
    invalid=decision;invalid["uncertain"]=true;rejects([&]{domain_selection(invalid,candidates);},"Uncertain SAME cannot merge");
    invalid=decision;invalid["selected_id"]="outside";rejects([&]{domain_selection(invalid,candidates);},"Non-candidate merge rejected");
    invalid=decision;invalid["comparisons"].erase(1);rejects([&]{domain_selection(invalid,candidates);},"Incomplete candidate comparison rejected");
    auto plan=investigation_plan({{"intent","overview"},{"stocks",Json::array({"NVDA.US"})}});
    check(plan.at("mode")=="strict","Unspecified time basis stays strict");
    check(investigation_plan({{"intent","relationships"}}).at("max_hops")==1,"Relationship plan discloses its executed default hop bound");
    rejects([&]{investigation_plan({{"cypher","MATCH (n) DELETE n"}});},"Planner cannot supply arbitrary code");
    rejects([&]{investigation_plan({{"intent","compare"},{"stocks",Json::array({"NVDA.US","NVDA.US"})}});},"Compare requires distinct companies");
    rejects([&]{investigation_plan({{"from","2025-02-30"}});},"Impossible date rejected");
    rejects([&]{investigation_plan({{"intent","coverage"},{"stocks",Json::array({"NVDA.US"})}});},"Coverage cannot silently ignore company scope");
    rejects([&]{investigation_plan({{"intent","evidence"}});},"Evidence needs record identity");

    auto scratch=component/"artifacts/test-work"/("domains-"+std::to_string(getpid()));Store store(scratch);
    Graph graph;graph.meta={{"run_id","domain-fixture"},{"policy",{{"max_results",1000},{"max_hops",0}}},{"artifacts",Json::array()}};
    auto org=graph.put("Organisation","one","One"),other=graph.put("Organisation","two","Two");
    const std::vector<std::string> labels={"gpu manufacturing","graphics processor manufacturing","gpu manufacturing services","semiconductor manufacturing"};
    int number=0;std::vector<std::string> domains;
    for(const auto& label:labels){
        ++number;auto date=number==2?"2025-02-01":"2025-01-01";
        auto r=graph.put("Report",label,label,{{"number",number},{"body_text",label},{"published_date",date},{"source_available_at",date},{"source_host","fixture.example"}});
        auto span=locate_passage(label,{{"quote",label},{"occurrence",1}});span["report"]=r;auto p=graph.put("Passage",label,label,span);
        auto domain=graph.put("Domain",label,label);domains.push_back(domain);auto company=number==2?other:org;
        auto a=graph.put("OrganisationActivity",Json::array({company,domain}),label,{{"organisation",company},{"domain",domain}});
        auto e=graph.put("Event",label,label,{{"report",r}},Json::array({p}));graph.link("CONCERNS_ACTIVITY",e,a,Json::object(),Json::array({p}));graph.link("CONCERNS",e,company,Json::object(),Json::array({p}));graph.link("IN_DOMAIN",a,domain);graph.link("HAS_ACTIVITY",company,a);
    }
    graph.link("SUPPLIES",org,other,{{"fixture","only original organisation dependency"}});
    auto config=read_json(component/"config/annual-2025-recovery.json");config["domain_resolution"]="candidate_union_v1";
    auto before=graph.json();auto resolved=resolve_domains(store,graph,config,false,(component/"tests/domain_fixture.py").string());
    check(graph.json()==before,"Resolver leaves source graph unchanged");
    check(resolved.meta.at("domain_resolution").at("canonical_categories")==3,"Equivalent paraphrase reuses category; broad and service variants stay separate");
    check(resolved.nodes.at(identity("OrganisationActivity",Json::array({other,domains[0]}))).props.at("organisation")==other,"Reuse retains organisation-specific activity pair");
    int dependencies=0;for(const auto& [id,e]:resolved.edges){(void)id;if(e.kind=="SUPPLIES"){++dependencies;check(e.from==org&&e.to==other,"Organisation dependency unchanged");}}
    check(dependencies==1,"No dependency transferred during domain merge");
    check(resolve_domains(store,graph,config,true,(component/"tests/domain_fixture.py").string()).json()==resolved.json(),"Retained domain answers replay byte-equivalent graph");
    auto result=query(resolved,{{"query","Q1"},{"mode","declared_date"},{"from","2025-01-01"},{"to","2025-12-31"}});
    check(!result.at("nodes").empty(),"Resolved graph remains queryable");
    auto early=query(resolved,{{"query","Q1"},{"mode","declared_date"},{"from","2025-01-01"},{"to","2025-01-31"}});
    check(!early.at("grouping_exclusions").empty(),"Domain grouping with future comparison context is withheld");
    for(const auto& e:early.at("edges"))for(const auto& report:e.at("props").value("domain_context_reports",Json::array()))check(resolved.nodes.at(report).props.at("published_date")<="2025-01-31","Returned domain memberships have eligible context");
    config["domain_attempt"]="missing";rejects([&]{resolve_domains(store,graph,config,true,(component/"tests/domain_fixture.py").string());},"Missing replay aborts instead of creating category");
    config["domain_attempt"]="bounded-recovery-test";config["recovery_attempts"]=2;
    setenv("AXTHENA_DOMAIN_FAIL_ONCE",(scratch/"failed-once").c_str(),1);
    auto recovered=resolve_domains(store,graph,config,false,(component/"tests/domain_fixture.py").string());
    unsetenv("AXTHENA_DOMAIN_FAIL_ONCE");
    check(recovered.meta.at("domain_resolution").at("canonical_categories")==3,"Bounded invalid-response recovery preserves semantic contrasts");
    check(resolve_domains(store,graph,config,true,(component/"tests/domain_fixture.py").string()).json()==recovered.json(),"Failed original plus versioned recovery replay deterministically");
    cleanup_fixture(scratch);std::cout<<count<<" investigation checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
