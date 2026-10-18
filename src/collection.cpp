#include "axthena/core.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <curl/curl.h>
#include <iomanip>
#include <sstream>

namespace axthena {
namespace {
std::string day_shift(const std::string& day,int offset){
    require(day.size()==10&&date_only(day)==day,"Invalid collection date");
    std::chrono::year_month_day ymd{std::chrono::year{std::stoi(day.substr(0,4))},std::chrono::month{unsigned(std::stoi(day.substr(5,2)))},std::chrono::day{unsigned(std::stoi(day.substr(8,2)))}};
    ymd=std::chrono::year_month_day{std::chrono::sys_days{ymd}+std::chrono::days{offset}};
    std::ostringstream out;out<<std::setw(4)<<std::setfill('0')<<int(ymd.year())<<'-'<<std::setw(2)<<unsigned(ymd.month())<<'-'<<std::setw(2)<<unsigned(ymd.day());return out.str();
}
std::string host(const std::string& url){
    require(url.starts_with("https://"),"Public sources require HTTPS");
    auto end=url.find('/',8);auto value=url.substr(8,end==std::string::npos?end:end-8);
    require(!value.empty()&&value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == std::string::npos,"Invalid public source host");return value;
}
std::string escape(const std::string& text){
    char* p=curl_easy_escape(nullptr,text.data(),int(text.size()));require(p,"URL encoding failed");std::string out=p;curl_free(p);return out;
}
HttpResponse download(const std::string& url){
    HttpResponse response;auto* h=curl_easy_init();require(h,"Cannot initialize public transport");
    curl_easy_setopt(h,CURLOPT_URL,url.c_str());curl_easy_setopt(h,CURLOPT_TIMEOUT,60L);
    curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(h,CURLOPT_USERAGENT,"Axthena/0.2 local research collector");
    curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,+[](char* p,size_t s,size_t n,void* data)->size_t{auto& body=static_cast<HttpResponse*>(data)->body;size_t count=s*n;if(body.size()+count>32*1024*1024)return 0;body.append(p,count);return count;});
    curl_easy_setopt(h,CURLOPT_WRITEDATA,&response);
    curl_easy_setopt(h,CURLOPT_HEADERFUNCTION,(+[](char* p,size_t s,size_t n,void* data)->size_t{
        size_t count=s*n;std::string line(p,count);auto colon=line.find(':');
        if(line.starts_with("HTTP/"))static_cast<HttpResponse*>(data)->headers=Json::object();
        else if(colon!=std::string::npos){auto key=line.substr(0,colon),value=line.substr(colon+1);for(char& c:key)c=char(std::tolower(static_cast<unsigned char>(c)));while(!value.empty()&&std::isspace(static_cast<unsigned char>(value.back())))value.pop_back();while(!value.empty()&&std::isspace(static_cast<unsigned char>(value.front())))value.erase(0,1);static_cast<HttpResponse*>(data)->headers[key]=value;}
        return count;}));
    curl_easy_setopt(h,CURLOPT_HEADERDATA,&response);auto rc=curl_easy_perform(h);
    curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&response.status);curl_easy_cleanup(h);response.complete=rc==CURLE_OK;return response;
}
int bound(const Json& plan,const char* key,int maximum){
    require(plan.contains(key)&&plan.at(key).is_number_integer(),std::string("Missing integer bound: ")+key);
    int n=plan.at(key);require(n>0&&n<=maximum,std::string("Invalid bound: ")+key);return n;
}
void unique_refs(Json& refs){std::sort(refs.begin(),refs.end());refs.erase(std::unique(refs.begin(),refs.end()),refs.end());}
Json capture(Store& store,const fs::path& root,const std::string& url,bool retry,int& calls,int maximum,const PublicFetch& fetch,bool retained_only){
    auto page_id=sha256(url);auto folder=root/"pages"/page_id;int attempt=1;
    while(fs::exists(folder/std::to_string(attempt+1)/"request.json"))++attempt;
    auto dir=folder/std::to_string(attempt);
    if(fs::exists(dir/"response.json")){
        auto old=read_json(dir/"response.json");(void)store.evidence(old.at("response"));
        if((old.at("state")=="RETAINED"&&!fs::exists(dir/"decoding-failed.json"))||!retry)return old;
        ++attempt;dir=folder/std::to_string(attempt);
    }else if(fs::exists(dir/"request.json")){
        if(!retry)return {{"page_id",page_id},{"url",url},{"attempt",attempt},{"state","INTERRUPTED"},{"http_status",0},{"transport_complete",false}};
        ++attempt;dir=folder/std::to_string(attempt);
    }
    if(!retained_only&&calls>=maximum)return {{"page_id",page_id},{"url",url},{"state","REQUEST_LIMIT"},{"http_status",0},{"transport_complete",false}};
    write_json(dir/"request.json",{{"url",url},{"started_at",now()},{"attempt",attempt}});++calls;
    HttpResponse response;std::string error;
    try{response=fetch?fetch(url):download(url);}catch(const std::exception& e){error=e.what();}
    Json result={{"page_id",page_id},{"url",url},{"attempt",attempt},{"http_status",response.status},{"transport_complete",response.complete},
        {"response",store.retain(response.body)},{"headers",response.headers},{"collected_at",now()},
        {"state",!response.complete?"TRANSPORT_FAILED":response.status!=200?"HTTP_FAILED":"RETAINED"},{"error",error},
         {"request_artifact",store.retain(read_file(dir/"request.json"))}};
    if(!response.provenance.empty()){
        result["transport"]="ingestor_capture_replay";
        result["fetch_provenance"]=response.provenance;
        result["collected_at"]=response.provenance.at("body_collected_at");
        result["checked_at"]=response.provenance.at("observed_at");
    }
    write_json(dir/"response.json",result);return result;
}
Json public_report(Store& store,const Json& item,const Json& page,const Json& source,Json& artifacts){
    require(item.at("id").is_number_integer(),"Missing publisher ID");auto url=item.at("link").get<std::string>();
    require(host(url)==host(source.at("endpoint")),"Publication host differs from configured publisher");
    auto title=publisher_text(item.at("title").at("rendered"));auto body=publisher_text(item.at("content").at("rendered"));
    require(!title.empty(),"Missing publisher title");auto date=date_only(item.at("date"));require(!date.empty(),"Missing publisher-local date");
    auto metadata=store.retain(item.dump()),text=store.retain(body);artifacts.push_back(metadata);artifacts.push_back(text);
    return {{"report_id",identity("public-report",Json::array({source.at("endpoint"),item.at("id")}))},{"title",title},{"url",url},{"source_host",host(url)},
        {"body_text",body},{"body_sha256",sha256(body)},{"body",text},{"capture_metadata",metadata},{"response",page.at("response")},
        {"published_at",item.at("date")},{"published_date",date},{"publication_precision","PUBLISHER_LOCAL_TIMESTAMP"},
        {"published_gmt",item.value("date_gmt",Json(nullptr))},{"modified_at",item.value("modified",Json(nullptr))},{"modified_gmt",item.value("modified_gmt",Json(nullptr))},
        {"source_available_at",nullptr},{"collected_at",page.at("collected_at")},{"author",nullptr},{"source_kind","first_party_publisher_api"},
        {"body_representation","publisher-html-fragment-text-v1"},
        {"inspection",{{"status",body.empty()?"MISSING_BODY":"PRESENT_COMPLETENESS_UNVERIFIED"},{"note","Publisher content fragment; media omitted; exact-version first availability unknown"}}}};
}

} // namespace
Json collect_public(Store& store,const Json& plan,const std::string& batch,bool retry,const PublicFetch& fetch,bool retained_only){
    require(!retained_only||bool(fetch),"Retained-only collection requires a capture reader");
    require(plan.at("schema_version")=="axthena.public-plan.v1","Unsupported public collection plan");
    require(!batch.empty()&&batch.size()<=120,"A bounded batch name is required");
    auto from=plan.at("from").get<std::string>(),to=plan.at("to").get<std::string>();
    (void)day_shift(from,0);(void)day_shift(to,0);require(from<=to,"Reversed collection dates");
    int page_size=bound(plan,"page_size",100),article_limit=bound(plan,"articles_per_scope",100000);
    int page_limit=bound(plan,"pages_per_scope",10000),request_limit=bound(plan,"max_requests",100000);
    require(plan.at("partition")=="month"||plan.at("partition")=="window","Unknown date partition");
    require(plan.at("sources").is_array()&&!plan.at("sources").empty()&&plan.at("sources").size()<=100,"Invalid source list");
    std::set<std::string> endpoints;
    for(const auto& source:plan.at("sources")){
        auto endpoint=source.at("endpoint").get<std::string>();(void)host(endpoint);
        require(endpoint.find_first_of("?#")==std::string::npos&&endpoints.insert(endpoint).second,"Invalid or repeated source endpoint");
        require(source.at("name").is_string()&&source.value("search",Json("")).is_string(),"Invalid source configuration");
    }
    Json scopes=Json::array();auto cursor=from;
    while(cursor<=to){
        auto end=to;
        if(plan.at("partition")=="month"){
            auto next=add_days(cursor.substr(0,7)+"-01",32).substr(0,7)+"-01";
            end=std::min(to,day_shift(next,-1));
        }
        scopes.push_back({{"from",cursor},{"to",end}});require(scopes.size()<=1200,"Too many date partitions");cursor=add_days(end,1);
    }
    auto acquisition=identity("acquisition",Json::array({plan,batch}));auto root=store.root/"collections"/acquisition.substr(12);
    auto start=root/"started.json";if(!fs::exists(start))write_json(start,{{"plan",plan},{"batch",batch},{"started_at",now()}});
    auto started=read_json(start);require(started.at("plan")==plan&&started.at("batch")==batch,"Acquisition identity conflict");
    Json pages=Json::array(),coverage=Json::array(),reports=Json::array(),issues=Json::array(),artifacts=Json::array();
    std::set<std::string> publications;int calls=0;
    // Each source/window is independently bounded and its stopping reason is retained.
    for(const auto& source:plan.at("sources"))for(const auto& scope:scopes){
        Json row=scope;row["source"]=source.at("name");row["endpoint"]=source.at("endpoint");row["search"]=source.value("search","");
        int selected=0,seen=0;std::string stop="PAGE_LIMIT";std::set<std::string> page_hashes;Json totals=Json::array();
        for(int page=1;page<=page_limit;++page){
            std::string url=source.at("endpoint");
            url+="?per_page="+std::to_string(page_size)+"&page="+std::to_string(page)+"&orderby=date&order=asc";
            url+="&after="+escape(day_shift(scope.at("from"),-1)+"T00:00:00")+"&before="+escape(add_days(scope.at("to"),1)+"T23:59:59");
            url+="&_fields=id,date,date_gmt,modified,modified_gmt,link,title,content";
            if(!source.value("search","").empty())url+="&search="+escape(source.at("search").get<std::string>());
            auto captured=capture(store,root,url,retry,calls,request_limit,fetch,retained_only);pages.push_back(captured);
            if(captured.contains("response"))artifacts.push_back(captured.at("response"));
            if(captured.contains("request_artifact"))artifacts.push_back(captured.at("request_artifact"));
            if(captured.at("state")!="RETAINED"){stop=captured.at("state");break;}
            try{
                auto bytes=store.evidence(captured.at("response"));auto items=Json::parse(bytes);
                require(items.is_array()&&items.size()<=size_t(page_size),"Invalid public API page shape/size");
                if(items.empty()){stop="EXHAUSTED";break;}
                if(!page_hashes.insert(sha256(bytes)).second){stop="REPEATED_PAGE";break;}
                auto headers=captured.at("headers");if(headers.contains("x-wp-total"))totals.push_back(headers.at("x-wp-total"));
                int total_pages=0;
                if(headers.contains("x-wp-totalpages")){
                    auto value=headers.at("x-wp-totalpages").get<std::string>();
                    require(!value.empty()&&value.find_first_not_of("0123456789")==std::string::npos,"Invalid publisher page count");
                    total_pages=std::stoi(value);require(total_pages>=page,"Publisher page count contradicts response");
                }
                seen+=int(items.size());
                for(const auto& item:items){
                    if(selected>=article_limit)break;
                    try{
                        auto day=date_only(item.at("date"));require(!day.empty(),"Missing publisher-local date");
                        if(day<scope.at("from").get<std::string>()||day>scope.at("to").get<std::string>())continue;
                        auto report=public_report(store,item,captured,source,artifacts);
                        if(!publications.insert(report.at("report_id")).second){issues.push_back({{"state","REPEATED_PUBLICATION"},{"report_id",report.at("report_id")}});continue;}
                        report["number"]=reports.size()+1;reports.push_back(report);++selected;
                    }catch(const std::exception& e){issues.push_back({{"state","INVALID_ITEM"},{"reason",e.what()},{"response",captured.at("response")}});}
                }
                if(selected>=article_limit){stop="ARTICLE_LIMIT";break;}
                if(items.size()<size_t(page_size)||total_pages==page){stop="EXHAUSTED";break;}
            }catch(const std::exception& e){
                stop="INVALID_PAGE";issues.push_back({{"page_id",captured.at("page_id")},{"reason",e.what()}});
                auto marker=root/"pages"/captured.at("page_id").get<std::string>()/std::to_string(captured.at("attempt").get<int>())/"decoding-failed.json";
                if(!fs::exists(marker))write_json(marker,{{"reason",e.what()},{"response",captured.at("response")}});
                break;
            }
        }
        row["selected_reports"]=selected;row["returned_items"]=seen;row["stop_reason"]=stop;row["provider_totals"]=totals;
        row["provider_totals_changed"]=false;for(const auto& total:totals)if(total!=totals.front())row["provider_totals_changed"]=true;
        coverage.push_back(row);
    }
    bool exhausted=issues.empty();for(const auto& row:coverage)if(row.at("stop_reason")!="EXHAUSTED"||row.at("provider_totals_changed")==true)exhausted=false;
    Json result={{"schema_version","axthena.public-collection.v1"},{"acquisition_id",acquisition},{"batch",batch},{"started_at",started.at("started_at")},
        {"plan",plan},{"pages",pages},{"coverage",coverage},{"status",exhausted?"EXHAUSTED":"BOUNDED_OR_PARTIAL"},{"issues",issues}};
    result["run_id"]=identity("collection",result);
    for(auto& report:reports)report["collection_run"]=result.at("run_id");
    artifacts.push_back(store.retain(result.dump()));unique_refs(artifacts);
    Json imported={{"schema_version","axthena.import.v1"},{"reports",reports},{"source_runs",Json::array({result})},{"artifacts",artifacts},{"issues",issues},
        {"source_change","ENG-055 credential-free publisher APIs; deterministic configured date sampling; first-party bias; no generated source prose"}};
    store.document("collection",result.at("run_id"),imported);return imported;
}

Json corpus_input(const Store& store){
    auto selected=store.latest_document("corpus");require(!selected.is_null(),"No corpus: collect a configured batch or explicitly import an input first");
    auto input=selected.at("value");input["corpus_id"]=selected.at("id");return input;
}
Json apply_corpus(Store& store,const Json& imported){
    require(imported.at("schema_version")=="axthena.import.v1"&&imported.at("reports").is_array(),"Unsupported corpus input");
    auto latest=store.latest_document("corpus");
    Json corpus=latest.is_null()?Json{{"schema_version","axthena.import.v1"},{"reports",Json::array()},{"source_runs",Json::array()},{"artifacts",Json::array()},
        {"issues",Json::array()},{"observations",Json::array()},{"applied_imports",Json::array()},{"source_change",imported.at("source_change")}}:latest.at("value");
    auto import_id=identity("import",imported);
    for(const auto& old:corpus.at("applied_imports"))if(old==import_id)return corpus_input(store);
    std::map<std::string,size_t> existing;for(size_t i=0;i<corpus.at("reports").size();++i)existing[corpus["reports"][i].at("report_id")]=i;
    std::map<std::string,Json> runs;for(const auto& run:corpus.at("source_runs"))runs[run.at("run_id")]=run;
    for(const auto& run:imported.at("source_runs")){
        auto id=run.at("run_id").get<std::string>();
        if(runs.contains(id))require(runs.at(id)==run,"Collection identity conflict");
        else{runs[id]=run;corpus["source_runs"].push_back(run);}
    }
    std::set<std::string> incoming;
    for(auto report:imported.at("reports")){
        auto id=report.at("report_id").get<std::string>();require(incoming.insert(id).second,"Duplicate input publication");
        require(runs.contains(report.at("collection_run")),"Missing collection lineage");
        auto body=store.evidence(report.at("body"));require(body==report.at("body_text").get<std::string>()&&sha256(body)==report.at("body_sha256").get<std::string>(),"Corpus body differs from retained evidence");
        (void)store.evidence(report.at("capture_metadata"));
        if(existing.contains(id)){
            const auto& first=corpus["reports"][existing.at(id)];
            corpus["observations"].push_back({{"report_id",id},{"state",first.at("body_sha256")==report.at("body_sha256")?"REPEATED_PUBLICATION":"UNSELECTED_BODY_VERSION"},
                {"selected_body_sha256",first.at("body_sha256")},{"observation",report}});
        }else{report["number"]=corpus["reports"].size()+1;existing[id]=corpus["reports"].size();corpus["reports"].push_back(report);}
        corpus["artifacts"].push_back(report.at("body"));corpus["artifacts"].push_back(report.at("capture_metadata"));
    }
    for(const auto& ref:imported.at("artifacts")){(void)store.evidence(ref);corpus["artifacts"].push_back(ref);}
    corpus["artifacts"].push_back(store.retain(imported.dump()));unique_refs(corpus["artifacts"]);
    for(const auto& issue:imported.value("issues",Json::array()))corpus["issues"].push_back(issue);
    corpus["applied_imports"].push_back(import_id);
    corpus["parent_corpus"]=latest.is_null()?Json(nullptr):latest.at("id");
    auto id=identity("corpus",corpus);store.document("corpus",id,corpus);corpus["corpus_id"]=id;return corpus;
}
}
