#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace axthena {
using Json = nlohmann::json;
namespace fs = std::filesystem;
inline const fs::path component = std::getenv("AXTHENA_HOME")?std::getenv("AXTHENA_HOME"):AXTHENA_COMPONENT;
fs::path resource_root();
std::string read_file(const fs::path& path);
Json read_json(const fs::path& path);
void write_once(const fs::path& path, const std::string& bytes);
void write_json(const fs::path& path, const Json& value);
std::string sha256(const std::string& bytes);
std::string now();
std::string identity(const std::string& kind, const Json& key);
std::string normalized(std::string text);
void require(bool condition, const std::string& message);
std::string date_only(const Json& raw);
std::string add_days(const std::string& start, int days);
Json locate_passage(const std::string& body, const Json& quote);
Json resolve_window(const Json& horizon, const Json& publication, const Json& fallback);
double name_similarity(const std::string& a, const std::string& b);
double cosine(const std::vector<float>& a, const std::vector<float>& b);
Json candidate_union(const Json& names, const Json& vectors);

struct Node { std::string id, kind, label; Json props = Json::object(); Json support = Json::array(); };
struct Edge { std::string id, kind, from, to; Json props = Json::object(); Json support = Json::array(); };
void to_json(Json& j, const Node& n);
void from_json(const Json& j, Node& n);
void to_json(Json& j, const Edge& e);
void from_json(const Json& j, Edge& e);
struct Graph {
    std::map<std::string,Node> nodes;
    std::map<std::string,Edge> edges;
    Json meta = Json::object();
    std::string put(const std::string& kind, const Json& key, const std::string& label,
                    Json props = Json::object(), Json support = Json::array());
    void link(const std::string& kind, const std::string& from, const std::string& to,
              Json props = Json::object(), Json support = Json::array());
    Json json() const;
    static Graph parse(const Json& value);
    void verify() const;
};

class Store {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    fs::path root;
    explicit Store(fs::path path, bool readonly=false);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Json retain(const std::string& bytes);
    std::string evidence(const Json& reference) const;
    void publish(const std::string& run, const Graph& graph, const std::function<void()>& before_commit = {});
    Graph load(const std::string& run="") const;
    bool contains(const std::string& run) const;
    void verify() const;
    void backup(const fs::path& destination) const;
    void document(const std::string& kind, const std::string& id, const Json& value);
    Json documents(const std::string& kind) const;
    Json latest_document(const std::string& kind) const;
    Json status() const;
    Json runs() const;
    Json read(const Json& request, const std::string& run="") const;
    void migrate_legacy(const fs::path& source);
    void restore(const fs::path& backup);
};

// Query policy is shared; the persisted adapter performs indexed selection and
// adjacency queries instead of reconstructing an entire Graph for ordinary reads.
class GraphAccess {
public:
    virtual ~GraphAccess() = default;
    virtual const Json& metadata() const = 0;
    virtual bool contains(const std::string& id) const = 0;
    virtual Node node(const std::string& id) const = 0;
    virtual Edge edge(const std::string& id) const = 0;
    virtual std::vector<Node> select(const std::vector<std::string>& kinds,
                                   const Json& reports=nullptr) const = 0;
    virtual std::vector<Edge> adjacent(const std::string& id, const std::string& direction="both",
                                       const std::vector<std::string>& kinds={}) const = 0;
};
Json query(const GraphAccess& graph, const Json& request);

struct ModelAnswer { Json value; Json provenance; };
class Model {
    fs::path root_;
    Json model_;
    bool replay_;
    std::string generation_;
    std::string executable_;
    int timeout_ms_;
    Json task_attempts_;
    bool version_verified_=false;
public:
    unsigned calls=0, replays=0;
    Model(fs::path root, Json model, bool replay=false, std::string generation="initial", std::string executable="opencode", int timeout_ms=600000, Json task_attempts=Json::object());
    ModelAnswer ask(const std::string& kind, const std::string& instructions, const Json& input);
};

class Encoder {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Encoder(const Json& config);
    ~Encoder();
    std::vector<float> encode(const std::string& text, bool query);
    Json tokenize(const std::string& text, bool query);
};

Json import_pilot(Store& store);
Json validate_extraction(const Json& answer, const Json& report);
Json select_identity(const Json& decision, const Json& candidates);
std::string select_meaning(const Json& decision, const Json& candidates, const Json& incoming);
Json compare_quantity(const Json& quantity, const Json& unit);
Json claim_state(const Graph& graph, const std::string& claim, const std::set<std::string>& eligible_reports,
                 const std::string& cutoff);
Graph revise(const Graph& graph, const Json& change);
Graph prepare(Store& store, const Json& reports, const Json& config, bool replay, int max_reports=0,
              const std::string& model_executable="opencode");
Json query(const Graph& graph, const Json& request);
void export_graph(const Graph& graph, const fs::path& destination);
Json collect_news(Store& store, const Json& plan, const std::string& token);
Json decode_news(Store& store, const Json& collection);
struct HttpResponse { long status=0; bool complete=false; std::string body; Json headers=Json::object(); Json provenance=Json::object(); };
using PublicFetch = std::function<HttpResponse(const std::string&)>;
std::string publisher_text(const std::string& html);
Json collect_public(Store& store, const Json& plan, const std::string& batch,
                    bool retry_failed=false, const PublicFetch& fetch={}, bool retained_only=false);
Json apply_corpus(Store& store, const Json& imported);
Json corpus_input(const Store& store);
Json import_captures(Store& store,const fs::path& manifest);
}
