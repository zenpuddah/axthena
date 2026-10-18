#include "axthena/core.hpp"
#include <algorithm>
#include <queue>
#include <sstream>

namespace axthena {
namespace {
std::string cypher(const Json& j){
    if(j.is_object()){std::string out="{";bool first=true;for(auto it=j.begin();it!=j.end();++it){if(!first)out+=",";first=false;std::string key=it.key();require(key.find('`')==std::string::npos,"Invalid export property key");out+="`"+key+"`:"+cypher(it.value());}return out+"}";}
    if(j.is_array()){std::string out="[";bool first=true;for(const auto& v:j){if(!first)out+=",";first=false;out+=cypher(v);}return out+"]";}
    return j.dump();
}
Json flat_properties(const Node& node,const std::string& run){
    Json p={{"id",run+"/"+node.id},{"native_id",node.id},{"name",node.label},{"run_id",run},{"kind",node.kind},{"support_json",node.support.dump()}};
    for(auto it=node.props.begin();it!=node.props.end();++it){auto key=it.key();if(p.contains(key))key="data_"+key;if(it.value().is_structured())p[key+"_json"]=it.value().dump();else p[key]=it.value();}return p;
}
bool matches_sources(const Json& request,const Node& report){
    if(!request.contains("sources")||request["sources"].empty())return true;
    for(const auto& source:request["sources"])if(source==report.props.at("source_host"))return true;return false;
}
class MemoryGraph final : public GraphAccess {
    const Graph& graph_;
    std::map<std::string,std::vector<Edge>> outgoing_,incoming_;
public:
    explicit MemoryGraph(const Graph& graph):graph_(graph){for(const auto& [id,e]:graph.edges){(void)id;outgoing_[e.from].push_back(e);incoming_[e.to].push_back(e);}}
    const Json& metadata() const override{return graph_.meta;}
    bool contains(const std::string& id) const override{return graph_.nodes.contains(id);}
    Node node(const std::string& id) const override{return graph_.nodes.at(id);}
    Edge edge(const std::string& id) const override{return graph_.edges.at(id);}
    std::vector<Node> select(const std::vector<std::string>& kinds,const Json& reports) const override{
        std::vector<Node> out;for(const auto& [id,n]:graph_.nodes){(void)id;if(std::find(kinds.begin(),kinds.end(),n.kind)!=kinds.end()&&(reports.is_null()||(n.props.contains("report")&&std::find(reports.begin(),reports.end(),n.props.at("report"))!=reports.end())))out.push_back(n);}return out;
    }
    std::vector<Edge> adjacent(const std::string& id,const std::string& direction,const std::vector<std::string>& kinds) const override{
        std::map<std::string,Edge> found;auto add=[&](const auto& map){if(map.contains(id))for(const auto& e:map.at(id))if(kinds.empty()||std::find(kinds.begin(),kinds.end(),e.kind)!=kinds.end())found.emplace(e.id,e);};
        if(direction!="in")add(outgoing_);if(direction!="out")add(incoming_);std::vector<Edge> out;for(const auto& [key,e]:found){(void)key;out.push_back(e);}return out;
    }
};
Json read_claim_state(const GraphAccess& source,const std::string& claim,const std::set<std::string>& eligible,const std::string& cutoff){
    Graph graph;
    for(const auto& e:source.adjacent(claim,"in",{"CONFIRMS","REVIEWS"})){
        auto n=source.node(e.from);graph.nodes.emplace(n.id,n);
        for(const auto& p:n.support){auto passage=source.node(p);graph.nodes.emplace(passage.id,passage);}
    }
    return claim_state(graph,claim,eligible,cutoff);
}
}

Json query(const Graph& graph,const Json& supplied){
    return query(MemoryGraph(graph),supplied);
}
Json query(const GraphAccess& graph,const Json& supplied){
    const auto& meta=graph.metadata();
    Json request=supplied;
    if(request.contains("view_id")){
        auto id=request.at("view_id").get<std::string>();require(graph.contains(id)&&graph.node(id).kind=="ViewVersion","Unknown view version");
        const auto view=graph.node(id);request["sources"]=view.props.at("sources");request["stocks"]=view.props.at("stocks");request["view_version"]=view.props;
    }
    auto mode=request.value("mode",std::string("strict"));require(mode=="strict"||mode=="declared_date", "Unsupported time mode; operational-as-of replay is not implemented");
    auto kind=request.value("query",std::string("Q1"));require(std::set<std::string>{"Q1","Q2","Q3","Q4","Q5","Q6","Q7"}.contains(kind),"Unsupported query");
    auto start=request.value("from",std::string("0001-01-01")),end=request.value("to",std::string("9999-12-31"));
    require(start.size()==10&&end.size()==10&&!date_only(start).empty()&&!date_only(end).empty()&&start<=end,"Invalid inclusive query bounds");
    std::set<std::string> eligible,view_support,selected_reports;Json exclusions=Json::array();
    const auto reports=graph.select({"Report"});
    for(const auto& n:reports){const auto& id=n.id;
        auto source_time=mode=="strict"?n.props.at("source_available_at"):n.props.at("published_date");auto day=date_only(source_time);
        if(day.empty())exclusions.push_back({{"report",id},{"reason","UNKNOWN_EXACT_VERSION_AVAILABILITY"}});
        else if(day>end)exclusions.push_back({{"report",id},{"reason","AFTER_CUTOFF"}});
        else{eligible.insert(id);if(matches_sources(request,n)){view_support.insert(id);if(day>=start)selected_reports.insert(id);}else exclusions.push_back({{"report",id},{"reason","OUTSIDE_SOURCE_VIEW"}});}
    }
    auto supported=[&](const Json& support,const std::set<std::string>& reports){
        for(const auto& p:support){const auto passage=graph.node(p);if(!reports.contains(passage.props.at("report")))return false;}return true;
    };
    auto category_eligible=[&](const Edge& edge){for(const auto& r:edge.props.value("domain_context_reports",Json::array()))if(!view_support.contains(r))return false;return true;};
    bool truncated=false;size_t expansions=0;
    std::set<std::string> subject_orgs;
    auto symbols=request.value("stocks",Json::array());
    require(symbols.is_array(),"Stocks must be a list of configured symbols");
    const auto stock_nodes=graph.select({"Stock"});
    for(const auto& symbol:symbols){bool found=false;for(const auto& n:stock_nodes)if(n.props.at("symbol")==symbol)found=true;require(found,"Unknown configured stock");}
    for(const auto& n:stock_nodes){const auto& id=n.id;
        bool selected=symbols.empty();for(const auto& symbol:symbols)if(n.props.at("symbol")==symbol)selected=true;
        if(selected)for(const auto& e:graph.adjacent(id,"out",{"CONFIGURED_ISSUER"}))subject_orgs.insert(e.to);
    }
    std::set<std::string> reachable=subject_orgs;std::queue<std::pair<std::string,unsigned>> pending;for(const auto& org:subject_orgs)pending.emplace(org,0);
    unsigned max_hops=request.value("max_hops",meta.at("policy").value("max_hops",3U));require(max_hops<=10,"Hop bound exceeds V1 maximum");
    size_t max_expansions=meta.at("policy").value("max_expansions",1000U);
    std::set<std::string> traversable={"SUBSIDIARY_OF","MEMBER_OF","SUPPLIES","DEPENDS_ON","PARTNERS_WITH","COMPETES_WITH","USES","PRODUCES","OPERATES"};
    Json paths=Json::array();std::map<std::string,Json> proof;for(const auto& org:subject_orgs)proof[org]=Json::array();
    if(!(kind=="Q2"||((kind=="Q1"||kind=="Q4")&&!symbols.empty())))while(!pending.empty())pending.pop();
    while(!pending.empty()){
        auto [id,depth]=pending.front();pending.pop();if(depth>=max_hops)continue;
        for(const auto& e:graph.adjacent(id,"both",std::vector<std::string>(traversable.begin(),traversable.end()))){const auto& eid=e.id;
            if(++expansions>max_expansions){truncated=true;while(!pending.empty())pending.pop();break;}
            if(e.support.empty()||!supported(e.support,view_support))continue;
            if(e.props.value("modality",std::string("REPORTED_OCCURRED"))!="REPORTED_OCCURRED")continue;
            auto effective=date_only(e.props.value("date",Json(nullptr)));if(!effective.empty()&&effective>end)continue;
            auto next=e.from==id?e.to:e.from;
            if(reachable.insert(next).second){proof[next]=proof[id];proof[next].push_back(eid);pending.emplace(next,depth+1);paths.push_back({{"subject",next},{"edges",proof[next]},{"basis","evidence-supported relevance path, not causal or price impact"}});}
        }
    }
    std::set<std::string> relevant_reports,roots;Json grouping_exclusions=Json::array();
    auto requested=request.value("id",std::string(""));
    if(kind=="Q3")require(graph.contains(requested)&&graph.node(requested).kind=="Idea","Q3 requires an existing Idea ID");
    if(kind=="Q5"&&!requested.empty())require(graph.contains(requested)&&graph.node(requested).kind=="Claim","Q5 requires a Claim ID");
    auto membership_eligible=[&](const Node& expression){
        for(const auto& report:expression.props.value("membership_inputs_reports",Json::array()))if(!view_support.contains(report))return false;
        return true;
    };
    for(const auto& n:graph.select({"Expression","Event","Assertion"},Json(selected_reports))){const auto& id=n.id;
            auto report=n.props.at("report").get<std::string>();if(!selected_reports.contains(report)||!supported(n.support,selected_reports))continue;
            bool match=symbols.empty();
            for(const auto& e:graph.adjacent(id,"out",{"CONCERNS"}))if(reachable.contains(e.to)&&supported(e.support,selected_reports))match=true;
            auto links_to=[&](const std::string& relation){for(const auto& e:graph.adjacent(id,"out",{relation}))if(e.to==requested)return true;return false;};
            if(kind=="Q3")match=n.kind=="Expression"&&membership_eligible(n)&&links_to("EXPRESSES");
            if(kind=="Q5")match=n.kind=="Assertion"&&(requested.empty()||links_to("ABOUT_CLAIM"));
            if(kind=="Q6"||kind=="Q7")match=false;
            if(request.contains("kind")&&n.props.value("kind","")!=request.at("kind").get<std::string>())match=false;
            if(request.contains("text")&&!request.at("text").get<std::string>().empty()&&normalized(n.label).find(normalized(request.at("text")))==std::string::npos)match=false;
            if(request.contains("domain")&&!request.at("domain").get<std::string>().empty()){
                bool found=false;for(const auto& e:graph.adjacent(id,"out",{"CONCERNS_ACTIVITY"}))if(category_eligible(e)&&graph.node(e.to).props.at("domain")==request.at("domain"))found=true;if(!found)match=false;
            }
            if(request.contains("direction")){
                auto direction=request.at("direction").get<std::string>();require(direction=="ANY"||direction=="UP"||direction=="DOWN","Invalid judgment direction");bool found=false;
                for(const auto& e:graph.adjacent(id,"out",{"HAS_JUDGMENT"})){auto judgment=graph.node(e.to);bool target=symbols.empty()||std::find(symbols.begin(),symbols.end(),judgment.props.at("stock"))!=symbols.end();if(target&&supported(judgment.support,selected_reports)&&(direction=="ANY"||judgment.props.at("direction")==direction))found=true;}if(!found)match=false;
            }
            if(request.value("confirmed_only",false))match=false;
            if(match){relevant_reports.insert(report);roots.insert(id);}
    }
    const auto claims=graph.select({"Claim"});
    if(kind=="Q5"||request.value("confirmed_only",false))for(const auto& n:claims)if(requested.empty()||n.id==requested){const auto& id=n.id;
        const auto assertions=graph.adjacent(id,"in",{"ABOUT_CLAIM"});
        bool has_selected=false;for(const auto& e:assertions)if(selected_reports.contains(graph.node(e.from).props.at("report")))has_selected=true;
        auto state=read_claim_state(graph,id,eligible,end);if(request.value("confirmed_only",false)&&state.at("status")!="CONFIRMED_SCOPED"&&state.at("status")!="OWNER_ACCEPTED")continue;
        if(has_selected){roots.insert(id);for(const auto& e:assertions){auto r=graph.node(e.from).props.at("report").get<std::string>();if(selected_reports.contains(r)){roots.insert(e.from);relevant_reports.insert(r);}}}
    }
    if(kind=="Q6"){
        require(graph.contains(requested),"Trace node not found");const auto n=graph.node(requested);
        bool eligible_trace=supported(n.support,view_support)&&(!n.props.contains("report")||selected_reports.contains(n.props.at("report")));
        if(n.kind=="ProcessingAttempt"){
            eligible_trace=false;for(const auto& e:graph.adjacent(requested,"out",{"PROCESSED"}))if(selected_reports.contains(e.to)){eligible_trace=true;relevant_reports.insert(e.to);}
        }
        if(n.kind=="Report")eligible_trace=selected_reports.contains(requested);
        if(eligible_trace)roots.insert(requested);
        if(n.kind=="Report"&&selected_reports.contains(requested))relevant_reports.insert(requested);
        if(n.props.contains("report")&&selected_reports.contains(n.props.at("report")))relevant_reports.insert(n.props.at("report"));
        for(const auto& p:n.support){auto r=graph.node(p).props.at("report").get<std::string>();if(selected_reports.contains(r))relevant_reports.insert(r);}
    }
    if(kind=="Q7")relevant_reports=selected_reports;
    std::set<std::string> included=roots;
    included.insert(relevant_reports.begin(),relevant_reports.end());
    if(kind=="Q2"){included.insert(reachable.begin(),reachable.end());for(const auto& p:paths)for(const auto& eid:p.at("edges"))for(const auto& support:graph.edge(eid).support){included.insert(support);included.insert(graph.node(support).props.at("report"));}}
    for(int round=0;round<4;++round){std::map<std::string,Edge> relevant_edges;
      for(const auto& id:included)for(const auto& e:graph.adjacent(id))relevant_edges.emplace(e.id,e);
      for(const auto& [eid,e]:relevant_edges){(void)eid;
        // Preserve the original sorted-edge pass: newly reached edges after the
        // current edge are visited now; earlier edges wait for the next round.
        auto include=[&](const std::string& id){if(included.insert(id).second)for(const auto& next:graph.adjacent(id))if(next.id>eid)relevant_edges.emplace(next.id,next);};
        if(!supported(e.support,view_support))continue;
        if(!category_eligible(e)){grouping_exclusions.push_back({{"subject",e.from},{"reason","DOMAIN_CONTEXT_OUTSIDE_ELIGIBLE_VIEW"}});continue;}
        if(e.kind=="EXPRESSES"&&included.contains(e.from)&&!membership_eligible(graph.node(e.from))){grouping_exclusions.push_back({{"expression",e.from},{"reason","GROUPING_CONTEXT_OUTSIDE_ELIGIBLE_VIEW"}});continue;}
        if(included.contains(e.from)&&(e.kind=="CONCERNS"||e.kind=="CONCERNS_ACTIVITY"||e.kind=="EXPRESSES"||e.kind=="ABOUT_CLAIM"||e.kind=="ATTRIBUTED_TO"||e.kind=="TARGETS"||e.kind=="HAS_JUDGMENT"||e.kind=="IN_DOMAIN"||e.kind=="IN_SNAPSHOT"))include(e.to);
        if(included.contains(e.to)&&(e.kind=="PUBLISHED"||e.kind=="CONFIGURED_ISSUER"||(kind=="Q6"&&(e.kind=="PROCESSED"||(e.kind=="DECIDED"&&membership_eligible(graph.node(e.to)))))))include(e.from);
        if(e.kind=="REVIEWS"&&included.contains(e.to)&&date_only(graph.node(e.from).props.at("recorded_at"))<=end)include(e.from);
        if(e.kind=="HAS_WEIGHT_VERSION"&&included.contains(e.from)&&date_only(graph.node(e.to).props.at("recorded_at"))<=end)include(e.to);
    }}
    for(const auto& id:std::vector<std::string>(included.begin(),included.end()))for(const auto& p:graph.node(id).support)if(view_support.contains(graph.node(p).props.at("report")))included.insert(p);
    const auto confirmations=graph.select({"Confirmation"});
    for(const auto& n:confirmations)if(n.props.contains("report")&&eligible.contains(n.props.at("report"))&&included.contains(n.props.at("claim"))){const auto& id=n.id;
        // Retain outside-view conflict disclosure without silently incorporating it as selected evidence.
        if(view_support.contains(n.props.at("report")))included.insert(id);
    }
    Json rows=Json::array(),edges=Json::array(),holds=Json::array();size_t max_results=request.value("limit",meta.at("policy").value("max_results",500U));require(max_results>0&&max_results<=10000,"Invalid result bound");
    for(const auto& n:claims){const auto& id=n.id;
        auto state=read_claim_state(graph,id,eligible,end);if(state.at("status")=="ON_HOLD"&&(included.contains(id)||(kind=="Q5"&&(requested.empty()||requested==id))))holds.push_back({{"claim",id},{"state",state},{"excluded_from_confirmed_only",request.value("confirmed_only",false)}});
    }
    std::vector<std::string> ordered(roots.begin(),roots.end());for(const auto& id:included)if(!roots.contains(id))ordered.push_back(id);
    for(const auto& id:ordered){auto node=graph.node(id);
        if(node.kind=="Claim"){
            Json outside=Json::array();
            for(const auto& c:confirmations)if(c.props.at("claim")==id&&eligible.contains(c.props.at("report"))){
                if(!view_support.contains(c.props.at("report")))outside.push_back({{"confirmation",c},{"basis","outside selected source view; disclosed for conflict checking"}});
            }
            node.props.update(read_claim_state(graph,id,eligible,end));node.props["outside_view_confirmations"]=outside;
            if(request.value("confirmed_only",false)&&node.props["status"]!="CONFIRMED_SCOPED"&&node.props["status"]!="OWNER_ACCEPTED")continue;
        }
        if(node.kind=="Idea")for(const auto& e:graph.adjacent(id,"in",{"EXPRESSES"}))if(included.contains(e.from)&&membership_eligible(graph.node(e.from))){
            const auto expression=graph.node(e.from);node.label=expression.label;node.props["summary"]=expression.props.at("summary");node.props["display_definition_expression"]=expression.id;node.support=expression.support;break;}
        Json evidence=Json::array(),support=Json::array();for(const auto& p:node.support)if(view_support.contains(graph.node(p).props.at("report"))){support.push_back(p);evidence.push_back(graph.node(p));}node.support=support;
        if(node.kind=="Organisation"){
            node.props.erase("embedding");node.props.erase("description");node.props.erase("aliases");
            node.props["name_basis"]="current resolved identity; source-specific spellings are retained on mentions; not a historical legal-identity assertion";
        }
        if(node.kind=="Domain"&&meta.contains("domain_resolution")){node.props.erase("aliases");node.props["name_basis"]="resolved category in selected run; original source activity remains on subject links";}
        if(node.kind=="Expression"&&!membership_eligible(node)){
            node.props.erase("membership");node.props["membership_status"]="WITHHELD_INELIGIBLE_SUBSTANTIVE_CONTEXT";
        }
        if(rows.size()==max_results){truncated=true;break;}Json row=node;row["evidence"]=evidence;rows.push_back(row);
    }
    std::set<std::string> returned;for(const auto& n:rows)returned.insert(n.at("id"));
    std::map<std::string,Edge> result_edges;for(const auto& id:returned)for(const auto& e:graph.adjacent(id,"out"))result_edges.emplace(e.id,e);
    for(const auto& [id,e]:result_edges){(void)id;if(e.kind=="EXPRESSES"&&!membership_eligible(graph.node(e.from)))continue;if(returned.contains(e.from)&&returned.contains(e.to)&&supported(e.support,view_support)&&category_eligible(e))edges.push_back(e);}
    std::sort(grouping_exclusions.begin(),grouping_exclusions.end());grouping_exclusions.erase(std::unique(grouping_exclusions.begin(),grouping_exclusions.end()),grouping_exclusions.end());
    Json spreads=Json::array();for(const auto& id:included)if(graph.node(id).kind=="Idea"){
        std::set<std::string> publications,expressions;for(const auto& e:graph.adjacent(id,"in",{"EXPRESSES"}))if(roots.contains(e.from)&&membership_eligible(graph.node(e.from))){expressions.insert(e.from);publications.insert(graph.node(e.from).props.at("report"));}
        spreads.push_back({{"idea",id},{"distinct_reports",publications.size()},{"expressions",expressions.size()}});
    }
    Json processing=Json::array(),collections=Json::array();
    for(const auto& n:reports){
        if(n.kind=="Report")processing.push_back({{"report",n.id},{"processing_status",n.props.value("processing_status","NOT_PROCESSED")},{"body_status",n.props.value("inspection",Json::object()).value("status","UNKNOWN")},{"issues",n.props.value("issues",Json::array())},{"validation_failures",n.props.value("validation_failures",Json::array())}});
    }
    for(const auto& n:graph.select({"CollectionRun","AcquisitionFailure"}))collections.push_back(n);
    return {{"schema_version","axthena.query-result.v1"},{"run_id",meta.at("run_id")},{"request",request},{"time_mode",mode},
        {"interpretation","current selected-run retrospective analysis; not operational-as-of replay"},{"distinct_reports",relevant_reports.size()},
        {"nodes",rows},{"edges",edges},{"holds",holds},{"spread_counts",spreads},{"relevance_paths",paths},{"truncated",truncated},{"expansions",expansions},{"grouping_exclusions",grouping_exclusions},
        {"coverage",{{"available_reports",meta.value("input_reports",20)},{"eligible_reports",eligible.size()},{"selected_reports",selected_reports.size()},
        {"exclusions",exclusions},{"processing",processing},{"collection",collections},{"processing_failures",meta.value("operations",Json::array())},{"source_limit",meta.value("coverage",Json(nullptr))}}}};
}

void export_graph(const Graph& graph,const fs::path& destination){
    graph.verify();require(fs::weakly_canonical(destination).string().starts_with(fs::weakly_canonical(component/"artifacts").string()+"/"),"Export must remain in component artifacts");
    auto data=graph.json();auto run=graph.meta.at("run_id").get<std::string>();write_json(destination/"graph.json",data);
    Json node_rows=Json::array(),edge_rows=Json::array();std::set<std::string> kinds,relations;
    for(const auto& [id,n]:graph.nodes){(void)id;kinds.insert(n.kind);node_rows.push_back({{"kind",n.kind},{"props",flat_properties(n,run)}});}
    for(const auto& [id,e]:graph.edges){relations.insert(e.kind);Json p={{"id",run+"/"+id},{"run_id",run},{"native_id",id},{"support_json",e.support.dump()},{"properties_json",e.props.dump()}};edge_rows.push_back({{"kind",e.kind},{"from",run+"/"+e.from},{"to",run+"/"+e.to},{"props",p}});}
    std::string loader="// Axthena native semantic projection. One statement. Sequential same-file MERGE; no deletion.\nWITH "+cypher(node_rows)+" AS nodes,\n"+cypher(edge_rows)+" AS edges\n";
    for(const auto& kind:kinds){require(kind.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_")==std::string::npos,"Invalid node kind");loader+="FOREACH (row IN [n IN nodes WHERE n.kind = "+Json(kind).dump()+"] | MERGE (n:AxthenaV1:"+kind+" {id:row.props.id}) SET n = row.props)\n";}
    loader+="WITH edges UNWIND edges AS edge MATCH (a:AxthenaV1 {id:edge.from}), (b:AxthenaV1 {id:edge.to})\n";
    for(const auto& relation:relations){require(relation.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ_")==std::string::npos,"Invalid edge kind");loader+="FOREACH (_ IN CASE WHEN edge.kind = "+Json(relation).dump()+" THEN [1] ELSE [] END | MERGE (a)-[r:"+relation+" {id:edge.props.id}]->(b) SET r = edge.props)\n";}
    loader+="RETURN count(edge) AS relationships;\n";write_once(destination/"load.cypher",loader);
    std::string filter=" WHERE n.run_id = "+Json(run).dump();
    std::string queries="// Run individually. Native queries enforce time/view policies; these inspect the full retained projection.\nMATCH (n:AxthenaV1)"+filter+" RETURN labels(n), count(*);\n\nMATCH p=(r:AxthenaV1:Report)-[:CONTAINS]->(:AxthenaV1:Expression)-[:EXPRESSES]->(:AxthenaV1:Idea)-[:CONCERNS]->(:AxthenaV1:Organisation) WHERE r.run_id = "+Json(run).dump()+" RETURN p;\n\nMATCH p=(:AxthenaV1:Expression)-[:HAS_JUDGMENT]->(n:AxthenaV1:Judgment)-[:TARGETS]->(:AxthenaV1:Stock)"+filter+" RETURN p;\n\nMATCH (n:AxthenaV1:Claim)"+filter+" RETURN n.name,n.status,n.scope_json;\n";
    write_once(destination/"queries.cypher",queries);
    auto html=read_file(resource_root()/"web/index.html"),script=read_file(resource_root()/"web/app.js");
    auto replace=[](std::string& s,const std::string& key,const std::string& value){auto at=s.find(key);require(at!=std::string::npos,"Missing viewer template marker");s.replace(at,key.size(),value);};
    std::string embedded=data.dump(-1,' ',true);for(size_t pos=0;(pos=embedded.find('<',pos))!=std::string::npos;pos+=6)embedded.replace(pos,1,"\\u003c");
    replace(html,"/*__APP__*/",script);replace(html,"__GRAPH__",embedded);write_once(destination/"index.html",html);
    write_json(destination/"manifest.json",{{"schema_version","axthena.semantic-export.v1"},{"run_id",run},{"graph_sha256",sha256(data.dump(2)+"\n")},{"html_sha256",sha256(html)},{"cypher_sha256",sha256(loader)},{"neo4j_executed",false}});
}
}
