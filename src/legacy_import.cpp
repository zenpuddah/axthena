#include "axthena/core.hpp"
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace axthena {
namespace {
struct Legacy {
    sqlite3* db=nullptr;
    int lock=-1;
    ~Legacy(){sqlite3_close(db);if(lock>=0)::close(lock);}
    Json rows(const char* sql,const std::string& parameter="") const {
        sqlite3_stmt* raw=nullptr;require(sqlite3_prepare_v2(db,sql,-1,&raw,nullptr)==SQLITE_OK,"Legacy query preparation failed");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(raw,sqlite3_finalize);Json result=Json::array();int code;
        if(!parameter.empty())require(sqlite3_bind_text(raw,1,parameter.c_str(),-1,SQLITE_TRANSIENT)==SQLITE_OK,"Legacy parameter binding failed");
        while((code=sqlite3_step(raw))==SQLITE_ROW){Json row=Json::array();for(int i=0;i<sqlite3_column_count(raw);++i){const auto* text=sqlite3_column_text(raw,i);row.push_back(text?reinterpret_cast<const char*>(text):"");}result.push_back(row);}require(code==SQLITE_DONE,"Legacy read failed");return result;
    }
};
}
void Store::migrate_legacy(const fs::path& supplied){
    auto source=fs::weakly_canonical(supplied);require(source!=root&&source.string().starts_with(fs::weakly_canonical(component/"artifacts").string()+"/"),"Legacy source must be a separate component artifact store");
    require(!root.string().starts_with(source.string()+"/")&&!source.string().starts_with(root.string()+"/"),"Migration source and destination must not overlap");
    Legacy legacy;legacy.lock=::open((source/".operation.lock").c_str(),O_RDONLY|O_CLOEXEC);require(legacy.lock>=0&&::flock(legacy.lock,LOCK_SH|LOCK_NB)==0,"Legacy operation is still running");
    require(sqlite3_open_v2((source/"store.sqlite").c_str(),&legacy.db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"Cannot open legacy SQLite read-only");
    require(legacy.rows("PRAGMA integrity_check")[0][0]=="ok"&&legacy.rows("PRAGMA foreign_key_check").empty(),"Legacy SQLite integrity failure");
    require(legacy.rows("SELECT version FROM format")==Json::array({Json::array({"1"})}),"Unsupported legacy store version");
    Json intent={{"source",source.string()},{"source_sha256",sha256(read_file(source/"store.sqlite"))}};
    auto marker=root/"migration-intent.json";
    if(fs::exists(marker))require(read_json(marker)==intent,"Migration source changed; retain the prior migration and use a fresh destination");
    else{
        if(fs::exists(root/"migration.json")){auto prior=read_json(root/"migration.json");require(prior.at("source")==intent.at("source")&&prior.at("source_sha256")==intent.at("source_sha256"),"Existing migration has another source");}
        else require(runs().empty()&&documents("collection").empty()&&documents("corpus").empty()&&documents("owner_change").empty(),"Migration requires a fresh namespace");
        write_json(marker,intent);
    }
    for(const auto& file:fs::recursive_directory_iterator(source))if(file.is_regular_file()){
        auto relative=fs::relative(file.path(),source);auto name=relative.generic_string();if(name==".operation.lock"||name=="backup.json"||name.starts_with("store.sqlite"))continue;write_once(root/relative,read_file(file.path()));
    }
    if(!legacy.rows("SELECT name FROM sqlite_master WHERE type='table' AND name='documents'").empty())for(const auto& row:legacy.rows("SELECT kind,id,digest,payload FROM documents ORDER BY rowid")){
        require(sha256(row[3].get<std::string>())==row[2].get<std::string>(),"Legacy document hash mismatch");document(row[0],row[1],Json::parse(row[3].get<std::string>()));
    }
    for(const auto& row:legacy.rows("SELECT id,graph_hash,graph FROM runs ORDER BY rowid")){
        require(sha256(row[2].get<std::string>())==row[1].get<std::string>(),"Legacy run hash mismatch");auto graph=Graph::parse(Json::parse(row[2].get<std::string>()));require(graph.meta.at("run_id")==row[0],"Legacy run identity mismatch");
        for(const auto* table:{"nodes","edges"}){auto sql=std::string("SELECT payload FROM ")+table+" WHERE run=? ORDER BY id";Json actual=Json::array();for(const auto& item:legacy.rows(sql.c_str(),row[0]))actual.push_back(Json::parse(item[0].get<std::string>()));require(actual==graph.json().at(table),"Legacy tables differ from published graph");}
        publish(row[0],graph);
    }
    verify();write_json(root/"migration.json",{{"schema_version","axthena.sqlite-migration.v1"},{"source",source.string()},{"source_sha256",sha256(read_file(source/"store.sqlite"))},{"backend","neo4j"}});
}
}
