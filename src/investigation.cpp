#include "axthena/investigation.hpp"
#include <algorithm>

namespace axthena {
Json investigation_catalog(){return Json::array({
    {{"intent","overview"},{"query","Q1"},{"view","graph"},{"description","Activities, ideas and reported events concerning selected companies"}},
    {{"intent","relationships"},{"query","Q2"},{"view","graph"},{"description","Evidence-supported company connections; paths indicate relevance"}},
    {{"intent","opinions"},{"query","Q1"},{"view","table"},{"description","Source expressions explicitly classified as OPINION"}},
    {{"intent","judgments"},{"query","Q1"},{"view","table"},{"description","Supported absolute stock-price UP/DOWN judgments"}},
    {{"intent","claims"},{"query","Q5"},{"view","table"},{"description","Claims and their scoped confirmation or hold status"}},
    {{"intent","evidence"},{"query","Q6"},{"view","table"},{"description","Trace a selected record to retained source evidence; requires id"}},
    {{"intent","coverage"},{"query","Q7"},{"view","timeline"},{"description","Collection and processing coverage and exclusions"}},
    {{"intent","compare"},{"query","Q1"},{"view","comparison"},{"description","Separate company reads with shared publication IDs visible"}}
});}
Json investigation_plan(const Json& input){
    require(input.is_object(),"Plan must be an object");
    const std::set<std::string> fields={"intent","stocks","sources","from","to","mode","text","domain","id","limit","max_hops"};
    for(auto it=input.begin();it!=input.end();++it)require(fields.contains(it.key()),"Unsupported plan field: "+it.key());
    Json plan={{"intent",input.value("intent","overview")},{"stocks",input.value("stocks",Json::array())},{"sources",input.value("sources",Json::array())},
        {"from",input.value("from","0001-01-01")},{"to",input.value("to","9999-12-31")},{"mode",input.value("mode","strict")},
        {"text",input.value("text","")},{"domain",input.value("domain","")},{"id",input.value("id","")},{"limit",input.value("limit",5000)},{"max_hops",input.value("max_hops",0)}};
    bool known=false;for(const auto& item:investigation_catalog())if(item.at("intent")==plan["intent"])known=true;require(known,"Unknown investigation intent");
    if(plan["intent"]=="relationships"&&plan["max_hops"]==0)plan["max_hops"]=1;
    require(plan["stocks"].is_array()&&plan["stocks"].size()<=2&&plan["sources"].is_array()&&plan["sources"].size()<=20,"Invalid stock/source selection");
    for(const auto& s:plan["stocks"])require(s=="NVDA.US"||s=="MSFT.US","Unknown configured stock");
    for(const auto& s:plan["sources"])require(s.is_string()&&s.get<std::string>().size()<=255,"Invalid source");
    require(plan["mode"]=="strict"||plan["mode"]=="declared_date","Unsupported historical mode");
    for(const auto* k:{"from","to"})require(plan[k].is_string()&&plan[k].get<std::string>().size()==10&&!date_only(plan[k]).empty(),"Invalid date");
    require(plan["from"]<=plan["to"],"Reversed date bounds");
    for(const auto* k:{"text","domain","id"})require(plan[k].is_string()&&plan[k].get<std::string>().size()<=300,"Invalid plan text/identity");
    require(plan["limit"].is_number_integer()&&plan["limit"].get<int>()>0&&plan["limit"].get<int>()<=10000,"Invalid result limit");
    require(plan["max_hops"].is_number_integer()&&plan["max_hops"].get<int>()>=0&&plan["max_hops"].get<int>()<=3,"Invalid hop bound");
    if(plan["intent"]=="evidence")require(!plan["id"].get<std::string>().empty(),"Evidence intent requires a selected record");
    if(plan["intent"]=="compare")require(plan["stocks"].size()==2&&plan["stocks"][0]!=plan["stocks"][1],"Comparison requires two different stocks");
    if(plan["intent"]=="claims"||plan["intent"]=="coverage")require(plan["stocks"].empty()&&plan["text"]==""&&plan["domain"]=="","Claims/coverage use source/date scope; clear company, text and domain filters");
    return plan;
}
Json investigate(Store& store,const Json& supplied,const std::string& run){
    auto plan=investigation_plan(supplied);auto selected=run.empty()?store.status().at("latest_run").get<std::string>():run;
    Json request=plan;request.erase("intent");std::string intent=plan.at("intent");
    for(const auto& item:investigation_catalog())if(item.at("intent")==intent)request["query"]=item.at("query");
    if(intent=="opinions")request["kind"]="OPINION";
    if(intent=="judgments")request["direction"]="ANY";
    Json results=Json::array();
    if(intent=="compare")for(const auto& stock:plan["stocks"]){auto part=request;part["stocks"]=Json::array({stock});results.push_back(store.read(part,selected));}
    else{
        auto result=store.read(request,selected);
        if(intent=="evidence")for(const auto& n:result.at("nodes"))if(n.at("id")==plan.at("id")){
            if(n.at("kind")=="Domain"){auto related=request;related["query"]="Q1";related["domain"]=plan.at("id");related.erase("id");result=store.read(related,selected);break;}
            if(n.at("kind")=="Idea"){auto related=request;related["query"]="Q3";related.erase("text");related.erase("domain");result=store.read(related,selected);break;}
        }
        results.push_back(result);
    }
    Json answer={{"schema_version","axthena.investigation.v1"},{"run_id",selected},{"plan",plan},{"results",results},{"catalog",investigation_catalog()}};
    return answer;
}
}
