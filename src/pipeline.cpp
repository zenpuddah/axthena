#include "axthena/core.hpp"
#include "axthena/investigation.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <iostream>
#include <regex>

namespace axthena {
namespace {
std::string verified_source(const fs::path& root,const Json& ref){
    auto path=fs::weakly_canonical(root/ref.at("path").get<std::string>());
    require(path.string().starts_with(fs::weakly_canonical(root).string()+"/"),"Source reference escapes capture");
    auto bytes=read_file(path);require(bytes.size()==ref.at("bytes")&&sha256(bytes)==ref.at("sha256").get<std::string>(),"Source reference hash mismatch");return bytes;
}
void retain_references(Store& store,const fs::path& root,const Json& value,Json& artifacts){
    if(value.is_object()&&value.contains("path")&&value.contains("sha256")&&value.contains("bytes"))artifacts.push_back(store.retain(verified_source(root,value)));
    if(value.is_structured())for(const auto& child:value)retain_references(store,root,child,artifacts);
}
std::string str(const Json& j,const char* key){require(j.contains(key)&&j.at(key).is_string(),std::string("Missing string: ")+key);return j.at(key);}
void enumeration(const Json& j,const char* key,const std::set<std::string>& values){require(values.contains(str(j,key)),std::string("Unsupported ")+key);}
Json validated_support(const Json& item,const std::string& body){
    require(item.contains("evidence")&&item["evidence"].is_array()&&!item["evidence"].empty(),"Missing evidence");
    Json out=Json::array();for(const auto& p:item["evidence"])out.push_back(locate_passage(body,p));return out;
}
Json top_candidates(Json rows,size_t limit){
    std::sort(rows.begin(),rows.end(),[](const Json& a,const Json& b){return a["score"]==b["score"]?a["id"]<b["id"]:a["score"]>b["score"];});
    if(rows.size()>limit)rows.erase(rows.begin()+static_cast<Json::difference_type>(limit),rows.end());return rows;
}
}

Json import_pilot(Store& store){
    auto pilot=component/"artifacts/pilot",pub=pilot/"slice-1-public-sources";
    auto cap=read_json(pub/"public-capture.json"),text=read_json(pub/"text-v1.json");
    require(cap.at("schema_version")=="axthena.public-source-capture.v1"&&text.at("schema_version")=="axthena.public-source-text.v1","Unsupported source import");
    auto parent_path=pilot/cap.at("plan").at("parent").at("run_id").get<std::string>();
    auto parent_bytes=read_file(parent_path/"collection-manifest.json");auto parent=Json::parse(parent_bytes);
    require(sha256(parent_bytes)==cap["plan"]["parent"]["manifest_sha256"].get<std::string>(),"Parent capture changed");
    require(text["capture_sha256"]==sha256(read_file(pub/"public-capture.json")),"Prepared text input changed");
    auto review=component/"artifacts/pilot/review";
    auto old_review=read_json(review/"body-review.json"),new_review=read_json(review/"public-body-review.json");
    require(old_review.at("manifest_sha256")==sha256(parent_bytes)&&new_review.at("text_manifest_sha256")==sha256(read_file(pub/"text-v1.json")),"Body inspection input changed");
    std::map<int,Json> inspections;for(const auto& r:{old_review,new_review})for(const auto& a:r["articles"])inspections[a.at("inventory_number")]=a;
    Json reports=Json::array(),artifacts=Json::array();int number=0;
    for(auto pair:{std::pair{parent_path,parent},std::pair{pub,text}}){
        bool original=pair.first==parent_path;const auto& manifest=pair.second;
        retain_references(store,pair.first,manifest,artifacts);artifacts.push_back(store.retain(manifest.dump()));
        for(const auto& a:manifest.at("articles")){
            ++number;require(a["snapshots"].size()==1,"Import needs an explicit snapshot choice");const auto& s=a["snapshots"][0];
            auto body=verified_source(pair.first,s.at("body"));auto inspection=inspections.at(number);
            require(inspection.at("body_sha256")==sha256(body),"Body inspection hash mismatch");
            auto url=original?a.at("original_link"):a.at("canonical_url");std::string link=url;auto host_start=link.find("://");require(host_start!=std::string::npos,"Invalid source link");host_start+=3;auto host=link.substr(host_start,link.find('/',host_start)-host_start);
            auto published=s.at("published_at");Json declared=published.value("date",Json(date_only(published.at("raw"))));
            Json r={{"number",number},{"report_id",a.at("report_id")},{"title",s.at("title")},{"url",url},{"source_host",host},
                {"body_text",body},{"body_sha256",sha256(body)},{"body",store.retain(body)},{"inspection",inspection},
                {"published_at",published.at("raw")},{"published_date",declared},{"publication_precision",published.at("precision")},
                {"source_available_at",s.at("source_available_at").at("value")},{"collected_at",s.at("collected_at")},
                {"modified_at",s.value("modified_at",Json::object()).value("raw",Json(nullptr))},
                {"author",s.value("author_supplied",Json(nullptr))},{"source_kind",original?Json("provider_news_item"):a.at("source_kind")},
                {"capture_metadata",store.retain(s.dump())},{"collection_run",pair.first.filename().string()},
                {"body_representation",original?"exact provider content":"nvidia-html-text-v1"}};
            reports.push_back(r);
        }
    }
    retain_references(store,pub,cap,artifacts);
    for(const auto& obj:{cap,old_review,new_review})artifacts.push_back(store.retain(obj.dump()));
    for(const auto& path:{parent_path/"collection-manifest.json",pub/"public-capture.json",pub/"text-v1.json",review/"body-review.json",review/"public-body-review.json"})artifacts.push_back(store.retain(read_file(path)));
    std::set<std::string> ids;for(const auto& r:reports)require(ids.insert(r.at("report_id")).second,"Duplicate source report");
    require(reports.size()==20,"Native pilot must import exactly the retained twenty reports");
    return {{"schema_version","axthena.import.v1"},{"reports",reports},{"source_runs",Json::array({parent,cap})},{"artifacts",artifacts},
            {"source_change","ENG-050: ten EODHD publications + ten public NVIDIA pages; GPT-assisted selection"}};
}

Json validate_extraction(const Json& answer,const Json& report){
    require(answer.at("schema_version")=="axthena.extraction.v1","Unsupported extraction schema");
    require(answer.at("report_id")==report.at("report_id")&&answer.at("body_sha256")==report.at("body_sha256"),"Extraction source identity mismatch");
    enumeration(answer,"status",{"COMPLETE","PARTIAL","INSUFFICIENT_INFORMATION"});
    for(const auto* k:{"organisations","people","resources","ideas","events","claims","relationships","issues","omissions"})require(answer.contains(k)&&answer[k].is_array(),std::string("Missing array: ")+k);
    require(answer["ideas"].size()<=2,"More than two Ideas");
    std::string body=report.at("body_text");Json out=answer;Json failures=Json::array();std::map<std::string,std::string> keys;
    for(const auto* family:{"organisations","people","resources","ideas","events","claims"})for(const auto& item:answer[family]){
        if(!item.is_object()||!item.contains("key")||!item["key"].is_string())continue;
        auto key=str(item,"key");require(!key.empty()&&keys.emplace(key,family).second,"Duplicate/empty extraction key");
    }
    std::set<std::string> valid;
    auto reference=[&](const Json& key,const std::string& family){require(key.is_string()&&valid.contains(key)&&keys.at(key)==family,"Unsupported local subject reference");};
    for(const auto* family:{"organisations","people","resources","ideas","events","claims","relationships"}){
        out[family]=Json::array();
        for(const auto& original:answer[family])try{
            auto item=original;require(item.is_object(),"Malformed component object");std::string f=family;
            if(f!="relationships")require(keys.contains(str(item,"key")),"Missing component key");
            item["evidence"]=validated_support(item,body);
            if(f=="organisations"||f=="people"||f=="resources")require(!str(item,"name").empty(),"Empty entity name");
            if(f=="organisations"){(void)str(item,"description");require(item.contains("aliases")&&item["aliases"].is_array(),"Missing aliases");for(const auto& a:item["aliases"])require(a.is_string(),"Invalid alias");}
            if(f=="resources")enumeration(item,"kind",{"RESOURCE","PROCESS"});
            if(f=="ideas"||f=="events"){
                require(!str(item,"summary").empty()&&item.contains("subjects")&&item["subjects"].is_array()&&!item["subjects"].empty(),"Missing idea/event scope");
                for(const auto& s:item["subjects"]){reference(s.at("organisation"),"organisations");require(s.contains("domain")&&(s["domain"].is_null()||s["domain"].is_string()),"Invalid activity pairing");}
            }
            if(f=="ideas"){
                enumeration(item,"kind",{"NEWS","OPINION"});require(item.contains("judgments")&&item["judgments"].is_array(),"Missing judgment array");
                auto agreement=item.value("agreement",Json(nullptr));require(agreement.is_null()||(agreement.is_string()&&std::set<std::string>{"weak","moderate","strong"}.contains(agreement)),"Invalid expressed agreement");
                if(!item.value("speaker",Json(nullptr)).is_null())reference(item["speaker"],"people");
                Json judgments=Json::array();
                for(auto judgment:item["judgments"])try{
                    enumeration(judgment,"stock",{"NVDA.US","MSFT.US"});enumeration(judgment,"direction",{"UP","DOWN"});
                    require(judgment.contains("quoted")&&judgment["quoted"]==false,"Quoted judgment excluded");
                    judgment["evidence"]=validated_support(judgment,body);
                    require(judgment.contains("conditions")&&(judgment["conditions"].is_null()||judgment["conditions"].is_string()),"Missing condition status");
                    if(judgment["conditions"].is_string())require(body.find(judgment["conditions"].get<std::string>())!=std::string::npos,"Unsupported qualification wording");
                    require(judgment.contains("horizon")&&judgment["horizon"].is_object(),"Missing horizon provenance");
                    const auto& h=judgment.at("horizon");require(h.contains("wording")&&(h["wording"].is_string()||h["wording"].is_null()),"Missing horizon wording status");
                    if(h["wording"].is_string())require(body.find(h["wording"].get<std::string>())!=std::string::npos,"Unsupported horizon wording");
                    (void)resolve_window(h,report.value("published_date",Json(nullptr)),nullptr);
                    judgments.push_back(judgment);
                }catch(const std::exception& e){failures.push_back({{"component","judgment"},{"reason",e.what()}});}
                item["judgments"]=judgments;
            }
            if(f=="events"){
                enumeration(item,"kind",{"ANNOUNCEMENT","PRODUCTION_START","PRODUCT_RELEASE","RESEARCH_RESULT","AGREEMENT","DISRUPTION","OTHER"});
                enumeration(item,"modality",{"REPORTED_OCCURRED","ANNOUNCED_PLAN","FORECAST"});
                if(!item.value("date",Json(nullptr)).is_null())require(!date_only(item["date"]).empty(),"Unsupported event date");
            }
            if(f=="claims"){
                auto subject=str(item,"subject");require(valid.contains(subject)&&(keys.at(subject)=="organisations"||keys.at(subject)=="resources"),"Unsupported claim subject reference");
                (void)str(item,"text");(void)str(item,"predicate");(void)str(item,"object");enumeration(item,"stance",{"ASSERTS","DENIES"});
                if(!item.value("event",Json(nullptr)).is_null())reference(item["event"],"events");
                for(const auto* k:{"period","quantity","unit","currency","basis"})require(item.contains(k)&&(item[k].is_string()||item[k].is_null()),"Invalid claim scope field");
            }
            if(f=="relationships"){
                require(valid.contains(item.at("from"))&&valid.contains(item.at("to")),"Unsupported relationship endpoint");
                enumeration(item,"kind",{"SUBSIDIARY_OF","MEMBER_OF","SUPPLIES","DEPENDS_ON","PARTNERS_WITH","COMPETES_WITH","USES","PRODUCES","OPERATES","REPRESENTS"});
                enumeration(item,"modality",{"REPORTED_OCCURRED","ANNOUNCED_PLAN","FORECAST"});(void)str(item,"description");
                auto from=keys.at(item.at("from")),to=keys.at(item.at("to"));auto relation=item.at("kind").get<std::string>();
                if(relation=="SUBSIDIARY_OF"||relation=="MEMBER_OF"||relation=="PARTNERS_WITH"||relation=="COMPETES_WITH")require(from=="organisations"&&to=="organisations","Corporate relationship requires organisation endpoints");
                if(relation=="REPRESENTS")require(from=="people"&&to=="organisations","Representation requires person and organisation");
            }
            if(item.contains("key"))valid.insert(item.at("key"));out[family].push_back(item);
        }catch(const std::exception& e){failures.push_back({{"component",family},{"key",original.is_object()?original.value("key",Json(nullptr)):Json(nullptr)},{"reason",e.what()}});}
    }
    out["validation_failures"]=failures;
    out["validator_version"]="axthena.validator.v1.1";
    if(!failures.empty())out["status"]="PARTIAL";
    if(out["ideas"].empty()&&out["events"].empty()&&out["claims"].empty())out["status"]="INSUFFICIENT_INFORMATION";
    return out;
}

Graph prepare(Store& store,const Json& imported,const Json& config,bool replay,int max_reports,const std::string& model_executable){
    require(config.at("schema_version")=="axthena.policy.v1","Unsupported policy");
    if(max_reports==0)max_reports=int(imported.at("reports").size());
    require(max_reports>0&&max_reports<=config.value("max_prepare_reports",100000),"Invalid configured preparation bound");
    require(config.at("candidate_limit").is_number_integer()&&config.at("candidate_limit").get<int>()>=1&&config.at("candidate_limit").get<int>()<=32,"Invalid per-branch organisation candidate bound");
    require(config.at("idea_candidate_limit").is_number_integer()&&config.at("idea_candidate_limit").get<int>()>=1&&config.at("idea_candidate_limit").get<int>()<=32,"Invalid meaning candidate bound");
    require(imported.at("schema_version")=="axthena.import.v1"&&imported.at("reports").is_array(),"Unsupported source import");
    std::set<std::string> report_identities;int expected_number=0;
    for(const auto& report:imported.at("reports")){
        require(report.at("number")==++expected_number,"Preparation requires contiguous insertion-order report numbers");
        require(report_identities.insert(report.at("report_id")).second,"Repeated report identity in preparation input");
        require(report.at("body_text")==store.evidence(report.at("body"))&&report.at("body_sha256")==sha256(report.at("body_text").get<std::string>()),"Imported text differs from retained evidence");
    }
    auto resources=resource_root();
    auto extraction_prompt=read_file(resources/"prompts/extract-v1.md"),id_prompt=read_file(resources/"prompts/identity-v1.md"),meaning_prompt=read_file(resources/"prompts/meaning-v1.md");
    Json implementation=Json::object();
    for(const auto& folder:{"src","include","prompts","config"})for(const auto& file:fs::recursive_directory_iterator(resources/folder))if(file.is_regular_file())implementation[fs::relative(file.path(),resources).string()]=store.retain(read_file(file.path()));
    for(const auto* file:{"CMakeLists.txt","CMakePresets.json"})implementation[file]=store.retain(read_file(resources/file));
    auto selected_changes=store.latest_document("owner_change");
    auto owner_changes=selected_changes.is_null()?Json::array():Json::array({selected_changes});
    auto plan=identity("plan",Json::array({imported,config,implementation,max_reports,owner_changes}));
    auto plan_dir=store.root/"preparations"/plan.substr(5);
    if(fs::exists(plan_dir/"published.json")){auto selected=read_json(plan_dir/"published.json").at("run_id").get<std::string>();return store.load(selected);}
    Json started;if(fs::exists(plan_dir/"started.json"))started=read_json(plan_dir/"started.json");else{started={{"plan_id",plan},{"started_at",now()}};write_json(plan_dir/"started.json",started);}
    Graph graph;graph.meta={{"plan_id",plan},{"prepared_at",started.at("started_at")},{"schema_version","axthena.preparation.v1"},{"source_change",imported.at("source_change")},{"policy",config},{"implementation",implementation},{"artifacts",imported.at("artifacts")},{"operations",Json::array()},{"input_reports",imported.at("reports").size()}};
    for(const auto& ref:implementation)graph.meta["artifacts"].push_back(ref);
    for(const auto& data:{config,Json(extraction_prompt),Json(id_prompt),Json(meaning_prompt)})graph.meta["artifacts"].push_back(store.retain(data.dump()));
    auto record_attempt=[&](const ModelAnswer& result){
        auto dir=fs::path(result.provenance.at("attempt_directory").get<std::string>());Json p=result.provenance;
        p.erase("attempt_directory");p["model_output"]=result.value;
        p["request"]=store.retain(read_file(dir/"request.json"));p["response"]=store.retain(read_file(dir/"raw.json"));
        graph.meta["artifacts"].push_back(p["request"]);graph.meta["artifacts"].push_back(p["response"]);
        return graph.put("ProcessingAttempt",p.at("task_id"),p.at("kind"),p);
    };
    std::map<std::string,std::string> stocks;
    for(const auto& stock:config.at("stocks"))stocks[stock.at("symbol")]=graph.put("Stock",stock.at("symbol"),stock.at("symbol"),stock);
    for(const auto& source_run:imported.at("source_runs")){
        auto capture=store.retain(source_run.dump());graph.meta["artifacts"].push_back(capture);
        auto rid=graph.put("CollectionRun",source_run.at("run_id"),source_run.at("run_id"),{{"status",source_run.at("status")},{"started_at",source_run.at("started_at")},{"plan",source_run.at("plan")},{"capture",capture}});
        for(const auto& p:source_run.at("pages"))if(p.at("http_status")!=200){auto f=graph.put("AcquisitionFailure",Json::array({rid,p.at("page_id")}),"Source access failure",p);graph.link("HAD_FAILURE",rid,f);}
    }
    Encoder encoder(config.at("encoder"));Model model(store.root/"attempts",config.at("model"),replay,config.value("attempt_generation","initial"),model_executable,600000,config.value("task_attempts",Json::object()));
    struct Organisation{std::string id,name,description;Json aliases;std::vector<float> vector;};
    std::vector<Organisation> organisations;
    for(const auto& stock:config.at("stocks")){
        std::string name=stock.at("issuer"),description="Configured issuer reference for "+stock.at("symbol").get<std::string>();
        auto id=graph.put("Organisation",Json::array({"configured_issuer",name}),name,{{"description",description},{"aliases",Json::array()},{"provisional",true},{"identity_basis","owner configured reference"}});
        auto vector=encoder.encode(name+". "+description,false);graph.nodes.at(id).props["embedding"]=vector;
        organisations.push_back({id,name,description,Json::array(),vector});
        graph.link("CONFIGURED_ISSUER",stocks.at(stock.at("symbol")),id,{{"basis",stock.at("basis")}});
    }
    std::map<std::string,Json> variants;
    int completed=0, failed=0;
    for(const auto& report:imported.at("reports")){
        auto number=report.at("number").get<int>();if(number>max_reports)break;
        auto report_id=graph.put("Report",report.at("report_id"),report.at("title"),report);
        auto collection_id=identity("CollectionRun",report.at("collection_run"));require(graph.nodes.contains(collection_id),"Source collection lineage missing");graph.link("COLLECTED_BY",report_id,collection_id);
        auto source=graph.put("Source",report.at("source_host"),report.at("source_host"),{{"host",report.at("source_host")},{"identity_basis","source URL host"}});graph.link("PUBLISHED",source,report_id);
        auto body=report.at("body");graph.meta["artifacts"].push_back(body);graph.meta["artifacts"].push_back(report.at("capture_metadata"));
        auto snapshot=graph.put("EvidenceSnapshot",Json::array({report.at("report_id"),body.at("sha256")}),"Retained text",{{"artifact",body},{"report",report_id},{"collected_at",report.at("collected_at")},{"source_available_at",report.at("source_available_at")}});graph.link("HAS_SNAPSHOT",report_id,snapshot);
        std::cerr<<"Processing report "<<number<<"/"<<max_reports<<": "<<report.at("title").get<std::string>()<<std::endl;
        std::string stage="EXTRACTION";
        try{
            auto answer=model.ask("extraction",extraction_prompt,report);auto attempt=record_attempt(answer);graph.link("PROCESSED",attempt,report_id);
            auto extracted=validate_extraction(answer.value,report);
            graph.nodes.at(report_id).props["processing_status"]=extracted.at("status");graph.nodes.at(report_id).props["validation_failures"]=extracted.at("validation_failures");
            graph.nodes.at(report_id).props["issues"]=extracted.at("issues");graph.nodes.at(report_id).props["omissions"]=extracted.at("omissions");
            std::map<std::string,std::string> local;
            auto passages=[&](const Json& evidence){Json ids=Json::array();for(auto p:evidence){p["report"]=report_id;auto id=graph.put("Passage",Json::array({report_id,p.at("begin"),p.at("end")}),p.at("quote"),p);graph.link("IN_SNAPSHOT",id,snapshot);ids.push_back(id);}return ids;};
            for(const auto& organisation:extracted.at("organisations")){
                stage="ORGANISATION_RETRIEVAL_AND_DECISION";
                std::string name=organisation.at("name"),description=organisation.at("description");auto vector=encoder.encode(name+". "+description,true);
                Json names=Json::array(),vectors=Json::array();
                for(const auto& old:organisations){double score=name_similarity(name,old.name);for(const auto& alias:old.aliases)score=std::max(score,name_similarity(name,alias));names.push_back({{"id",old.id},{"score",score}});vectors.push_back({{"id",old.id},{"score",cosine(vector,old.vector)}});}
                names=top_candidates(names,config.at("candidate_limit"));vectors=top_candidates(vectors,config.at("candidate_limit"));auto united=candidate_union(names,vectors);
                for(auto& c:united){const auto& n=graph.nodes.at(c.at("id"));c["canonical_name"]=n.label;c["description"]=n.props.at("description");c["aliases"]=n.props.at("aliases");}
                Json packet={{"mention",organisation},{"report_id",report.at("report_id")},{"name_candidates",names},{"vector_candidates",vectors},{"union",united},{"retrieval_status","COMPLETE"},{"encoder",config.at("encoder")}};
                auto decision=model.ask("organisation_identity",id_prompt,packet);auto decision_node=record_attempt(decision);
                auto selected_identity=select_identity(decision.value,united);
                std::string chosen;
                auto support=passages(organisation.at("evidence"));
                if(selected_identity.is_null()){
                    chosen=graph.put("Organisation",decision.provenance.at("task_id"),name,{{"description",description},{"aliases",organisation.at("aliases")},{"provisional",true},{"uncertain",decision.value["uncertain"]},{"creation_reason",decision.value.value("reason","")}},support);
                    auto embedded=encoder.encode(name+". "+description,false);graph.nodes.at(chosen).props["embedding"]=embedded;organisations.push_back({chosen,name,description,organisation.at("aliases"),embedded});
                }else{
                    chosen=selected_identity;
                    auto& n=graph.nodes.at(chosen);auto aliases=n.props["aliases"];aliases.push_back(name);for(const auto& alias:organisation.at("aliases"))aliases.push_back(alias);
                    std::set<std::string> distinct;for(const auto& alias:aliases)distinct.insert(alias);n.props["aliases"]=distinct;
                    for(auto& old:organisations)if(old.id==chosen)old.aliases=n.props["aliases"];
                }
                local[organisation.at("key")]=chosen;
                auto resolution=graph.put("ResolutionAttempt",decision.provenance.at("task_id"),name+" → "+graph.nodes.at(chosen).label,{{"candidates",packet},{"decision",decision.value},{"selected_organisation",chosen}},support);
                graph.nodes.at(resolution).props["mention_embedding"]=vector;
                graph.link("DECIDED",decision_node,resolution);graph.link("RESOLVED_TO",resolution,chosen,Json::object(),support);graph.link("MENTIONS",report_id,chosen,{{"source_name",name}},support);
                for(const auto& stock:config.at("stocks"))if(normalized(graph.nodes.at(chosen).label)==normalized(stock.at("issuer")))graph.link("CONFIGURED_ISSUER",stocks.at(stock.at("symbol")),chosen,{{"basis",stock.at("basis")}});
            }
            for(const auto* family:{"people","resources"})for(const auto& item:extracted.at(family)){
                auto kind=std::string(family)=="people"?"Person":(item.at("kind")=="PROCESS"?"Process":"Resource");
                auto props=item;props.erase("evidence");props["weight_status"]="UNKNOWN";
                auto id=graph.put(kind,Json::array({report_id,item.at("key")}),item.at("name"),props,passages(item.at("evidence")));local[item.at("key")]=id;graph.link("MENTIONS",report_id,id);
            }
            std::string author;
            if(report.at("author").is_string()){
                author=graph.put("Person",Json::array({report.at("source_host"),report.at("author")}),report.at("author"),{{"identity_basis","publisher author metadata"},{"weight_status","UNKNOWN"},{"credibility",nullptr},{"influence",nullptr},{"predictive_record",nullptr}});graph.link("AUTHORED",author,report_id);
            }
            auto subjects=[&](const std::string& subject,const Json& pairs,const Json& support){
                for(const auto& pair:pairs){auto org=local.at(pair.at("organisation"));graph.link("CONCERNS",subject,org,{{"role","direct_subject"}},support);
                    if(pair.at("domain").is_string()){
                        auto label=normalized(pair["domain"].get<std::string>());auto domain=graph.put("Domain",label,label);
                        auto activity=graph.put("OrganisationActivity",Json::array({org,domain}),graph.nodes.at(org).label+" / "+label,{{"organisation",org},{"domain",domain}});
                        graph.link("HAS_ACTIVITY",org,activity);graph.link("IN_DOMAIN",activity,domain);graph.link("CONCERNS_ACTIVITY",subject,activity,Json::object(),support);
                    }
                }
            };
            for(const auto& idea:extracted.at("ideas")){
                stage="MEANING_MEMBERSHIP";
                auto support=passages(idea.at("evidence"));Json meaning=idea;meaning.erase("evidence");meaning.erase("key");meaning.erase("speaker");meaning.erase("agreement");
                Json orgids=Json::array();for(auto& pair:meaning["subjects"]){auto org=local.at(pair["organisation"]);orgids.push_back(org);pair["organisation"]=org;}meaning["organisation_ids"]=orgids;meaning["source_report"]=report_id;
                for(auto& judgment:meaning["judgments"])judgment["window"]=resolve_window(judgment.at("horizon"),report.at("published_date"),config.at("forecast_fallback_days"));
                auto incoming_vector=encoder.encode(idea.at("summary"),true);Json candidates=Json::array();
                for(const auto& [id,old]:variants){bool shared=false;for(const auto& org:orgids)for(const auto& other:old[0]["organisation_ids"])if(org==other)shared=true;
                    if(shared){auto vec=encoder.encode(old[0]["summary"],false);candidates.push_back({{"id",id},{"score",cosine(incoming_vector,vec)},{"canonical",old[0]},{"variants",old}});}}
                candidates=top_candidates(candidates,config.at("idea_candidate_limit"));
                auto decision=model.ask("meaning_membership",meaning_prompt,{{"incoming",meaning},{"candidates",candidates}});auto decision_node=record_attempt(decision);
                std::string shared=select_meaning(decision.value,candidates,meaning);
                if(shared.empty()){shared=graph.put("Idea",Json::array({report_id,idea.at("key")}),idea.at("summary"),{{"summary",idea.at("summary")},{"kind",idea.at("kind")},{"membership_policy",config.at("membership")}},support);variants[shared]=Json::array();}
                variants[shared].push_back(meaning);
                std::set<std::string> membership_reports{report_id};for(const auto& candidate:candidates)for(const auto& variant:candidate.at("variants"))membership_reports.insert(variant.at("source_report"));
                auto expression=graph.put("Expression",Json::array({report_id,idea.at("key")}),idea.at("summary"),{{"summary",idea.at("summary")},{"kind",idea.at("kind")},{"agreement",idea.value("agreement",Json(nullptr))},{"report",report_id},{"membership",decision.value},{"membership_inputs_reports",membership_reports},{"complete_meaning",meaning}},support);
                graph.nodes.at(expression).props["news_status"]=idea.at("kind")=="NEWS"?Json("REPORTED_UNCONFIRMED; only separately scoped claims may be confirmed"):Json(nullptr);
                graph.nodes.at(expression).props["judgment_status"]=idea.at("judgments").empty()?"NO_SUPPORTED_UP_DOWN":"SUPPORTED_DIRECTION";
                graph.nodes.at(expression).props["agreement_status"]=idea.value("agreement",Json(nullptr)).is_null()?"NOT_EXPRESSED_OR_EXCLUDED":"SOURCE_EXPRESSED";
                graph.link("CONTAINS",report_id,expression);graph.link("EXPRESSES",expression,shared,{{"decision_task",decision.provenance.at("task_id")}},support);graph.link("DECIDED",decision_node,expression);
                subjects(expression,idea.at("subjects"),support);subjects(shared,idea.at("subjects"),support);
                if(idea.value("speaker",Json(nullptr)).is_string())graph.link("ATTRIBUTED_TO",expression,local.at(idea["speaker"]));else if(!author.empty())graph.link("ATTRIBUTED_TO",expression,author,{{"basis","author metadata"}});
                for(size_t j=0;j<idea["judgments"].size();j++){
                    auto judgment=idea["judgments"][j];auto jsupport=passages(judgment.at("evidence"));judgment.erase("evidence");judgment["window"]=resolve_window(judgment.at("horizon"),report.at("published_date"),config.at("forecast_fallback_days"));judgment["summary"]=idea.at("summary");judgment["report"]=report_id;judgment["qualification_status"]=judgment.at("conditions").is_null()?"UNQUALIFIED_IN_SOURCE":"CONDITIONAL";
                    auto id=graph.put("Judgment",Json::array({expression,j}),judgment.at("stock").get<std::string>()+" "+judgment.at("direction").get<std::string>(),judgment,jsupport);
                    graph.link("HAS_JUDGMENT",expression,id);graph.link("TARGETS",id,stocks.at(judgment.at("stock")),Json::object(),jsupport);
                }
            }
            for(const auto& event:extracted.at("events")){
                stage="WORLD_RECORDS";
                auto support=passages(event.at("evidence"));auto props=event;props.erase("evidence");props["report"]=report_id;props["confirmation_status"]="SOURCE_REPORTED";
                auto id=graph.put("Event",Json::array({report_id,event.at("key")}),event.at("summary"),props,support);local[event.at("key")]=id;graph.link("REPORTS_EVENT",report_id,id);subjects(id,event.at("subjects"),support);
            }
            for(const auto& claim:extracted.at("claims")){
                auto support=passages(claim.at("evidence"));auto org=local.at(claim.at("subject"));Json scope={{"subject",org},{"subject_kind",graph.nodes.at(org).kind},{"predicate",claim.at("predicate")},{"object",normalized(claim.at("object"))},{"period",claim.at("period")},{"currency",claim.at("currency")},{"basis",claim.at("basis")}};
                auto id=identity("Claim",scope);if(!graph.nodes.contains(id))graph.put("Claim",scope,claim.at("object"),{{"scope",scope},{"status","REPORTED"}},support);
                if(claim.at("predicate")=="ANNOUNCEMENT")graph.nodes.at(id).props["confirmation_scope"]="Announcement made only; does not independently establish its contents, consequences or future execution";
                auto assertion=claim;assertion.erase("evidence");assertion["report"]=report_id;auto aid=graph.put("Assertion",Json::array({report_id,claim.at("key")}),claim.at("text"),assertion,support);
                graph.link("ASSERTS",report_id,aid);graph.link("ABOUT_CLAIM",aid,id);graph.link("CONCERNS",id,org,Json::object(),support);
                if(claim.value("event",Json(nullptr)).is_string())graph.link("ABOUT_EVENT",aid,local.at(claim["event"]));
                bool qualified=false;for(const auto& authority:config.at("authorities"))if(authority.at("host")==report.at("source_host")&&normalized(authority.at("organisation"))==normalized(graph.nodes.at(org).label)&&authority.at("predicate")==claim.at("predicate")){
                    auto prefix="https://"+report.at("source_host").get<std::string>()+authority.value("path_prefix","/");
                    if(report.at("url").get<std::string>().starts_with(prefix))qualified=true;
                }
                if(qualified){auto confirmation=graph.put("Confirmation",aid,"Scoped announcement confirmation",{{"claim",id},{"stance",claim.at("stance")},{"quantity",claim.at("quantity")},{"unit",claim.at("unit")},{"report",report_id},{"process","configured first-party announcement only"}},support);graph.link("CONFIRMS",confirmation,id);graph.link("SUPPORTED_BY",confirmation,aid);}
            }
            for(const auto& relationship:extracted.at("relationships")){
                auto support=passages(relationship.at("evidence"));auto props=relationship;props.erase("evidence");props["report"]=report_id;props["assertion_status"]="SOURCE_REPORTED";
                graph.link(relationship.at("kind"),local.at(relationship.at("from")),local.at(relationship.at("to")),props,support);
            }
            ++completed;
        }catch(const std::exception& e){
            ++failed;bool readable=false;for(const auto& [id,n]:graph.nodes){(void)id;if((n.kind=="Expression"||n.kind=="Event"||n.kind=="Assertion")&&n.props.value("report","")==report_id)readable=true;}
            std::string state=readable?"PARTIAL":"FAILED";graph.nodes.at(report_id).props["processing_status"]=state;
            graph.meta["operations"].push_back({{"report",report_id},{"stage",stage},{"state",state},{"reason",e.what()}});std::cerr<<"Retained failure: "<<e.what()<<std::endl;
        }
    }
    if(config.value("domain_resolution",std::string("normalized_label_v1"))=="candidate_union_v1")graph=resolve_domains(store,graph,config,replay,model_executable);
    for(const auto& saved:owner_changes){
        const auto& change=saved.at("value");
        for(const auto& raw:change.at("nodes")){Node n=raw;graph.nodes.emplace(n.id,n);}
        for(const auto& raw:change.at("edges")){Edge e=raw;require(graph.nodes.contains(e.from)&&graph.nodes.contains(e.to),"Retained owner revision target absent from this preparation");graph.edges.emplace(e.id,e);}
        for(const auto& ref:change.at("artifacts"))graph.meta["artifacts"].push_back(ref);
    }
    graph.meta["owner_changes"]=owner_changes;
    std::set<std::string> all_reports;for(const auto& [id,n]:graph.nodes)if(n.kind=="Report")all_reports.insert(id);
    for(auto& [id,node]:graph.nodes)if(node.kind=="Claim")node.props.update(claim_state(graph,id,all_reports,"9999-12-31"));
    Json responses=graph.meta.value("retained_attempts",Json::array());
    if(fs::exists(store.root/"attempts"))for(const auto& directory:fs::directory_iterator(store.root/"attempts"))if(directory.is_directory())for(const auto* filename:{"request.json","raw.json","completion.json"})if(fs::exists(directory.path()/filename)){
        auto ref=store.retain(read_file(directory.path()/filename));graph.meta["artifacts"].push_back(ref);responses.push_back({{"path",fs::relative(directory.path()/filename,store.root).string()},{"artifact",ref}});
    }
    std::sort(responses.begin(),responses.end());graph.meta["retained_attempts"]=responses;
    graph.meta["reports_processed"]=completed;graph.meta["reports_failed"]=failed;graph.meta["import_issues"]=imported.value("issues",Json::array());graph.meta["compiler"]=__VERSION__;
    bool partial=failed>0;for(const auto& [id,n]:graph.nodes){(void)id;if(n.kind=="Report"&&n.props.value("processing_status","")!="COMPLETE")partial=true;}
    graph.meta["status"]=partial?"PARTIAL":"PREPARED";
    graph.meta["corpus_id"]=imported.value("corpus_id",Json(nullptr));
    Json coverage={{"input_reports",imported.at("reports").size()},{"attempted_reports",completed+failed},{"unprocessed_reports",int(imported.at("reports").size())-completed-failed},
        {"processing_statuses",Json::object()},{"months",Json::object()},{"source_runs",imported.at("source_runs")},
        {"historical_note","Exact-version first availability is not inferred from publication dates"}};
    for(const auto& [id,n]:graph.nodes)if(n.kind=="Report"){
        (void)id;auto state=n.props.at("processing_status").get<std::string>();auto day=date_only(n.props.value("published_date",Json(nullptr)));
        coverage["processing_statuses"][state]=coverage["processing_statuses"].value(state,0)+1;
        auto key=n.props.at("source_host").get<std::string>()+"/"+(day.empty()?"UNKNOWN":day.substr(0,7));coverage["months"][key]=coverage["months"].value(key,0)+1;
    }
    graph.meta["coverage"]=coverage;
    std::sort(graph.meta["artifacts"].begin(),graph.meta["artifacts"].end());auto& refs=graph.meta["artifacts"];refs.erase(std::unique(refs.begin(),refs.end()),refs.end());
    auto run=identity("run",Json::array({plan,responses}));graph.meta["run_id"]=run;
    graph.verify();store.publish(run,graph);write_json(plan_dir/"published.json",{{"run_id",run},{"model_calls",model.calls},{"model_replays",model.replays}});return graph;
}

Json collect_news(Store& store,const Json& plan,const std::string& token){
    require(plan.at("provider")=="eodhd"&&plan.at("requests").is_array()&&plan.at("requests").size()<=20,"Unsupported native collection plan");
    unsigned budget=0;for(const auto& r:plan["requests"]){require(r.at("limit").is_number_integer()&&r.at("limit").get<int64_t>()>0&&r.at("limit").get<int64_t>()<=20,"Invalid reserved source slots");(void)str(r,"s");budget+=r.at("limit").get<unsigned>();}require(budget<=20&&budget>0,"Collection cap exceeded before requests");
    auto from=str(plan,"from"),to=str(plan,"to");require(from.size()==10&&to.size()==10&&!date_only(from).empty()&&!date_only(to).empty()&&from<=to,"Invalid collection date bounds");
    require(!token.empty(),"EODHD access token is missing");auto marker=store.root/"collection-start.json";require(!fs::exists(marker),"Existing collection attempt; preserve it before continuing");write_json(marker,{{"plan",plan},{"started_at",now()}});
    Json result={{"schema_version","axthena.native-collection.v1"},{"run_id",identity("collection",Json::array({plan,now()}))},{"started_at",now()},{"status","PARTIAL"},{"plan",plan},{"pages",Json::array()}};
    for(const auto& request:plan["requests"]){
        CURL* handle=curl_easy_init();require(handle,"Cannot initialize source transport");
        auto escape=[&](const std::string& s){char* p=curl_easy_escape(handle,s.c_str(),int(s.size()));require(p,"URL encoding failed");std::string value=p;curl_free(p);return value;};
        std::string url="https://eodhd.com/api/news?fmt=json&api_token="+escape(token)+"&s="+escape(request.at("s"))+"&from="+escape(plan.at("from"))+"&to="+escape(plan.at("to"))+"&offset=0&limit="+std::to_string(request.at("limit").get<int>());
        std::string bytes;curl_easy_setopt(handle,CURLOPT_URL,url.c_str());curl_easy_setopt(handle,CURLOPT_TIMEOUT,30L);curl_easy_setopt(handle,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(handle,CURLOPT_WRITEFUNCTION,+[](char* data,size_t size,size_t n,void* p)->size_t{auto& b=*static_cast<std::string*>(p);size_t count=size*n;if(b.size()+count>8*1024*1024)return 0;b.append(data,count);return count;});curl_easy_setopt(handle,CURLOPT_WRITEDATA,&bytes);
        auto rc=curl_easy_perform(handle);long status=0;curl_easy_getinfo(handle,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(handle);
        Json page={{"page_id",std::to_string(result["pages"].size()+1)},{"request",request},{"http_status",status},{"transport_complete",rc==CURLE_OK},{"collected_at",now()}};
        if(bytes.find(token)!=std::string::npos&&token!="demo")page["state"]="CREDENTIAL_ECHO_WITHHELD";
        else{page["response"]=store.retain(bytes);page["state"]=rc!=CURLE_OK?"TRANSPORT_FAILED":status!=200?"HTTP_FAILED":"RETAINED";}
        result["pages"].push_back(page);write_json(store.root/("collection-page-"+std::to_string(result["pages"].size())+".json"),page);
    }
    bool complete=true;for(const auto& page:result["pages"])if(page.at("state")!="RETAINED")complete=false;
    result["status"]=complete?"COMPLETE":"PARTIAL";result["finished_at"]=now();write_json(store.root/"collection-result.json",result);return result;
}

Json decode_news(Store& store,const Json& collection){
    require(collection.at("schema_version")=="axthena.native-collection.v1","Unsupported native source capture");
    Json reports=Json::array(),artifacts=Json::array(),issues=Json::array();std::set<std::string> identities;
    for(const auto& page:collection.at("pages")){
        if(!page.contains("response"))continue;
        artifacts.push_back(page.at("response"));auto bytes=store.evidence(page.at("response"));
        if(page.at("http_status")!=200||!page.at("transport_complete").get<bool>())continue;
        try{
            auto items=Json::parse(bytes);require(items.is_array(),"News response is not an item array");
            require(items.size()<=page.at("request").at("limit").get<size_t>(),"Provider exceeded reserved response slots");
            for(const auto& item:items)try{
                auto url=str(item,"link"),title=str(item,"title"),body=str(item,"content");
                require(url.starts_with("https://")||url.starts_with("http://"),"Unsupported publication URL");
                auto id=identity("eodhd-report",url);
                if(!identities.insert(id).second){issues.push_back({{"report_id",id},{"state","REPEATED_PUBLICATION_OBSERVATION"},{"response",page.at("response")}});continue;}
                require(reports.size()<20,"Native collection exceeds publication cap");
                auto host_start=url.find("://")+3;auto host=url.substr(host_start,url.find('/',host_start)-host_start);
                Json published=item.value("date",Json(nullptr));auto day=date_only(published);auto body_ref=store.retain(body),metadata=store.retain(item.dump());artifacts.push_back(body_ref);artifacts.push_back(metadata);
                reports.push_back({{"number",reports.size()+1},{"report_id",id},{"title",title},{"url",url},{"source_host",host},
                    {"body_text",body},{"body_sha256",sha256(body)},{"body",body_ref},{"capture_metadata",metadata},
                    {"inspection",{{"status",body.empty()?"MISSING_BODY":"PRESENT_COMPLETENESS_UNVERIFIED"},{"note","Native provider capture; no full-publisher-text verification"}}},
                    {"published_at",published},{"published_date",day.empty()?Json(nullptr):Json(day)},{"publication_precision",day.empty()?"UNKNOWN":published.get<std::string>().size()==10?"DATE_ONLY":"PROVIDER_DECLARED_TIMESTAMP"},
                    {"source_available_at",nullptr},{"collected_at",page.at("collected_at")},{"modified_at",nullptr},{"author",item.value("author",Json(nullptr))},
                    {"source_kind","provider_news_item"},{"collection_run",collection.at("run_id")},{"body_representation","exact provider content"}});
            }catch(const std::exception& e){issues.push_back({{"state","INVALID_ITEM"},{"reason",e.what()},{"response",page.at("response")}});}
        }catch(const std::exception& e){issues.push_back({{"state","INVALID_PAGE"},{"reason",e.what()},{"page_id",page.at("page_id")}});}
    }
    artifacts.push_back(store.retain(collection.dump()));
    return {{"schema_version","axthena.import.v1"},{"reports",reports},{"source_runs",Json::array({collection})},{"artifacts",artifacts},{"issues",issues},{"source_change","Explicit native EODHD request; no GPT-generated source text"}};
}
}
