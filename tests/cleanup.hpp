#pragma once
#include "axthena/neo4j.hpp"

// Successful fixtures remove only namespaces whose local markers were created
// under this invocation's scratch directory. Failed fixtures remain inspectable.
inline void cleanup_fixture(const axthena::fs::path& scratch){
    using namespace axthena;
    require(fs::weakly_canonical(scratch).string().starts_with(fs::weakly_canonical(component/"artifacts/test-work").string()+"/"),"Test cleanup outside scratch root");
    Json spaces=Json::array();
    for(const auto& file:fs::recursive_directory_iterator(scratch))if(file.path().filename()=="neo4j-store.json"&&file.is_regular_file())spaces.push_back(read_json(file.path()).at("namespace"));
    if(!spaces.empty()){
        Neo4j db;db.execute("MATCH (n) WHERE ((n:AxRecord OR n:AxRun OR n:AxDocument) AND n.store IN $spaces) OR (n:AxStore AND n.id IN $spaces) DETACH DELETE n RETURN count(n) AS removed",{{"spaces",spaces}},false);
    }
    fs::remove_all(scratch);
}
