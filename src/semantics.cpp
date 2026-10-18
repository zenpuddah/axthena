#include "axthena/core.hpp"
#include <algorithm>
#include <regex>

namespace axthena {
Json select_identity(const Json& decision,const Json& candidates) {
    require(decision.contains("selected_id") && decision.contains("uncertain") && decision["uncertain"].is_boolean()
            && decision.contains("reason") && decision["reason"].is_string(),"Invalid organisation decision");
    if(decision["selected_id"].is_null())return nullptr;
    require(decision["selected_id"].is_string(),"Invalid selected organisation type");
    require(std::any_of(candidates.begin(),candidates.end(),[&](const Json& c){return c.at("id")==decision["selected_id"];}),"Identity selected a non-candidate");
    return decision["selected_id"];
}

namespace {
Json forecast_signature(const Json& meaning) {
    Json result=Json::array();
    for(const auto& j:meaning.at("judgments")) {
        const auto& w=j.at("window");
        result.push_back({{"stock",j.at("stock")},{"direction",j.at("direction")},
                          {"start",w.at("start")},{"end",w.at("end")},
                          {"conditional",!j.at("conditions").is_null()}});
    }
    std::sort(result.begin(),result.end());return result;
}
}

std::string select_meaning(const Json& decision,const Json& candidates,const Json& incoming) {
    require(decision.contains("selected_id")&&decision.contains("comparisons")&&decision["comparisons"].is_array(),"Invalid meaning decision");
    std::map<std::string,Json> comparisons;
    for(const auto& c:decision["comparisons"]) {
        require(c.at("id").is_string()&&c.at("decision").is_string()&&c.at("reason").is_string(),"Malformed meaning comparison");
        require(std::set<std::string>{"SAME","DIFFERENT","UNCERTAIN"}.contains(c.at("decision")),"Unsupported meaning decision");
        require(comparisons.emplace(c.at("id"),c).second,"Duplicate meaning comparison");
    }
    require(comparisons.size()==candidates.size(),"Incomplete meaning comparison");
    for(const auto& c:candidates)require(comparisons.contains(c.at("id")),"Missing meaning candidate");
    if(decision["selected_id"].is_null())return "";
    require(decision["selected_id"].is_string(),"Malformed selected meaning");
    std::string id=decision["selected_id"];
    require(comparisons.contains(id)&&comparisons.at(id).at("decision")=="SAME","Meaning reuse lacks candidate SAME decision");
    for(const auto& c:candidates)if(c.at("id")==id) {
        require(c.at("variants").is_array()&&!c.at("variants").empty(),"Missing material variants");
        // The LLM decides paraphrase/reason equivalence; these non-negotiable fields
        // are independently checked against every member, preventing transitive drift.
        Json all=c.at("variants");all.push_back(c.at("canonical"));
        for(const auto& variant:all) {
            auto subjects=variant.at("organisation_ids"),current=incoming.at("organisation_ids");
            std::sort(subjects.begin(),subjects.end());std::sort(current.begin(),current.end());
            require(subjects==current,"SAME violates complete organisation scope");
            require(variant.at("kind")==incoming.at("kind"),"SAME changes news/opinion meaning");
            require(forecast_signature(variant)==forecast_signature(incoming),"SAME violates stock/direction/window/qualification invariant");
        }
    }
    return id;
}

Json compare_quantity(const Json& quantity,const Json& unit) {
    if(quantity.is_null()&&unit.is_null())return {{"status","NOT_NUMERIC"},{"value",nullptr}};
    if(!quantity.is_string()||!unit.is_string())return {{"status","UNRESOLVED"},{"reason","Quantity/unit incomplete"}};
    auto number=quantity.get<std::string>(),label=normalized(unit.get<std::string>());
    static const std::regex decimal("^-?[0-9]+(\\.[0-9]+)?$");
    if(!std::regex_match(number,decimal)||number.size()>1000)return {{"status","UNRESOLVED"},{"reason","Unsupported exact-decimal spelling"}};
    int power=0;std::string dimension;
    if(label=="million"||label=="millions"){power=6;dimension="scalar";}
    else if(label=="thousand"||label=="thousands"){power=3;dimension="scalar";}
    else if(label=="billion"||label=="billions"){power=9;dimension="scalar";}
    else if(label=="unit"||label=="units"||label=="1"){dimension="scalar";}
    else if(label=="percent"||label=="percentage points"){dimension=label;}
    else return {{"status","UNRESOLVED"},{"reason","Unconfigured unit conversion"}};
    bool negative=number[0]=='-';if(negative)number.erase(0,1);
    auto point=number.find('.');int decimals=point==std::string::npos?0:int(number.size()-point-1);
    if(point!=std::string::npos)number.erase(point,1);
    int scale=decimals-power;
    if(scale<0){number.append(static_cast<size_t>(-scale),'0');scale=0;}
    while(number.size()>1&&number[0]=='0')number.erase(0,1);
    while(scale>0&&number.size()>1&&number.back()=='0'){number.pop_back();--scale;}
    if(number.find_first_not_of('0')==std::string::npos){number="0";negative=false;scale=0;}
    return {{"status","COMPARABLE"},{"value",(negative?"-":"")+number+"e-"+std::to_string(scale)},
            {"dimension",dimension},{"original_quantity",quantity},{"original_unit",unit}};
}

Json claim_state(const Graph& graph,const std::string& claim,const std::set<std::string>& eligible,const std::string& cutoff) {
    std::set<std::string> outcomes,stances;Json confirmations=Json::array(),unresolved=Json::array();
    for(const auto& [id,n]:graph.nodes)if(n.kind=="Confirmation"&&n.props.at("claim")==claim&&eligible.contains(n.props.at("report"))) {
        bool supported=true;for(const auto& p:n.support)if(!eligible.contains(graph.nodes.at(p).props.at("report")))supported=false;
        if(!supported)continue;
        confirmations.push_back(id);std::string stance=n.props.at("stance");stances.insert(stance);
        auto value=compare_quantity(n.props.value("quantity",Json(nullptr)),n.props.value("unit",Json(nullptr)));
        if(value["status"]=="UNRESOLVED")unresolved.push_back({{"confirmation",id},{"comparison",value}});
        else outcomes.insert(stance+":"+(value["status"]=="NOT_NUMERIC"?"proposition":value.at("dimension").get<std::string>()+":"+value.at("value").get<std::string>()));
    }
    std::string status=confirmations.empty()?"REPORTED":!unresolved.empty()?"COMPARISON_UNRESOLVED":outcomes.size()>1?"ON_HOLD":"CONFIRMED_SCOPED";
    if(stances.size()>1)status="ON_HOLD";
    Json reviews=Json::array(),chosen=nullptr;
    for(const auto& [id,n]:graph.nodes)if(n.kind=="OwnerReview"&&n.props.at("claim")==claim&&date_only(n.props.at("recorded_at"))<=cutoff) {
        reviews.push_back(id);if(chosen.is_null()||n.props.at("recorded_at")>chosen.at("recorded_at"))chosen=n.props;
    }
    if(!chosen.is_null()&&status=="ON_HOLD")status=chosen.at("outcome")=="ACCEPT"?"OWNER_ACCEPTED":"OWNER_REJECTED";
    return {{"status",status},{"confirmations",confirmations},{"comparison_unresolved",unresolved},
            {"stances",stances},{"reviews",reviews},{"selected_review",chosen}};
}

Graph revise(const Graph& original,const Json& change) {
    require(change.at("schema_version")=="axthena.review-change.v1","Unsupported review change");
    require(change.at("author").is_string()&&!change.at("author").get<std::string>().empty(),"Named review author required");
    require(change.at("reason").is_string()&&!change.at("reason").get<std::string>().empty(),"Review reason required");
    Graph graph=original;const auto type=change.at("kind").get<std::string>();
    Json payload=change;payload["recorded_at"]=now();payload["parent_run"]=original.meta.at("run_id");
    if(type=="claim_resolution") {
        auto id=change.at("claim").get<std::string>();require(graph.nodes.contains(id)&&graph.nodes.at(id).kind=="Claim","Review claim not found");
        require(change.at("outcome")=="ACCEPT"||change.at("outcome")=="REJECT","Review outcome must be ACCEPT or REJECT");
        std::set<std::string> all;for(const auto& [rid,r]:graph.nodes)if(r.kind=="Report")all.insert(rid);
        require(claim_state(graph,id,all,"9999-12-31").at("status")=="ON_HOLD","Only an unresolved confirmation hold can be resolved");
        require(change.contains("selected_confirmation")&&graph.nodes.contains(change.at("selected_confirmation"))&&graph.nodes.at(change.at("selected_confirmation")).kind=="Confirmation"&&graph.nodes.at(change.at("selected_confirmation")).props.at("claim")==id,"Review must identify its scoped confirmation");
        auto review=graph.put("OwnerReview",payload,"Dated owner resolution",payload);graph.link("REVIEWS",review,id);
        graph.nodes.at(id).props.update(claim_state(graph,id,all,"9999-12-31"));
    }else if(type=="person_weight") {
        auto id=change.at("person").get<std::string>();require(graph.nodes.contains(id)&&graph.nodes.at(id).kind=="Person","Weight person not found");
        require(change.at("status")=="DUMMY"||change.at("status")=="SUPPLIED_PROVISIONAL","Unmeasured supplied weights must be labelled");
        for(const auto* key:{"credibility","influence","predictive_record"})require(change.contains(key)&&(change[key].is_number()||change[key].is_null()),"Missing global weight component");
        require(!change.contains("domain"),"V1 weights are global, not domain-specific");
        auto version=graph.put("PersonWeightVersion",payload,"Supplied global weights",payload);graph.link("HAS_WEIGHT_VERSION",id,version);
    }else if(type=="view") {
        require(change.at("name").is_string()&&change.at("sources").is_array()&&change.at("stocks").is_array(),"Invalid named view");
        for(const auto& source:change.at("sources"))require(source.is_string(),"Invalid view source");
        for(const auto& symbol:change.at("stocks"))require(symbol=="NVDA.US"||symbol=="MSFT.US","Unknown configured stock");
        graph.put("ViewVersion",payload,change.at("name"),payload);
    }else throw std::runtime_error("Unsupported review change kind");
    graph.meta["parent_run"]=original.meta.at("run_id");graph.meta["revision"]=payload;
    graph.meta["run_id"]=identity("run",Json::array({original.meta.at("run_id"),payload}));graph.verify();return graph;
}
}
