#include "axthena/investigation.hpp"
#include <algorithm>
#include <iostream>

namespace axthena {
Json domain_selection(const Json& decision,const Json& candidates){
    require(decision.is_object()&&decision.contains("selected_id")&&decision.contains("uncertain")&&decision["uncertain"].is_boolean()
        &&decision.contains("reason")&&decision["reason"].is_string()&&decision.contains("comparisons")&&decision["comparisons"].is_array(),"Invalid domain decision");
    std::set<std::string> expected,seen;for(const auto& c:candidates)expected.insert(c.at("id"));
    std::map<std::string,std::string> outcomes;
    for(const auto& c:decision["comparisons"]){
        auto id=c.at("id").get<std::string>(),status=c.at("decision").get<std::string>();
        require(expected.contains(id)&&seen.insert(id).second,"Unknown or repeated domain comparison");
        require(std::set<std::string>{"SAME","RELATED","DIFFERENT","UNCERTAIN"}.contains(status)&&c.at("reason").is_string(),"Invalid domain comparison");outcomes[id]=status;
    }
    require(seen==expected,"Incomplete domain comparisons");
    if(decision["selected_id"].is_null())return nullptr;
    require(decision["selected_id"].is_string(),"Invalid domain selection type");
    auto id=decision["selected_id"].get<std::string>();
    require(expected.contains(id)&&outcomes.at(id)=="SAME"&&!decision["uncertain"].get<bool>(),"Only established SAME domain candidates can be reused");return id;
}

Graph resolve_domains(Store& store,const Graph& source,const Json& config,bool replay,const std::string& client){
    require(config.value("domain_resolution","")=="candidate_union_v1","Unsupported domain resolution policy");
    if(source.meta.contains("domain_resolution"))return source;
    const unsigned bound=config.value("candidate_limit",8U);require(bound>0&&bound<=32,"Invalid domain candidate bound");
    Graph graph=source;Encoder encoder(config.at("encoder"));
    Model model(store.root/"domain-attempts",config.at("model"),replay,config.value("domain_attempt","initial"),client,600000,config.value("task_attempts",Json::object()));
    const unsigned retries=config.value("recovery_attempts",0U);require(retries<=3,"Invalid domain recovery bound");
    const auto prompt=read_file(resource_root()/"prompts/domain-v1.md");
    graph.meta["artifacts"].push_back(store.retain(prompt));
    graph.meta["artifacts"].push_back(store.retain(read_file(resource_root()/"src/domains.cpp")));
    struct Category {std::string id;std::vector<float> vector;Json labels=Json::array();std::set<std::string> reports;};
    std::vector<Category> pool;
    std::map<std::string,std::set<std::string>> support,reports;
    std::map<std::string,std::string> activity_domain;
    for(const auto& [id,n]:source.nodes)if(n.kind=="OrganisationActivity")activity_domain[id]=n.props.at("domain");
    for(const auto& [id,e]:source.edges)if(e.kind=="CONCERNS_ACTIVITY"&&activity_domain.contains(e.to)){
        (void)id;auto domain=activity_domain.at(e.to);
        for(const auto& p:e.support){support[domain].insert(p);reports[domain].insert(source.nodes.at(p).props.at("report"));}
    }
    std::vector<std::string> ordered;
    for(const auto& [id,n]:source.nodes)if(n.kind=="Domain")ordered.push_back(id);
    auto first=[&](const std::string& id){int number=2147483647;for(const auto& r:reports[id])number=std::min(number,source.nodes.at(r).props.value("number",number));return number;};
    std::sort(ordered.begin(),ordered.end(),[&](const auto& a,const auto& b){return std::pair{first(a),source.nodes.at(a).label}<std::pair{first(b),source.nodes.at(b).label};});
    std::map<std::string,std::string> mapping;std::map<std::string,Json> contexts;Json decisions=Json::array();
    auto top=[&](Json values){std::sort(values.begin(),values.end(),[](const auto& a,const auto& b){return a.at("score")==b.at("score")?a.at("id")<b.at("id"):a.at("score")>b.at("score");});while(values.size()>bound)values.erase(values.end()-1);return values;};
    for(const auto& id:ordered){
        const auto& original=source.nodes.at(id);auto vector=encoder.encode(original.label,true);
        Json names=Json::array(),vectors=Json::array();
        for(const auto& candidate:pool){double score=0;for(const auto& label:candidate.labels)score=std::max(score,name_similarity(original.label,label));names.push_back({{"id",candidate.id},{"score",score}});vectors.push_back({{"id",candidate.id},{"score",cosine(vector,candidate.vector)}});}
        names=top(names);vectors=top(vectors);auto united=candidate_union(names,vectors);std::set<std::string> premises=reports[id];
        for(auto& c:united)for(const auto& candidate:pool)if(c.at("id")==candidate.id){c["canonical_name"]=graph.nodes.at(candidate.id).label;c["variants"]=candidate.labels;premises.insert(candidate.reports.begin(),candidate.reports.end());}
        Json excerpts=Json::array();for(const auto& p:support[id])if(excerpts.size()<6){const auto& n=source.nodes.at(p);excerpts.push_back({{"passage",p},{"report",n.props.at("report")},{"quote",n.props.at("quote")}});}
        Json packet={{"incoming",{{"id",id},{"label",original.label},{"evidence",excerpts},{"total_passages",support[id].size()}}},{"name_candidates",names},{"vector_candidates",vectors},{"union",united},{"retrieval_status","COMPLETE"},{"encoder",config.at("encoder")}};
        ModelAnswer answer;Json selected;Json failures=Json::array();
        for(unsigned attempt=0;;++attempt){
            try{
                if(attempt==0)answer=model.ask("domain_identity",prompt,packet);
                else{
                    Model recovery(store.root/"domain-attempts",config.at("model"),replay,
                        config.value("domain_attempt","initial")+"-bounded-recovery-v1-"+std::to_string(attempt),client);
                    answer=recovery.ask("domain_identity",prompt,packet);
                }
                selected=domain_selection(answer.value,united);break;
            }catch(const std::exception& e){
                failures.push_back({{"attempt",attempt},{"error",e.what()}});
                std::cerr<<"Domain "<<original.label<<" attempt "<<attempt<<": "<<e.what()<<std::endl;
                require(attempt<retries,"Domain recovery exhausted: "+original.label+": "+e.what());
            }
        }
        std::cerr<<"Domain "<<(mapping.size()+1)<<"/"<<ordered.size()<<": "<<original.label<<" -> "<<(selected.is_null()?"NEW":"REUSE")<<std::endl;
        std::string chosen=selected.is_null()?id:selected.get<std::string>();mapping[id]=chosen;contexts[id]=premises;
        if(selected.is_null())pool.push_back({id,encoder.encode(original.label,false),Json::array({original.label}),reports[id]});
        else for(auto& candidate:pool)if(candidate.id==chosen){candidate.labels.push_back(original.label);candidate.reports.insert(reports[id].begin(),reports[id].end());}
        auto provenance=answer.provenance;auto dir=fs::path(provenance.at("attempt_directory").get<std::string>());provenance.erase("attempt_directory");
        for(const auto* file:{"request.json","raw.json","completion.json"}){auto ref=store.retain(read_file(dir/file));graph.meta["artifacts"].push_back(ref);graph.meta["retained_attempts"].push_back({{"path",fs::relative(dir/file,store.root).string()},{"artifact",ref}});}
        auto resolution=graph.put("ResolutionAttempt",answer.provenance.at("task_id"),original.label+" → "+graph.nodes.at(chosen).label,
            {{"resolution_kind","domain"},{"source_domain",original.label},{"original_domain",id},{"selected_domain",chosen},{"candidates",packet},{"decision",answer.value},{"provenance",provenance},{"recovery_failures",failures},{"context_reports",premises}},Json(support[id]));
        graph.link("RESOLVED_TO",resolution,chosen,Json::object(),Json(support[id]));decisions.push_back(resolution);
    }
    std::map<std::string,std::string> activity_map;
    for(const auto& [id,n]:source.nodes)if(n.kind=="OrganisationActivity"){
        auto old=n.props.at("domain").get<std::string>(),domain=mapping.at(old),org=n.props.at("organisation").get<std::string>();
        auto target=identity("OrganisationActivity",Json::array({org,domain}));activity_map[id]=target;
        if(!graph.nodes.contains(target))graph.put("OrganisationActivity",Json::array({org,domain}),graph.nodes.at(org).label+" / "+graph.nodes.at(domain).label,{{"organisation",org},{"domain",domain}});
    }
    auto edges=graph.edges;graph.edges.clear();
    for(const auto& [id,e]:edges){(void)id;auto from=e.from,to=e.to;auto props=e.props;
        if(e.kind=="CONCERNS_ACTIVITY"){
            auto old=activity_domain.at(e.to);props["source_domain"]=source.nodes.at(old).label;props["original_domain"]=old;props["domain_context_reports"]=contexts.at(old);
        }
        if(activity_map.contains(from))from=activity_map.at(from);if(activity_map.contains(to))to=activity_map.at(to);
        if(mapping.contains(from))from=mapping.at(from);if(mapping.contains(to))to=mapping.at(to);
        graph.link(e.kind,from,to,props,e.support);
    }
    for(const auto& [old,canonical]:activity_map)if(old!=canonical)graph.nodes.erase(old);
    for(const auto& [old,canonical]:mapping)if(old!=canonical)graph.nodes.erase(old);
    for(const auto& c:pool){auto& n=graph.nodes.at(c.id);n.props["aliases"]=c.labels;n.props["resolution_policy"]="candidate_union_v1";}
    graph.meta["domain_resolution"]={{"policy","candidate_union_v1"},{"config",config},{"input_categories",ordered.size()},{"canonical_categories",pool.size()},{"mapping",mapping},{"decisions",decisions}};
    graph.verify();return graph;
}
}
