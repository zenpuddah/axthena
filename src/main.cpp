#include "axthena/core.hpp"
#include "axthena/investigation.hpp"
#include <iostream>
#include <unistd.h>
#include <mach-o/dyld.h>

int main(int argc,char** argv){
    using namespace axthena;
    try{
        if(argc<2||std::string(argv[1])=="--help"||std::string(argv[1])=="help"){
            std::cout<<"axthena init | status | collect --plan FILE --batch NAME [--retry-failed]\n"
                "        ingest --config FILE [--dry-run | --watch | --resume JOB_ID] [--store DIR]\n"
                "        prepare [--replay] [--limit N] | query --request FILE | export --out DIR\n"
                "        verify | backup --out DIR | restore --source DIR | migrate --source SQLITE_DIR\n"
                "        revise --change FILE | import [--input FILE]\n"
                "        resolve-domains --config FILE [--run ID] [--replay]\n"
                "        serve [--port 17780] [--assets DIR] | investigate --request FILE [--run ID] | mcp [--run ID]\n"
                "        smoke --article N | extract | embed --text TEXT | model-check\n"
                "All commands: --store DIR (default artifacts/neo4j-v1/store).\n"
                "Preparation: --config FILE, --attempt NAME; limit 0 means selected corpus.\n"
                "Reads/export: --run ID selects retained history. import without --input imports the original pilot explicitly.\n";return 0;}
        std::string command=argv[1];
        if(command=="ingest"){
            uint32_t size=0;_NSGetExecutablePath(nullptr,&size);std::vector<char> executable(size);
            require(_NSGetExecutablePath(executable.data(),&size)==0,"Cannot locate native ingestion executable");
            std::vector<std::string> args={"python3",(resource_root()/"tools/ingest.py").string(),"--native",fs::canonical(executable.data()).string()};
            for(int i=2;i<argc;++i)args.emplace_back(argv[i]);
            std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());raw.push_back(nullptr);
            execvp(raw[0],raw.data());throw std::runtime_error("Cannot start Python 3 ingestion runner");
        }
        std::map<std::string,std::string> options;bool replay=false,retry=false;
        for(int i=2;i<argc;i++){std::string key=argv[i];if(key=="--replay"){replay=true;continue;}if(key=="--retry-failed"){retry=true;continue;}require(key.starts_with("--")&&i+1<argc,"Invalid command arguments");options[key]=argv[++i];}
        const std::map<std::string,std::set<std::string>> allowed={
            {"init",{}},{"status",{}},{"verify",{}},{"import",{"--input"}},{"migrate",{"--source"}},{"restore",{"--source"}},
            {"collect",{"--plan","--batch"}},{"prepare",{"--input","--config","--attempt","--limit"}},
            {"import-captures",{"--manifest"}},
            {"smoke",{"--input","--config","--attempt","--article"}},{"extract",{"--input","--config","--attempt"}},
            {"model-check",{"--config","--attempt"}},{"embed",{"--config","--text","--role"}},
            {"query",{"--request","--run"}},{"export",{"--out","--run"}},{"backup",{"--out"}},{"revise",{"--change","--run"}},
            {"resolve-domains",{"--config","--run"}},{"serve",{"--assets","--port"}},{"mcp",{"--run"}},{"investigate",{"--request","--run"}}};
        require(allowed.contains(command),"Unknown command: "+command);
        for(const auto& [key,value]:options){(void)value;require(key=="--store"||allowed.at(command).contains(key),"Unsupported option for "+command+": "+key);}
        require(!retry||command=="collect","--retry-failed is a collection option");
        require(!replay||std::set<std::string>{"prepare","smoke","extract","model-check","resolve-domains"}.contains(command),"--replay is a model-work option");
        auto get=[&](const std::string& key,const std::string& fallback){return options.contains(key)?options.at(key):fallback;};
        Json config=Json::object();
        auto resources=resource_root();
        auto store_path=get("--store",(component/"artifacts/neo4j-v1/store").string());
        if(command=="serve")return serve_workspace(store_path,get("--assets",(resources/"workspace/dist").string()),std::stoul(get("--port","17780")));
        if(command=="mcp")return serve_mcp(store_path,get("--run",""));
        if(std::set<std::string>{"embed","smoke","extract","prepare","model-check","resolve-domains"}.contains(command))config=read_json(get("--config",(resources/"config/application-v1.json").string()));
        if(command=="embed"){
            Encoder encoder(config.at("encoder"));auto text=get("--text","NVIDIA");bool is_query=get("--role","query")=="query";
            std::cout<<Json({{"tokens",encoder.tokenize(text,is_query)},{"embedding",encoder.encode(text,is_query)},{"encoder",config.at("encoder")}}).dump()<<'\n';return 0;
        }
        bool readonly=command=="query"||command=="export"||command=="verify"||command=="backup"||command=="status"||command=="investigate";
        Store store(get("--store",(component/"artifacts/neo4j-v1/store").string()),readonly);
        auto retain_import=[&](const Json& input){auto path=store.root/"imports"/(sha256(input.dump())+".json");write_json(path,input);return path;};
        if(command=="init"){std::cout<<store.status().dump(2)<<'\n';}
        else if(command=="status"){std::cout<<store.status().dump(2)<<'\n';}
        else if(command=="import"){auto input=options.contains("--input")?read_json(options.at("--input")):import_pilot(store);retain_import(input);std::cout<<apply_corpus(store,input).at("corpus_id")<<'\n';}
        else if(command=="smoke"){
            auto input=options.contains("--input")?read_json(options.at("--input")):corpus_input(store);int number=std::stoi(get("--article","1"));require(number>=1&&size_t(number)<=input.at("reports").size(),"Invalid article");
            auto report=input["reports"][static_cast<size_t>(number-1)];Model model(store.root/"attempts",config.at("model"),replay,get("--attempt","initial"),"opencode",600000,config.value("task_attempts",Json::object()));
            auto answer=model.ask("extraction",read_file(resources/"prompts/extract-v1.md"),report);auto checked=validate_extraction(answer.value,report);
            auto validation=identity("validation",Json::array({answer.provenance.at("task_id"),answer.provenance.at("response_sha256"),sha256(read_file(resources/"src/pipeline.cpp"))}));
            write_json(store.root/"validations"/(validation.substr(11)+".json"),{{"provenance",answer.provenance},{"validated",checked}});
            std::cout<<checked.dump(2)<<'\n';
        }
        else if(command=="extract"){
            auto input=options.contains("--input")?read_json(options.at("--input")):corpus_input(store);retain_import(input);Model model(store.root/"attempts",config.at("model"),replay,get("--attempt","initial"),"opencode",600000,config.value("task_attempts",Json::object()));
            for(const auto& report:input.at("reports")){
                try{std::cerr<<"Extracting #"<<report.at("number")<<std::endl;auto answer=model.ask("extraction",read_file(resources/"prompts/extract-v1.md"),report);
                    auto checked=validate_extraction(answer.value,report);auto validation=identity("validation",Json::array({answer.provenance.at("task_id"),answer.provenance.at("response_sha256"),sha256(read_file(resources/"src/pipeline.cpp"))}));write_json(store.root/"validations"/(validation.substr(11)+".json"),{{"provenance",answer.provenance},{"validated",checked}});
                    std::cerr<<checked.at("status")<<": "<<checked.at("ideas").size()<<" ideas, "<<checked.at("validation_failures").size()<<" rejected components\n";
                }catch(const std::exception& e){std::cerr<<"Retained failure: "<<e.what()<<std::endl;}
            }
        }
        else if(command=="model-check"){
            auto fixture=read_json(resources/"tests/fixtures/meaning-cases.json");
            Model model(store.root/"attempts",config.at("model"),replay,get("--attempt","initial"));Json results=Json::array();
            auto meaning=[&](const Json& row){
                return Json{{"summary",row.at("summary")},{"kind","OPINION"},{"organisation_ids",Json::array({"synthetic-nvidia"})},
                    {"subjects",Json::array({{{"organisation","synthetic-nvidia"},{"domain","AI chips"}}})},
                    {"judgments",Json::array({{{"stock","NVDA.US"},{"direction","UP"},{"conditions",row.value("conditions","provided the chip ships on time")},
                    {"window",{{"start",row.value("start","2026-10-01")},{"end",row.value("end","2026-10-15")}}}}})}};
            };
            auto canonical=meaning({{"summary",fixture.at("canonical")}});
            for(const auto& item:fixture.at("cases")){
                auto incoming=meaning(item);auto variants=Json::array({canonical});if(item.contains("extra_variant"))variants.push_back(meaning({{"summary",item.at("extra_variant")}}));
                Json candidates=Json::array({{{"id","synthetic-idea"},{"canonical",canonical},{"variants",variants}}});
                auto answer=model.ask("meaning_membership",read_file(resources/"prompts/meaning-v1.md"),{{"incoming",incoming},{"candidates",candidates}});
                bool reused=!select_meaning(answer.value,candidates,incoming).empty();results.push_back({{"case",item},{"answer",answer.value},{"provenance",answer.provenance},{"passed",reused==item.at("expect_reuse").get<bool>()}});
                std::cerr<<item.at("name")<<": "<<(results.back().at("passed").get<bool>()?"PASS":"FAIL")<<std::endl;
            }
            write_json(store.root/"meaning-check.json",{{"fixture_sha256",sha256(fixture.dump())},{"results",results}});
            for(const auto& r:results)require(r.at("passed").get<bool>(),"Live semantic comparison contrast failed; first output preserved");std::cout<<results.size()<<" model contrasts passed\n";
        }
        else if(command=="prepare"){
            config["attempt_generation"]=get("--attempt","initial");
            auto input=options.contains("--input")?read_json(options.at("--input")):corpus_input(store);retain_import(input);auto graph=prepare(store,input,config,replay,std::stoi(get("--limit","0")));
            std::cout<<graph.meta.dump(2)<<"\nNodes: "<<graph.nodes.size()<<" Edges: "<<graph.edges.size()<<'\n';
        }
        else if(command=="resolve-domains"){
            auto previous=store.load(get("--run",""));if(previous.meta.contains("domain_resolution")){std::cout<<previous.meta.at("run_id")<<'\n';return 0;}auto graph=resolve_domains(store,previous,config,replay);
            graph.meta["parent_run"]=previous.meta.at("run_id");graph.meta["run_id"]=identity("run",Json::array({previous.meta.at("run_id"),graph.json()}));
            store.publish(graph.meta.at("run_id"),graph);std::cout<<graph.meta.at("domain_resolution").dump(2)<<'\n'<<graph.meta.at("run_id")<<'\n';
        }
        else if(command=="verify"){store.verify();std::cout<<"Neo4j records, relationships, exact evidence and passage references verified\n";}
        else if(command=="query"){auto request=read_json(get("--request",(resources/"examples/query-annual.json").string()));std::cout<<store.read(request,get("--run","")).dump(2)<<'\n';}
        else if(command=="investigate"){require(options.contains("--request"),"Investigation requires --request FILE");std::cout<<investigate(store,read_json(options.at("--request")),get("--run","")).dump(2)<<'\n';}
        else if(command=="migrate"){require(options.contains("--source"),"Migration requires --source legacy store");store.migrate_legacy(options.at("--source"));std::cout<<store.status().dump(2)<<'\n';}
        else if(command=="restore"){require(options.contains("--source"),"Restore requires --source backup");store.restore(options.at("--source"));store.verify();std::cout<<store.status().dump(2)<<'\n';}
        else if(command=="export"){auto graph=store.load(get("--run",""));require(options.contains("--out"),"Export requires a fresh --out directory");auto output=options.at("--out");export_graph(graph,output);std::cout<<fs::absolute(output)/"index.html"<<'\n';}
        else if(command=="backup"){require(options.contains("--out"),"Backup requires a fresh --out directory");store.backup(options.at("--out"));std::cout<<"Backup restored and verified\n";}
        else if(command=="revise"){
            require(options.contains("--change"),"Revision requires a supplied --change file");auto change=read_json(options.at("--change"));auto previous=store.load(get("--run",""));auto graph=revise(previous,change);
            auto artifact=store.retain(change.dump());graph.meta["artifacts"].push_back(artifact);
            Json saved={{"nodes",Json::array()},{"edges",Json::array()},{"artifacts",Json::array({artifact})}};
            std::set<std::string> controls;
            for(const auto& [id,n]:graph.nodes)if(n.kind=="OwnerReview"||n.kind=="PersonWeightVersion"||n.kind=="ViewVersion"){saved["nodes"].push_back(n);controls.insert(id);}
            for(const auto& [id,e]:graph.edges){(void)id;if(controls.contains(e.from)||controls.contains(e.to))saved["edges"].push_back(e);}
            saved["selected_parent_run"]=previous.meta.at("run_id");
            store.document("owner_change",graph.meta.at("run_id"),saved);store.publish(graph.meta.at("run_id"),graph);std::cout<<graph.meta.at("run_id")<<'\n';
        }
        else if(command=="collect"){
            require(options.contains("--plan"),"Collection requires explicit plan");auto plan=read_json(options.at("--plan"));Json input;
            if(plan.value("schema_version","")=="axthena.public-plan.v1")input=collect_public(store,plan,get("--batch","initial"),retry);
            else{const char* token=getenv("EODHD_API_TOKEN");input=decode_news(store,collect_news(store,plan,token?token:""));}
            retain_import(input);auto corpus=apply_corpus(store,input);
            std::cout<<Json({{"corpus_id",corpus.at("corpus_id")},{"retained_reports",corpus.at("reports").size()},{"collection",input.at("source_runs")}}).dump(2)<<'\n';
        }
        else if(command=="import-captures"){
            require(options.contains("--manifest"),"Capture import requires --manifest");
            auto input=import_captures(store,options.at("--manifest"));retain_import(input);auto corpus=apply_corpus(store,input);
            std::cout<<Json({{"corpus_id",corpus.at("corpus_id")},{"retained_reports",corpus.at("reports").size()},
                {"observations",corpus.at("observations").size()},{"collection",input.at("source_runs")},{"issues",input.at("issues")}}).dump(2)<<'\n';
        }
        else throw std::runtime_error("Unknown command");
    }catch(const std::exception& e){std::cerr<<"Axthena: "<<e.what()<<'\n';return 2;}
}
