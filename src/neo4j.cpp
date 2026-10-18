#include "axthena/neo4j.hpp"
#include <algorithm>
#include <curl/curl.h>

namespace axthena {
namespace {
Json projected(const Json& props){Json out=Json::object();for(auto it=props.begin();it!=props.end();++it){if(it.value().is_null())continue;if(it.value().is_structured())out["data_"+it.key()+"_json"]=it.value().dump();else out["data_"+it.key()]=it.value();}return out;}
}
Json neo4j_properties(const Node& n,const std::string& store,const std::string& run){
    auto row=projected(n.props);auto payload=Json(n).dump();row.update({{"key",store+"/"+run+"/"+n.id},{"store",store},{"run",run},{"id",n.id},{"kind",n.kind},{"name",n.label},{"payload",payload},{"digest",sha256(payload)},{"support",n.support}});
    for(const auto* field:{"report","symbol","source_host","published_date","source_available_at"})if(n.props.contains(field)&&!n.props.at(field).is_structured()&&!n.props.at(field).is_null())row[field]=n.props.at(field);return row;
}
Json neo4j_properties(const Edge& e,const std::string& store,const std::string& run){auto row=projected(e.props);auto payload=Json(e).dump();row.update({{"id",e.id},{"store",store},{"run",run},{"payload",payload},{"digest",sha256(payload)},{"support",e.support}});return row;}
struct Neo4j::Impl {CURL* handle=curl_easy_init();~Impl(){if(handle)curl_easy_cleanup(handle);}};
Neo4j::Neo4j(const fs::path& configuration):impl_(std::make_unique<Impl>()){
    require(impl_->handle,"Cannot initialize Neo4j HTTP client");
    auto path=configuration.empty()?(std::getenv("AXTHENA_NEO4J_CONFIG")?fs::path(std::getenv("AXTHENA_NEO4J_CONFIG")):component/"artifacts/neo4j/connection.json"):configuration;
    require(fs::exists(path),"Neo4j connection missing; run python3 tools/neo4j_runtime.py start");
    connection_=read_json(path);require(connection_.at("schema_version")=="axthena.neo4j-connection.v1","Unsupported Neo4j connection format");
    auto endpoint=connection_.at("endpoint").get<std::string>();
    require(endpoint.starts_with("http://127.0.0.1:")&&endpoint.find('/',7)==std::string::npos,"V1 Neo4j endpoint must be loopback HTTP");
    auto database=connection_.at("database").get<std::string>();require(!database.empty()&&database.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==std::string::npos,"Invalid Neo4j database name");
    password_=read_file(connection_.at("password_file").get<std::string>());require(!password_.empty(),"Neo4j password file empty");
}
Neo4j::~Neo4j()=default;
Json Neo4j::public_configuration() const {return {{"endpoint",connection_.at("endpoint")},{"database",connection_.at("database")}};}
Json Neo4j::request(const std::string& method,const std::string& suffix,const Json& body) const {
    auto* handle=impl_->handle;curl_easy_reset(handle); // Reset options, retaining the connection pool.
    char error[CURL_ERROR_SIZE]={};curl_easy_setopt(handle,CURLOPT_ERRORBUFFER,error);
    std::string bytes,url=connection_.at("endpoint").get<std::string>()+"/db/"+connection_.at("database").get<std::string>()+"/query/v2"+suffix,payload=body.dump();
    auto* headers=curl_slist_append(nullptr,"Content-Type: application/json");headers=curl_slist_append(headers,"Accept: application/json");
    curl_easy_setopt(handle,CURLOPT_URL,url.c_str());curl_easy_setopt(handle,CURLOPT_HTTPHEADER,headers);
    auto user=connection_.at("user").get<std::string>();curl_easy_setopt(handle,CURLOPT_USERNAME,user.c_str());curl_easy_setopt(handle,CURLOPT_PASSWORD,password_.c_str());
    curl_easy_setopt(handle,CURLOPT_HTTPAUTH,CURLAUTH_BASIC);curl_easy_setopt(handle,CURLOPT_CUSTOMREQUEST,method.c_str());
    curl_easy_setopt(handle,CURLOPT_POSTFIELDS,payload.c_str());curl_easy_setopt(handle,CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(payload.size()));
    curl_easy_setopt(handle,CURLOPT_CONNECTTIMEOUT,5L);curl_easy_setopt(handle,CURLOPT_TIMEOUT,180L);curl_easy_setopt(handle,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(handle,CURLOPT_NOPROXY,"127.0.0.1");
    curl_easy_setopt(handle,CURLOPT_WRITEFUNCTION,+[](char* data,size_t size,size_t n,void* p)->size_t{size_t count=size*n;auto& out=*static_cast<std::string*>(p);if(out.size()+count>512ULL*1024*1024)return 0;out.append(data,count);return count;});curl_easy_setopt(handle,CURLOPT_WRITEDATA,&bytes);
    auto rc=curl_easy_perform(handle);long status=0;curl_easy_getinfo(handle,CURLINFO_RESPONSE_CODE,&status);curl_slist_free_all(headers);
    require(rc==CURLE_OK,"Neo4j transport failed: "+std::string(error[0]?error:curl_easy_strerror(rc)));
    require(status>=200&&status<300,"Neo4j HTTP status "+std::to_string(status));
    auto response=Json::parse(bytes);require(!response.contains("errors")||response.at("errors").empty(),"Neo4j query failed: "+response.value("errors",Json::array()).dump());return response;
}
Json Neo4j::execute(const std::string& statement,const Json& parameters,bool readonly,const std::string& transaction) const {
    Json body={{"statement",statement},{"parameters",parameters}};if(transaction.empty())body["accessMode"]=readonly?"Read":"Write";
    auto response=request("POST",transaction.empty()?"":"/tx/"+transaction,body);
    require(response.contains("data")&&response.at("data").at("fields").is_array()&&response.at("data").at("values").is_array(),"Neo4j response lacks rows");
    Json out=Json::array();const auto& fields=response.at("data").at("fields");
    for(const auto& row:response.at("data").at("values")){require(row.size()==fields.size(),"Neo4j row width mismatch");Json item=Json::object();for(size_t i=0;i<fields.size();++i)item[fields[i].get<std::string>()]=row[i];out.push_back(item);}return out;
}
std::string Neo4j::begin(bool readonly) const {return request("POST","/tx",{{"statement","RETURN 1 AS started"},{"accessMode",readonly?"Read":"Write"}}).at("transaction").at("id");}
void Neo4j::commit(const std::string& transaction) const {(void)request("POST","/tx/"+transaction+"/commit",Json::object());}
void Neo4j::rollback(const std::string& transaction) const noexcept {try{(void)request("DELETE","/tx/"+transaction,Json::object());}catch(...){}}

namespace {
Json verified(const Json& row){auto text=row.at("payload").get<std::string>();require(sha256(text)==row.at("digest").get<std::string>(),"Neo4j record digest mismatch");return Json::parse(text);}
}
Neo4jGraph::Neo4jGraph(const Neo4j& db,std::string store,std::string run,Json meta,std::string transaction):db_(db),store_(std::move(store)),run_(std::move(run)),transaction_(std::move(transaction)),meta_(std::move(meta)){}
Json Neo4jGraph::parameters() const {return {{"store",store_},{"run",run_}};}
bool Neo4jGraph::contains(const std::string& id) const {
    if(nodes_.contains(id))return true;auto p=parameters();p["key"]=store_+"/"+run_+"/"+id;
    auto rows=db_.execute("MATCH (n:AxRecord {key:$key}) RETURN n.payload AS payload,n.digest AS digest,properties(n) AS properties,labels(n) AS labels",p,true,transaction_);
    if(rows.empty())return false;Node n=verified(rows[0]);require(n.id==id&&rows[0].at("properties")==neo4j_properties(n,store_,run_)&&std::find(rows[0].at("labels").begin(),rows[0].at("labels").end(),n.kind)!=rows[0].at("labels").end(),"Neo4j record projection mismatch");nodes_.emplace(id,n);return true;
}
Node Neo4jGraph::node(const std::string& id) const {require(contains(id),"Neo4j graph node not found: "+id);return nodes_.at(id);}
Edge Neo4jGraph::edge(const std::string& id) const {
    if(edges_.contains(id))return edges_.at(id);auto p=parameters();p["id"]=id;
    auto rows=db_.execute("MATCH (:AxRecord {store:$store,run:$run})-[e]->() WHERE e.id=$id RETURN e.payload AS payload,e.digest AS digest",p,true,transaction_);
    require(rows.size()==1,"Neo4j graph edge not found or duplicated");Edge e=verified(rows[0]);edges_.emplace(e.id,e);return e;
}
std::vector<Node> Neo4jGraph::select(const std::vector<std::string>& kinds,const Json& reports) const {
    auto p=parameters();p["kinds"]=kinds;p["reports"]=reports;
    auto rows=db_.execute("MATCH (n:AxRecord {store:$store,run:$run}) WHERE n.kind IN $kinds AND ($reports IS NULL OR n.report IN $reports) RETURN n.payload AS payload,n.digest AS digest,properties(n) AS properties,labels(n) AS labels ORDER BY n.id",p,true,transaction_);
    std::vector<Node> out;for(const auto& row:rows){Node n=verified(row);require(row.at("properties")==neo4j_properties(n,store_,run_)&&std::find(row.at("labels").begin(),row.at("labels").end(),n.kind)!=row.at("labels").end(),"Neo4j selected projection mismatch");nodes_.emplace(n.id,n);out.push_back(std::move(n));}return out;
}
std::vector<Edge> Neo4jGraph::adjacent(const std::string& id,const std::string& direction,const std::vector<std::string>& kinds) const {
    require(direction=="both"||direction=="out"||direction=="in","Invalid adjacency direction");
    auto cache=Json::array({id,direction,kinds}).dump();if(adjacency_.contains(cache))return adjacency_.at(cache);
    auto p=parameters();p["key"]=store_+"/"+run_+"/"+id;p["kinds"]=kinds;
    std::string pattern=direction=="out"?"(n)-[e]->()":direction=="in"?"(n)<-[e]-()":"(n)-[e]-()";
    auto rows=db_.execute("MATCH (n:AxRecord {key:$key}) MATCH "+pattern+" WHERE e.store=$store AND e.run=$run AND (size($kinds)=0 OR type(e) IN $kinds) RETURN DISTINCT e.payload AS payload,e.digest AS digest,e.id AS id,properties(e) AS properties,type(e) AS kind,startNode(e).id AS source,endNode(e).id AS target,startNode(e).store AS source_store,endNode(e).store AS target_store,startNode(e).run AS source_run,endNode(e).run AS target_run ORDER BY id",p,true,transaction_);
    std::vector<Edge> out;for(const auto& row:rows){Edge e=verified(row);require(row.at("properties")==neo4j_properties(e,store_,run_)&&row.at("kind")==e.kind&&row.at("source")==e.from&&row.at("target")==e.to&&row.at("source_store")==store_&&row.at("target_store")==store_&&row.at("source_run")==run_&&row.at("target_run")==run_,"Neo4j adjacency topology/projection mismatch");edges_.emplace(e.id,e);out.push_back(std::move(e));}adjacency_[cache]=out;return out;
}
}
