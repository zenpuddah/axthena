#pragma once
#include "axthena/core.hpp"

namespace axthena {
Json neo4j_properties(const Node& node,const std::string& store,const std::string& run);
Json neo4j_properties(const Edge& edge,const std::string& store,const std::string& run);
class Neo4j {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Json connection_;
    std::string password_;
public:
    explicit Neo4j(const fs::path& configuration = {});
    ~Neo4j();
    Json request(const std::string& method, const std::string& suffix, const Json& body) const;
    Json execute(const std::string& statement, const Json& parameters=Json::object(),
                 bool readonly=true, const std::string& transaction="") const;
    std::string begin(bool readonly=false) const;
    void commit(const std::string& transaction) const;
    void rollback(const std::string& transaction) const noexcept;
    Json public_configuration() const;
};
class Neo4jTransaction {
    const Neo4j& db_;
public:
    std::string id;
    explicit Neo4jTransaction(const Neo4j& db, bool readonly=false):db_(db),id(db.begin(readonly)){}
    ~Neo4jTransaction(){if(!id.empty())db_.rollback(id);}
    void commit(){db_.commit(id);id.clear();}
};
class Neo4jGraph final : public GraphAccess {
    const Neo4j& db_;
    std::string store_, run_, transaction_;
    Json meta_;
    mutable std::map<std::string,Node> nodes_;
    mutable std::map<std::string,Edge> edges_;
    mutable std::map<std::string,std::vector<Edge>> adjacency_;
    Json parameters() const;
public:
    Neo4jGraph(const Neo4j& db, std::string store, std::string run, Json meta, std::string transaction="");
    const Json& metadata() const override {return meta_;}
    bool contains(const std::string& id) const override;
    Node node(const std::string& id) const override;
    Edge edge(const std::string& id) const override;
    std::vector<Node> select(const std::vector<std::string>& kinds,const Json& reports=nullptr) const override;
    std::vector<Edge> adjacent(const std::string& id,const std::string& direction="both",const std::vector<std::string>& kinds={}) const override;
};
}
