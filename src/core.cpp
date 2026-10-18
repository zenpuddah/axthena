#include "axthena/core.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <unistd.h>
#include <mach-o/dyld.h>

namespace axthena {
fs::path resource_root(){
    if(const char* configured=std::getenv("AXTHENA_RESOURCES"))return fs::canonical(configured);
    uint32_t size=0;_NSGetExecutablePath(nullptr,&size);std::vector<char> buffer(size);
    if(_NSGetExecutablePath(buffer.data(),&size)==0){
        auto bundled=fs::canonical(buffer.data()).parent_path().parent_path()/"share/axthena";
        if(fs::exists(bundled/"config/application-v1.json"))return bundled;
    }
    return component;
}
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary); require(bool(in), "Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}
Json read_json(const fs::path& path) { return Json::parse(read_file(path)); }
void write_once(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    if (fs::exists(path)) { require(read_file(path)==bytes, "Immutable output differs: " + path.string()); return; }
    auto temporary=(path.parent_path()/".pending-XXXXXX").string();std::vector<char> name(temporary.begin(),temporary.end());name.push_back('\0');
    int fd=::mkstemp(name.data());require(fd>=0,"Cannot stage " + path.string());
    size_t done=0;
    while (done<bytes.size()) {
        auto n=::write(fd,bytes.data()+done,bytes.size()-done);
        if(n<0&&errno==EINTR)continue;
        if (n<=0) { ::close(fd);::unlink(name.data());throw std::runtime_error("Artifact write failed"); }
        done+=static_cast<size_t>(n);
    }
    int sync=::fsync(fd); ::close(fd);if(sync!=0){::unlink(name.data());throw std::runtime_error("Artifact sync failed");}
    int published=::link(name.data(),path.c_str());auto error=errno;::unlink(name.data());
    if(published!=0){require(error==EEXIST&&read_file(path)==bytes,"Immutable publication failed: "+path.string());}
    int parent=::open(path.parent_path().c_str(),O_RDONLY);require(parent>=0,"Artifact directory open failed");
    sync=::fsync(parent);::close(parent);require(sync==0,"Artifact directory sync failed");
}
void write_json(const fs::path& path, const Json& value) { write_once(path,value.dump(2)+"\n"); }
std::string sha256(const std::string& bytes) {
    require(bytes.size()<=UINT32_MAX,"Artifact exceeds supported single-buffer hash size");
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.data(),static_cast<CC_LONG>(bytes.size()),hash);
    std::ostringstream out; for(auto b:hash) out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);
    return out.str();
}
std::string now() {
    auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm value{}; gmtime_r(&t,&value); std::ostringstream out;
    out<<std::put_time(&value,"%Y-%m-%dT%H:%M:%SZ"); return out.str();
}
std::string identity(const std::string& kind, const Json& key) { return kind+":"+sha256(key.dump()); }
std::string normalized(std::string text) {
    for(char& c:text) if(static_cast<unsigned char>(c)<128) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string out; bool space=false;
    for(unsigned char c:text) {
        if(c<128 && !std::isalnum(c)) { space=!out.empty(); continue; }
        if(space) out+=' '; space=false; out+=static_cast<char>(c);
    }
    return out;
}
static std::chrono::sys_days checked_day(const std::string& text) {
    require(text.size()==10 && text[4]=='-' && text[7]=='-' && text.find_first_not_of("0123456789-")==std::string::npos,"Invalid date precision");
    for(size_t i=0;i<text.size();++i)if(i!=4&&i!=7)require(text[i]>='0'&&text[i]<='9',"Invalid date digits");
    auto y=std::stoi(text.substr(0,4)); auto m=std::stoi(text.substr(5,2)); auto d=std::stoi(text.substr(8,2));
    std::chrono::year_month_day value{std::chrono::year{y},std::chrono::month{unsigned(m)},std::chrono::day{unsigned(d)}};
    require(value.ok(),"Invalid calendar date"); return std::chrono::sys_days{value};
}
std::string date_only(const Json& raw) {
    if(!raw.is_string()) return "";
    auto s=raw.get<std::string>();
    if(s.size()<10 || s[4]!='-' || s[7]!='-') return "";
    auto day=s.substr(0,10); checked_day(day); return day;
}
std::string add_days(const std::string& start, int days) {
    require(days>=0 && days<=36600,"Invalid calendar duration");
    std::chrono::year_month_day d{checked_day(start)+std::chrono::days{days}};
    std::ostringstream out; out<<int(d.year())<<'-'<<std::setw(2)<<std::setfill('0')<<unsigned(d.month())<<'-'<<std::setw(2)<<unsigned(d.day());
    return out.str();
}
Json locate_passage(const std::string& body, const Json& quote) {
    require(quote.is_object() && quote.contains("quote") && quote["quote"].is_string(),"Missing exact quotation");
    auto text=quote["quote"].get<std::string>(); require(!text.empty(),"Empty quotation");
    require(quote.contains("occurrence") && quote["occurrence"].is_number_integer(),"Quotation occurrence required");
    int occurrence=quote["occurrence"]; require(occurrence>0 && occurrence<=1000,"Invalid occurrence");
    size_t pos=0;
    for(int i=0;i<occurrence;i++) { pos=body.find(text,pos); require(pos!=std::string::npos,"Fabricated or mislocated quotation"); if(i+1<occurrence) ++pos; }
    auto boundary=[&](size_t p) { return p==body.size() || (static_cast<unsigned char>(body[p])&0xc0)!=0x80; };
    require(boundary(pos) && boundary(pos+text.size()),"Invalid UTF-8 span boundary");
    return {{"quote",text},{"occurrence",occurrence},{"begin",pos},{"end",pos+text.size()},
            {"unit","UTF8_BYTES"},{"body_sha256",sha256(body)}};
}
Json resolve_window(const Json& horizon, const Json& publication, const Json& fallback) {
    Json out={{"wording",horizon.value("wording",Json(nullptr))},{"start",nullptr},{"end",nullptr},
              {"origin","UNRESOLVED"},{"boundary","[start,end)"}};
    bool has_start=!horizon.value("start",Json(nullptr)).is_null(),has_end=!horizon.value("end",Json(nullptr)).is_null();
    require(has_start==has_end,"Incomplete explicit horizon boundaries");
    if(horizon.contains("start") && horizon["start"].is_string() && horizon.contains("end") && horizon["end"].is_string()) {
        auto start=date_only(horizon["start"]), end=date_only(horizon["end"]);
        require(!start.empty() && !end.empty() && start<end,"Invalid explicit horizon");
        out["start"]=start;out["end"]=end;out["origin"]="SOURCE_EXPLICIT";return out;
    }
    Json days=horizon.value("calendar_days",Json(nullptr)); std::string origin="SOURCE_RELATIVE";
    require(days.is_null()||days.is_number_integer(),"Calendar duration must be an integer or unknown");
    if(days.is_null() && out["wording"].is_null() && !fallback.is_null()) { days=fallback; origin="CONFIGURED_FALLBACK"; }
    if(days.is_number_integer()) {
        int duration=days; require(duration>0 && duration<=36600,"Invalid horizon duration");
        auto anchor=horizon.value("anchor",Json(nullptr));if(anchor.is_null())anchor=publication;
        auto start=date_only(anchor);
        if(!start.empty()) { out["start"]=start; out["end"]=add_days(start,duration); out["origin"]=origin; }
    }
    if(out["start"].is_null()) out["origin"]=out["wording"].is_null()?"UNSTATED_HORIZON":"SOURCE_HORIZON_UNRESOLVED";
    return out;
}
double name_similarity(const std::string& a, const std::string& b) {
    auto x=normalized(a),y=normalized(b); if(x.empty()||y.empty()) return 0;
    std::vector<size_t> row(y.size()+1);std::iota(row.begin(),row.end(),0);
    for(size_t i=1;i<=x.size();i++){size_t prev=row[0];row[0]=i;for(size_t j=1;j<=y.size();j++){auto old=row[j];row[j]=std::min({row[j]+1,row[j-1]+1,prev+(x[i-1]!=y[j-1])});prev=old;}}
    return 1.-double(row.back())/double(std::max(x.size(),y.size()));
}
double cosine(const std::vector<float>& a,const std::vector<float>& b){
    require(!a.empty() && a.size()==b.size(),"Embedding dimension mismatch");double dot=0,aa=0,bb=0;
    for(size_t i=0;i<a.size();i++){require(std::isfinite(a[i])&&std::isfinite(b[i]),"Non-finite embedding");dot+=a[i]*b[i];aa+=a[i]*a[i];bb+=b[i]*b[i];}
    require(aa>0&&bb>0,"Zero embedding");return dot/std::sqrt(aa*bb);
}
Json candidate_union(const Json& names,const Json& vectors){
    std::map<std::string,Json> result;
    for(auto pair: {std::pair{"name",&names},std::pair{"vector",&vectors}})
        for(const auto& c:*pair.second){auto id=c.at("id").get<std::string>();if(!result.contains(id))result[id]={{"id",id},{"name",false},{"vector",false}};result[id][pair.first]=true;}
    Json out=Json::array();for(auto& [id,c]:result){(void)id;c["intersection"]=bool(c["name"])&&bool(c["vector"]);out.push_back(c);}return out;
}
void to_json(Json& j,const Node& n){j={{"id",n.id},{"kind",n.kind},{"label",n.label},{"props",n.props},{"support",n.support}};}
void from_json(const Json& j,Node& n){n={j.at("id"),j.at("kind"),j.at("label"),j.at("props"),j.at("support")};}
void to_json(Json& j,const Edge& e){j={{"id",e.id},{"kind",e.kind},{"from",e.from},{"to",e.to},{"props",e.props},{"support",e.support}};}
void from_json(const Json& j,Edge& e){e={j.at("id"),j.at("kind"),j.at("from"),j.at("to"),j.at("props"),j.at("support")};}
std::string Graph::put(const std::string& kind,const Json& key,const std::string& label,Json props,Json support){
    auto id=identity(kind,key);Node value{id,kind,label,std::move(props),std::move(support)};
    if(nodes.contains(id))require(Json(nodes.at(id))==Json(value),"Conflicting graph node version");else nodes.emplace(id,std::move(value));return id;
}
void Graph::link(const std::string& kind,const std::string& from,const std::string& to,Json props,Json support){
    auto id=identity("edge",Json::array({kind,from,to,props,support}));edges.emplace(id,Edge{id,kind,from,to,std::move(props),std::move(support)});
}
Json Graph::json() const {Json n=Json::array(),e=Json::array();for(const auto& [id,v]:nodes){(void)id;n.push_back(v);}for(const auto& [id,v]:edges){(void)id;e.push_back(v);}return {{"schema_version","axthena.graph.v1"},{"meta",meta},{"nodes",n},{"edges",e}};}
Graph Graph::parse(const Json& value){require(value.at("schema_version")=="axthena.graph.v1","Unsupported graph version");Graph g;g.meta=value.at("meta");for(const auto& n:value.at("nodes")){Node v=n;require(g.nodes.emplace(v.id,v).second,"Duplicate node");}for(const auto& e:value.at("edges")){Edge v=e;require(g.edges.emplace(v.id,v).second,"Duplicate edge");}g.verify();return g;}
void Graph::verify() const {
    const std::set<std::string> kinds={"Report","Source","CollectionRun","AcquisitionFailure","EvidenceSnapshot","Passage","ProcessingAttempt","ResolutionAttempt","Organisation","Stock","Person","PersonWeightVersion","Resource","Process","Domain","OrganisationActivity","Expression","Idea","Judgment","Event","Claim","Assertion","Confirmation","OwnerReview","ViewVersion"};
    for(const auto& [id,n]:nodes){require(id==n.id&&n.props.is_object()&&n.support.is_array(),"Node identity/shape mismatch");for(const auto& p:n.support)require(nodes.contains(p)&&nodes.at(p).kind=="Passage","Missing node support");
        require(kinds.contains(n.kind),"Unsupported graph record kind");
        if(n.props.contains("report")){auto r=n.props.at("report").get<std::string>();require(nodes.contains(r)&&nodes.at(r).kind=="Report","Invalid report reference");}
        if(n.kind=="Report"&&n.props.contains("body_sha256"))require(n.props.at("body_sha256")==sha256(n.props.at("body_text").get<std::string>()),"Report text digest mismatch");
        if(n.kind=="Passage"){const auto& p=n.props;require(p.at("unit")=="UTF8_BYTES","Unsupported passage offset unit");auto r=p.at("report").get<std::string>();require(nodes.contains(r),"Orphan passage report");auto body=nodes.at(r).props.at("body_text").get<std::string>();size_t begin=p.at("begin"),end=p.at("end");require(begin<=end&&end<=body.size()&&body.substr(begin,end-begin)==p.at("quote").get<std::string>()&&sha256(body)==p.at("body_sha256").get<std::string>(),"Passage evidence mismatch");}}
    const std::map<std::string,std::pair<std::string,std::string>> endpoints={{"CONTAINS",{"Report","Expression"}},{"EXPRESSES",{"Expression","Idea"}},{"HAS_JUDGMENT",{"Expression","Judgment"}},{"CONFIRMS",{"Confirmation","Claim"}},{"PUBLISHED",{"Source","Report"}},{"CONFIGURED_ISSUER",{"Stock","Organisation"}},{"SUBSIDIARY_OF",{"Organisation","Organisation"}}};
    const std::set<std::string> relationships={"HAD_FAILURE","PUBLISHED","HAS_SNAPSHOT","PROCESSED","DECIDED","RESOLVED_TO","MENTIONS","CONFIGURED_ISSUER","AUTHORED","HAS_ACTIVITY","IN_DOMAIN","CONCERNS","CONCERNS_ACTIVITY","CONTAINS","EXPRESSES","ATTRIBUTED_TO","HAS_JUDGMENT","TARGETS","REPORTS_EVENT","ASSERTS","ABOUT_CLAIM","ABOUT_EVENT","CONFIRMS","SUPPORTED_BY","COLLECTED_BY","REVIEWS","HAS_WEIGHT_VERSION","HAS_PASSAGE","IN_SNAPSHOT","SUBSIDIARY_OF","MEMBER_OF","SUPPLIES","DEPENDS_ON","PARTNERS_WITH","COMPETES_WITH","USES","PRODUCES","OPERATES","REPRESENTS"};
    for(const auto& [id,e]:edges){require(id==e.id&&nodes.contains(e.from)&&nodes.contains(e.to)&&e.props.is_object()&&e.support.is_array(),"Orphan or malformed edge");for(const auto& p:e.support)require(nodes.contains(p)&&nodes.at(p).kind=="Passage","Missing edge support");
        require(relationships.contains(e.kind),"Unsupported graph relationship kind");
        if(endpoints.contains(e.kind)){auto expected=endpoints.at(e.kind);require(nodes.at(e.from).kind==expected.first&&nodes.at(e.to).kind==expected.second,"Invalid typed edge endpoints");}}
}
}
