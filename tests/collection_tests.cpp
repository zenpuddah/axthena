#include "axthena/core.hpp"
#include "cleanup.hpp"
#include <iostream>
#include <unistd.h>
using namespace axthena;
namespace {
int checks=0;
void check(bool ok,const char* name){require(ok,std::string("TEST: ")+name);++checks;std::cout<<"PASS "<<name<<'\n';}
template<class F> void rejects(F f,const char* name){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,name);}
Json item(int id,const std::string& content="<p>Same <b>publisher</b> body &amp; evidence.</p>"){
    return {{"id",id},{"link","https://publisher.invalid/article/"+std::to_string(id)},{"date","2025-01-15T12:00:00"},
        {"date_gmt","2025-01-15T10:00:00"},{"title",{{"rendered","Publication "+std::to_string(id)}}},{"content",{{"rendered",content}}}};
}
Json plan(){return {{"schema_version","axthena.public-plan.v1"},{"from","2025-01-01"},{"to","2025-01-31"},{"partition","month"},
    {"page_size",10},{"articles_per_scope",30},{"pages_per_scope",5},{"max_requests",10},
    {"sources",Json::array({{{"name","Fixture"},{"endpoint","https://publisher.invalid/wp-json/wp/v2/posts"}}})}};}
int page_number(const std::string& url){return std::stoi(url.substr(url.find("&page=")+6));}
}
int main(){try{
    auto scratch=component/"artifacts/test-work"/("collection-"+std::to_string(getpid()));
    check(publisher_text("<p>AI<b>chip</b> &amp; &#x1f642;.</p><p>Next<br>line.</p>")=="AIchip & 🙂.\nNext\nline.","Inline boundaries, entities, UTF-8 and paragraphs");
    check(publisher_text("<p>Before</p><script>danger()</script><iframe>media</iframe><p>After</p>")=="Before\nAfter","Active and media content omitted");
    rejects([](){publisher_text("<script>unclosed");},"Malformed omitted element rejected");
    rejects([](){publisher_text("&#x110000;");},"Invalid Unicode entity rejected");
    int calls=0;
    auto fetch=[&](const std::string& url){++calls;int page=page_number(url);Json rows=Json::array();for(int i=(page-1)*10+1;i<=std::min(page*10,26);++i)rows.push_back(item(i));return HttpResponse{200,true,rows.dump(),{{"x-wp-total","26"}}};};
    {Store store(scratch/"store");
        check(store.status().at("state")=="NOT_PREPARED","Empty store status is readable");
        auto first=collect_public(store,plan(),"first",false,fetch);auto corpus=apply_corpus(store,first);
        check(calls==3&&corpus.at("reports").size()==26,"More than twenty publications acquired over three pages");
        check(first["source_runs"][0]["coverage"][0]["stop_reason"]=="EXHAUSTED","Provider exhaustion distinguished from work caps");
        check(corpus["reports"][0]["source_available_at"].is_null(),"Publication date never becomes exact-version availability");
        auto again=apply_corpus(store,collect_public(store,plan(),"first",false,fetch));
        check(calls==3&&again==corpus,"Acquisition restart reuses pages and corpus identity");
        auto next=apply_corpus(store,collect_public(store,plan(),"second",false,fetch));
        check(next["reports"]==corpus["reports"]&&next["observations"].size()==26,"Overlapping batches preserve selected reports and numbering");
        auto changed=apply_corpus(store,collect_public(store,plan(),"changed",false,[&](const std::string&){return HttpResponse{200,true,Json::array({item(1,"<p>Revised body.</p>")}).dump(),Json::object()};}));
        check(changed["reports"]==corpus["reports"]&&changed["observations"].back()["state"]=="UNSELECTED_BODY_VERSION","Changed body is retained without replacing selected evidence");
        auto capped=plan();capped["articles_per_scope"]=2;
        auto cap=collect_public(store,capped,"cap",false,fetch);
        check(cap["reports"].size()==2&&cap["source_runs"][0]["coverage"][0]["stop_reason"]=="ARTICLE_LIMIT","Article limit is explicit coverage truncation");
        int failures=0;auto flaky=[&](const std::string& url){++failures;if(failures==1)return HttpResponse{503,true,"unavailable",Json::object()};return fetch(url);};
        auto failed=collect_public(store,capped,"retry",false,flaky);
        check(failed["reports"].empty()&&failures==1,"HTTP failure is retained without hidden retry");
        auto reused=collect_public(store,capped,"retry",false,flaky);
        check(reused==failed&&failures==1,"Failed acquisition remains inspectable on restart");
        auto retried=collect_public(store,capped,"retry",true,flaky);
        check(retried["reports"].size()==2&&failures==2&&retried["source_runs"][0]["pages"][0]["attempt"]==2,"Explicit retry retains first attempt and captures a second");
        auto repeat=collect_public(store,plan(),"repeat",false,[](const std::string&){Json items=Json::array();for(int i=1;i<=10;++i)items.push_back(item(i));return HttpResponse{200,true,items.dump(),Json::object()};});
        check(repeat["source_runs"][0]["coverage"][0]["stop_reason"]=="REPEATED_PAGE","Nonadvancing pagination cannot imply completeness");
        auto limited=plan();limited["max_requests"]=1;
        auto partial=collect_public(store,limited,"limited",false,fetch);
        check(partial["reports"].size()==10&&partial["source_runs"][0]["coverage"][0]["stop_reason"]=="REQUEST_LIMIT","Request budget stops new HTTP without losing first page");
        partial=collect_public(store,limited,"limited",false,fetch);
        check(partial["reports"].size()==20,"Restart continues from retained page within new request budget");
        partial=collect_public(store,limited,"limited",false,fetch);
        check(partial["reports"].size()==26,"Repeated bounded continuation reaches actual exhaustion");
        auto url=cap["source_runs"][0]["pages"][0]["url"].get<std::string>();
        auto interrupted=store.root/"collections"/identity("acquisition",Json::array({capped,"interrupted"})).substr(12)/"pages"/sha256(url)/"1"/"request.json";
        write_json(interrupted,{{"url",url},{"started_at","2025-01-01"},{"attempt",1}});
        auto stopped=collect_public(store,capped,"interrupted",false,fetch);
        check(stopped["source_runs"][0]["coverage"][0]["stop_reason"]=="INTERRUPTED","Interrupted request requires explicit retry");
        auto continued=collect_public(store,capped,"interrupted",true,fetch);
        check(continued["reports"].size()==2&&read_json(interrupted).at("attempt")==1,"Retry preserves interrupted request record");
        auto outside=item(100);outside["date"]="2024-12-31T23:30:00";
        auto boundary=collect_public(store,capped,"boundary",false,[&](const std::string&){return HttpResponse{200,true,Json::array({outside,item(101)}).dump(),Json::object()};});
        check(boundary["reports"].size()==1&&boundary["reports"][0]["published_date"]=="2025-01-15","Overfetched adjacent-day records are excluded by publisher-local date");
        int exact_calls=0;
        auto exact=collect_public(store,plan(),"exact-pages",false,[&](const std::string& url){++exact_calls;Json rows=Json::array();int p=page_number(url);for(int i=(p-1)*10+1;i<=p*10;++i)rows.push_back(item(i));return HttpResponse{200,true,rows.dump(),{{"x-wp-totalpages","2"},{"x-wp-total",p==1?"21":"20"}}};});
        check(exact_calls==2&&exact["reports"].size()==20&&exact["source_runs"][0]["coverage"][0]["stop_reason"]=="EXHAUSTED","Full last page uses publisher pagination metadata");
        check(exact["source_runs"][0]["coverage"][0]["provider_totals_changed"]==true&&exact["source_runs"][0]["status"]=="BOUNDED_OR_PARTIAL","Changing provider totals prevent a complete-scope claim");
        int malformed_calls=0;
        auto malformed_fetch=[&](const std::string& url){if(++malformed_calls==1)return HttpResponse{200,true,"{broken",Json::object()};return fetch(url);};
        auto malformed=collect_public(store,capped,"malformed",false,malformed_fetch);
        check(malformed["reports"].empty()&&malformed["source_runs"][0]["coverage"][0]["stop_reason"]=="INVALID_PAGE","HTTP success does not hide invalid JSON");
        auto fixed=collect_public(store,capped,"malformed",true,malformed_fetch);
        check(fixed["reports"].size()==2&&malformed_calls==2,"Explicit retry can recover a malformed retained page");
        auto invalid=plan();invalid["from"]="2025-02-30";int before=calls;
        rejects([&](){collect_public(store,invalid,"invalid",false,fetch);},"Invalid plan rejected before HTTP");check(calls==before,"Plan validation performs no network work");
        store.verify();store.backup(scratch/"restore");
    }
    {Store restored(scratch/"restore",true);check(corpus_input(restored).at("reports").size()==26,"Unprepared corpus survives verified backup/restart");}
    cleanup_fixture(scratch);std::cout<<checks<<" public collection checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
