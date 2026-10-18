#include "axthena/neo4j.hpp"
#include <algorithm>
#include <fcntl.h>
#include <random>
#include <sys/file.h>
#include <unistd.h>

namespace axthena {
namespace {
Json checked_payload(const Json& row){auto raw=row.at("payload").get<std::string>();require(sha256(raw)==row.at("digest").get<std::string>(),"Neo4j stored payload digest mismatch");return Json::parse(raw);}
Json copy_archive(const fs::path& source,const fs::path& destination){
    auto from=fs::weakly_canonical(source).string(),to=fs::weakly_canonical(destination).string();
    require(from!=to&&!to.starts_with(from+"/")&&!from.starts_with(to+"/"),"Archive source and destination must not overlap");
    Json inventory=Json::object();
    for(const auto& file:fs::recursive_directory_iterator(source))if(file.is_regular_file()){
        require(!file.is_symlink(),"Archive must contain regular retained files, not symlinks");
        auto relative=fs::relative(file.path(),source);auto name=relative.generic_string();
        if(name==".operation.lock"||name=="neo4j-store.json"||name=="backup.json"||name=="database-backup.json"||name=="restore-intent.json"||name.starts_with("store.sqlite"))continue;
        auto bytes=read_file(file.path());inventory[name]={{"sha256",sha256(bytes)},{"bytes",bytes.size()}};write_once(destination/relative,bytes);
    }
    return inventory;
}
void verify_archive(const fs::path& root,const Json& inventory){
    require(inventory.is_object(),"Backup requires an archive inventory");
    for(auto it=inventory.begin();it!=inventory.end();++it){
        fs::path relative(it.key());require(!relative.empty()&&!relative.is_absolute()&&relative==relative.lexically_normal()&&!relative.generic_string().starts_with("../"),"Invalid backup archive path");
        auto path=fs::weakly_canonical(root/relative);require(path.string().starts_with(fs::weakly_canonical(root).string()+"/"),"Backup archive path escapes its root");
        auto bytes=read_file(path);require(it.value().at("bytes")==bytes.size()&&it.value().at("sha256")==sha256(bytes),"Backup archive file missing or corrupted: "+it.key());
    }
}
std::string token(){std::random_device r;std::string value=now();for(int i=0;i<8;++i)value+=std::to_string(r());return sha256(value);}
}
struct Store::Impl {
    Neo4j db;
    std::string space;
    bool readonly=false;
    int lock=-1;
    ~Impl(){if(lock>=0)::close(lock);}
    Json params() const{return {{"store",space}};}
    Json run(const std::string& id,const std::string& tx="") const {
        auto p=params();p["id"]=id;
        auto rows=db.execute(id.empty()?"MATCH (s:AxStore {id:$store}) MATCH (r:AxRun {key:s.latest_run}) RETURN r.id AS id,r.payload AS payload,r.digest AS digest,r.graph_hash AS graph_hash":"MATCH (r:AxRun {store:$store,id:$id}) RETURN r.id AS id,r.payload AS payload,r.digest AS digest,r.graph_hash AS graph_hash",p,true,tx);
        require(rows.size()==1,"No published run");return rows[0];
    }
};
Store::Store(fs::path path,bool readonly):root(fs::weakly_canonical(fs::absolute(path))){
    require(root.string().starts_with(fs::weakly_canonical(component/"artifacts").string()+"/"),"Store must remain under Axthena artifacts");
    require(!fs::exists(root/"store.sqlite"),"Legacy SQLite store is preserved read-only; use migrate --source DIR into a fresh Neo4j store");
    if(!readonly)fs::create_directories(root);
    impl_=std::make_unique<Impl>();impl_->readonly=readonly;
    impl_->lock=::open((root/".operation.lock").c_str(),readonly?(O_RDONLY|O_CLOEXEC):(O_RDWR|O_CREAT|O_CLOEXEC),0600);
    require(impl_->lock>=0&&::flock(impl_->lock,(readonly?LOCK_SH:LOCK_EX)|LOCK_NB)==0,"Another Axthena operation holds this store; use the sequential workflow");
    auto marker=root/"neo4j-store.json";
    if(fs::exists(marker)){
        auto m=read_json(marker);require(m.at("schema_version")=="axthena.neo4j-store.v1"&&m.at("connection")==impl_->db.public_configuration(),"Neo4j namespace connection mismatch");impl_->space=m.at("namespace");
    }else{
        require(!readonly,"Neo4j store is not initialized");impl_->space=token();write_json(marker,{{"schema_version","axthena.neo4j-store.v1"},{"namespace",impl_->space},{"connection",impl_->db.public_configuration()}});
    }
    if(!readonly){
        for(const auto& statement:{
            "CREATE CONSTRAINT ax_store_key IF NOT EXISTS FOR (n:AxStore) REQUIRE n.id IS UNIQUE",
            "CREATE CONSTRAINT ax_run_key IF NOT EXISTS FOR (n:AxRun) REQUIRE n.key IS UNIQUE",
            "CREATE CONSTRAINT ax_record_key IF NOT EXISTS FOR (n:AxRecord) REQUIRE n.key IS UNIQUE",
            "CREATE CONSTRAINT ax_document_key IF NOT EXISTS FOR (n:AxDocument) REQUIRE n.key IS UNIQUE",
            "CREATE INDEX ax_run_lookup IF NOT EXISTS FOR (n:AxRun) ON (n.store,n.id)",
            "CREATE INDEX ax_record_kind IF NOT EXISTS FOR (n:AxRecord) ON (n.store,n.run,n.kind)",
            "CREATE INDEX ax_record_report IF NOT EXISTS FOR (n:AxRecord) ON (n.store,n.run,n.report)",
            "CREATE INDEX ax_document_kind IF NOT EXISTS FOR (n:AxDocument) ON (n.store,n.kind)"})impl_->db.execute(statement,Json::object(),false);
        impl_->db.execute("MERGE (s:AxStore {id:$store}) ON CREATE SET s.format=1,s.sequence=0 RETURN s.format AS format",impl_->params(),false);
    }
    auto rows=impl_->db.execute("MATCH (s:AxStore {id:$store}) RETURN s.format AS format",impl_->params());require(rows.size()==1&&rows[0].at("format")==1,"Unknown Neo4j namespace/format");
}
Store::~Store()=default;
Json Store::retain(const std::string& bytes){require(!impl_->readonly,"Read-only store");auto hash=sha256(bytes);write_once(root/"evidence"/(hash+".bin"),bytes);return {{"sha256",hash},{"bytes",bytes.size()}};}
std::string Store::evidence(const Json& ref) const {auto hash=ref.at("sha256").get<std::string>();require(hash.size()==64&&hash.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid artifact identity");auto bytes=read_file(root/"evidence"/(hash+".bin"));require(bytes.size()==ref.at("bytes")&&sha256(bytes)==hash,"Evidence corrupted");return bytes;}
bool Store::contains(const std::string& run) const {auto p=impl_->params();p["id"]=run;return !impl_->db.execute("MATCH (r:AxRun {store:$store,id:$id}) RETURN r.id AS id",p).empty();}
void Store::publish(const std::string& run,const Graph& graph,const std::function<void()>& before_commit){
    require(!impl_->readonly,"Read-only store");graph.verify();require(graph.meta.at("run_id")==run,"Published run identity mismatch");
    if(contains(run)){require(load(run).json()==graph.json(),"Existing run differs");return;}
    for(const auto& ref:graph.meta.value("artifacts",Json::array()))(void)evidence(ref);
    Neo4jTransaction tx(impl_->db);auto p=impl_->params();p["run"]=run;p["key"]=impl_->space+"/"+run;
    p["payload"]=graph.meta.dump();p["digest"]=sha256(graph.meta.dump());p["graph_hash"]=sha256(graph.json().dump());p["created"]=now();
    impl_->db.execute("MATCH (s:AxStore {id:$store}) SET s.sequence=s.sequence+1 CREATE (r:AxRun {key:$key,store:$store,id:$run,sequence:s.sequence,payload:$payload,digest:$digest,graph_hash:$graph_hash,created:$created}) RETURN r.id AS id",p,false,tx.id);
    std::map<std::string,Json> groups;
    for(const auto& [id,n]:graph.nodes){
        require(n.kind.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_")==std::string::npos,"Invalid node label");
        (void)id;auto row=neo4j_properties(n,impl_->space,run);
        if(!groups.contains(n.kind))groups[n.kind]=Json::array();groups[n.kind].push_back(row);
    }
    for(const auto& [kind,rows]:groups)for(size_t start=0;start<rows.size();start+=128){p["rows"]=Json::array();for(size_t i=start;i<std::min(start+128,rows.size());++i)p["rows"].push_back(rows[i]);
        impl_->db.execute("UNWIND $rows AS row CREATE (n:AxRecord:"+kind+") SET n=row RETURN count(n) AS count",p,false,tx.id);
    }
    groups.clear();
    for(const auto& [id,e]:graph.edges){require(e.kind.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ_")==std::string::npos,"Invalid relationship type");
        (void)id;auto props=neo4j_properties(e,impl_->space,run);
        Json row={{"from",impl_->space+"/"+run+"/"+e.from},{"to",impl_->space+"/"+run+"/"+e.to},{"props",props}};
        if(!groups.contains(e.kind))groups[e.kind]=Json::array();groups[e.kind].push_back(row);
    }
    for(const auto& [kind,rows]:groups)for(size_t start=0;start<rows.size();start+=256){p["rows"]=Json::array();for(size_t i=start;i<std::min(start+256,rows.size());++i)p["rows"].push_back(rows[i]);
        auto result=impl_->db.execute("UNWIND $rows AS row MATCH (a:AxRecord {key:row.from}),(b:AxRecord {key:row.to}) CREATE (a)-[e:"+kind+"]->(b) SET e=row.props RETURN count(e) AS count",p,false,tx.id);require(result[0].at("count")==p["rows"].size(),"Neo4j publication lost endpoints");
    }
    impl_->db.execute("MATCH (s:AxStore {id:$store}) SET s.latest_run=$key RETURN s.id AS id",p,false,tx.id);
    if(before_commit)before_commit();tx.commit();
}
Graph Store::load(const std::string& run) const {
    auto record=impl_->run(run);Graph g;g.meta=checked_payload(record);auto p=impl_->params();p["run"]=record.at("id");
    for(const auto& row:impl_->db.execute("MATCH (n:AxRecord {store:$store,run:$run}) RETURN n.payload AS payload,n.digest AS digest,properties(n) AS properties,labels(n) AS labels ORDER BY n.id",p)){Node n=checked_payload(row);require(row.at("properties")==neo4j_properties(n,impl_->space,record.at("id"))&&std::find(row.at("labels").begin(),row.at("labels").end(),n.kind)!=row.at("labels").end(),"Neo4j node projection mismatch");require(g.nodes.emplace(n.id,n).second,"Duplicate stored node");}
    for(const auto& row:impl_->db.execute("MATCH (a:AxRecord {store:$store,run:$run})-[e]->(b:AxRecord) RETURN e.payload AS payload,e.digest AS digest,properties(e) AS properties,type(e) AS kind,a.id AS source,b.id AS target,b.store AS target_store,b.run AS target_run ORDER BY e.id",p)){
        Edge e=checked_payload(row);require(row.at("properties")==neo4j_properties(e,impl_->space,record.at("id"))&&e.kind==row.at("kind").get<std::string>()&&e.from==row.at("source").get<std::string>()&&e.to==row.at("target").get<std::string>()&&row.at("target_store")==impl_->space&&row.at("target_run")==record.at("id"),"Neo4j relationship topology differs from retained payload");require(g.edges.emplace(e.id,e).second,"Duplicate stored edge");
    }
    require(sha256(g.json().dump())==record.at("graph_hash").get<std::string>(),"Neo4j graph digest mismatch");g.verify();for(const auto& ref:g.meta.value("artifacts",Json::array()))(void)evidence(ref);return g;
}
Json Store::read(const Json& request,const std::string& run) const {
    Neo4jTransaction tx(impl_->db,true);auto record=impl_->run(run,tx.id);auto meta=checked_payload(record);
    Neo4jGraph access(impl_->db,impl_->space,record.at("id"),meta,tx.id);auto result=query(access,request);
    std::set<std::string> artifacts;
    std::function<void(const Json&)> verify_refs=[&](const Json& value){
        if(value.is_object()&&value.contains("sha256")&&value.contains("bytes")&&value.at("sha256").is_string()&&value.at("bytes").is_number_unsigned()){
            auto hash=value.at("sha256").get<std::string>();if(artifacts.insert(hash).second)(void)evidence(value);
        }
        if(value.is_structured())for(const auto& child:value)verify_refs(child);
    };
    verify_refs(result);
    // Verify referenced source bytes, including source text underlying returned passages.
    std::set<std::string> seen;for(const auto& row:result.at("nodes")){
        Node n=row;std::vector<Node> check{n};if(n.kind=="Passage")check.push_back(access.node(n.props.at("report")));
        for(const auto& p:n.support)check.push_back(access.node(access.node(p).props.at("report")));
        for(const auto& item:check)if(seen.insert(item.id).second&&item.kind=="Report"&&item.props.contains("body"))require(evidence(item.props.at("body"))==item.props.at("body_text").get<std::string>(),"Query source body differs from exact evidence");
    }
    tx.commit();return result;
}
Json Store::runs() const {return impl_->db.execute("MATCH (r:AxRun {store:$store}) RETURN r.id AS id ORDER BY r.sequence",impl_->params());}
void Store::document(const std::string& kind,const std::string& id,const Json& value){
    require(!impl_->readonly,"Read-only store");require(std::set<std::string>{"collection","corpus","owner_change"}.contains(kind),"Unsupported document kind");for(const auto& ref:value.value("artifacts",Json::array()))(void)evidence(ref);
    auto p=impl_->params();p["key"]=impl_->space+"/"+kind+"/"+id;auto rows=impl_->db.execute("MATCH (d:AxDocument {key:$key}) RETURN d.payload AS payload,d.digest AS digest",p);
    if(!rows.empty()){require(checked_payload(rows[0])==value,"Immutable document differs");return;}
    p["kind"]=kind;p["id"]=id;p["payload"]=value.dump();p["digest"]=sha256(value.dump());p["created"]=now();
    impl_->db.execute("MATCH (s:AxStore {id:$store}) SET s.sequence=s.sequence+1 CREATE (d:AxDocument {key:$key,store:$store,kind:$kind,id:$id,sequence:s.sequence,payload:$payload,digest:$digest,created:$created}) RETURN d.id AS id",p,false);
}
Json Store::documents(const std::string& kind) const {
    auto p=impl_->params();p["kind"]=kind;Json out=Json::array();
    for(const auto& row:impl_->db.execute("MATCH (d:AxDocument {store:$store,kind:$kind}) RETURN d.id AS id,d.payload AS payload,d.digest AS digest ORDER BY d.sequence",p))out.push_back({{"id",row.at("id")},{"value",checked_payload(row)}});return out;
}
Json Store::latest_document(const std::string& kind) const {auto rows=documents(kind);return rows.empty()?Json(nullptr):rows.back();}
void Store::verify() const {for(const auto& kind:{"collection","corpus","owner_change"})for(const auto& doc:documents(kind))for(const auto& ref:doc.at("value").value("artifacts",Json::array()))(void)evidence(ref);for(const auto& run:runs())(void)load(run.at("id"));}
Json Store::status() const {
    auto all=runs(),corpus=latest_document("corpus");Json run=all.empty()?Json(nullptr):all.back().at("id");
    Json out={{"store",root.string()},{"backend","neo4j"},{"database",impl_->db.public_configuration()},{"namespace",impl_->space},{"published_runs",all.size()},{"latest_run",run},{"corpus_id",corpus.is_null()?Json(nullptr):corpus.at("id")},{"retained_reports",corpus.is_null()?0:corpus.at("value").at("reports").size()},{"collection_receipts",documents("collection").size()},{"state",run.is_null()?"NOT_PREPARED":"PREPARED"}};
    if(!corpus.is_null())out["collection_coverage"]=corpus.at("value").value("source_runs",Json::array());
    if(!run.is_null()){auto meta=checked_payload(impl_->run(run));out["prepared_corpus_id"]=meta.value("corpus_id",Json(nullptr));out["reports_processed"]=meta.value("reports_processed",Json(nullptr));out["reports_failed"]=meta.value("reports_failed",Json(nullptr));if(!corpus.is_null()&&out["prepared_corpus_id"]!=corpus.at("id"))out["state"]="CORPUS_CHANGED_NEEDS_PREPARATION";}
    return out;
}
void Store::backup(const fs::path& destination) const {
    verify();require(!fs::exists(destination),"Restore destination already exists");auto target=fs::weakly_canonical(destination);
    require(target.string().starts_with(fs::weakly_canonical(component/"artifacts").string()+"/"),"Backup must remain in component");require(!target.string().starts_with(root.string()+"/"),"Backup destination must be outside the source archive");fs::create_directories(destination);
    Json bundle={{"schema_version","axthena.neo4j-backup.v1"},{"runs",Json::array()},{"documents",Json::array()},{"archive",copy_archive(root,destination)}};
    for(const auto& row:runs()){auto graph=load(row.at("id")).json();bundle["runs"].push_back({{"graph",graph},{"sha256",sha256(graph.dump())}});}
    for(const auto* kind:{"collection","corpus","owner_change"})for(const auto& row:documents(kind))bundle["documents"].push_back({{"kind",kind},{"id",row.at("id")},{"value",row.at("value")},{"sha256",sha256(row.at("value").dump())}});
    write_json(destination/"database-backup.json",bundle);{Store restored(destination);restored.restore(destination);restored.verify();for(const auto* kind:{"collection","corpus","owner_change"})require(restored.documents(kind)==documents(kind),"Restored document history differs");require(restored.runs()==runs(),"Restored run order differs");}
    write_json(destination/"backup.json",{{"source",root.string()},{"created_at",now()},{"bundle_sha256",sha256(read_file(destination/"database-backup.json"))},{"status",status()}});
}
void Store::restore(const fs::path& backup){
    require(!impl_->readonly,"Read-only store");auto bundle=read_json(backup/"database-backup.json");require(bundle.at("schema_version")=="axthena.neo4j-backup.v1","Unsupported backup");
    verify_archive(backup,bundle.at("archive"));
    for(const auto& doc:bundle.at("documents"))require(sha256(doc.at("value").dump())==doc.at("sha256").get<std::string>(),"Backup document digest mismatch");
    for(const auto& row:bundle.at("runs"))require(sha256(row.at("graph").dump())==row.at("sha256").get<std::string>(),"Backup graph digest mismatch");
    Json intent={{"bundle_sha256",sha256(bundle.dump())}};auto marker=root/"restore-intent.json";
    if(fs::exists(marker))require(read_json(marker)==intent,"Restore bundle changed; use a fresh namespace");
    else{require(runs().empty()&&documents("collection").empty()&&documents("corpus").empty()&&documents("owner_change").empty(),"Restore requires a fresh namespace");write_json(marker,intent);}
    if(fs::weakly_canonical(backup)!=root)copy_archive(backup,root);
    for(const auto& doc:bundle.at("documents")){require(sha256(doc.at("value").dump())==doc.at("sha256").get<std::string>(),"Backup document digest mismatch");document(doc.at("kind"),doc.at("id"),doc.at("value"));}
    for(const auto& row:bundle.at("runs")){require(sha256(row.at("graph").dump())==row.at("sha256").get<std::string>(),"Backup graph digest mismatch");auto graph=Graph::parse(row.at("graph"));publish(graph.meta.at("run_id"),graph);}
}
}
