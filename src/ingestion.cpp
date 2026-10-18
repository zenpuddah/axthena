#include "axthena/core.hpp"

namespace axthena {
Json import_captures(Store& store,const fs::path& manifest_path){
    const auto manifest=read_json(manifest_path);
    require(manifest.at("schema_version")=="axthena.captures.v1","Unsupported capture manifest");
    require(manifest.at("pages").is_array()&&manifest.at("attempts").is_array(),"Invalid capture lists");
    const auto root=fs::canonical(manifest.at("blob_root").get<std::string>());
    std::map<std::string,std::string> blobs;
    auto validate_blob=[&](const Json& ref){
        auto hash=ref.at("sha256").get<std::string>();
        require(hash.size()==64&&hash.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid capture SHA-256");
        auto path=fs::canonical(root/hash);require(path.parent_path()==root&&!fs::is_symlink(root/hash),"Capture escapes blob directory");
        require(fs::file_size(path)<=32*1024*1024,"Capture exceeds body bound");
        auto bytes=read_file(path);require(ref.at("bytes")==bytes.size()&&sha256(bytes)==hash,"Capture body hash/size differs");
        blobs[hash]=bytes;
    };
    std::map<std::string,Json> pages;
    for(const auto& page:manifest.at("pages")){
        auto url=page.at("url").get<std::string>();require(pages.emplace(url,page).second,"Duplicate captured page URL");
        validate_blob(page.at("body"));
        require(page.at("headers").is_object()&&page.at("body_collected_at").is_string()&&page.at("observed_at").is_string(),"Missing capture timing/headers");
        const auto& receipt=page.at("validation");validate_blob(receipt.at("response"));validate_blob(receipt.at("request"));
        const auto& origin=page.at("origin");validate_blob(origin.at("response"));validate_blob(origin.at("request"));
        require(origin.at("http_status")==200&&origin.at("url")==url&&origin.at("error").is_null()&&origin.at("response")==page.at("body")&&origin.at("observed_at")==page.at("body_collected_at"),"Missing original body acquisition lineage");
        for(const auto* captured:{&origin,&receipt}){
            auto request=Json::parse(blobs.at(captured->at("request").at("sha256")));
            require(request.at("url")==url,"Capture request URL mismatch");
            if(captured->at("http_status")==304)require(request.at("headers").contains("If-None-Match")||request.at("headers").contains("If-Modified-Since"),"304 requires conditional request");
        }
        require(receipt.at("url")==url&&(receipt.at("http_status")==200||receipt.at("http_status")==304)&&receipt.at("error").is_null(),"Invalid cached response validation");
        if(receipt.at("http_status")==200)require(receipt.at("response")==page.at("body"),"Downloaded body differs from raw response");
    }
    for(const auto& attempt:manifest.at("attempts")){validate_blob(attempt.at("response"));validate_blob(attempt.at("request"));}
    // Validate all bytes before publishing any collection/corpus document.
    Json artifacts=Json::array({store.retain(manifest.dump())});
    for(const auto& [hash,bytes]:blobs){(void)hash;artifacts.push_back(store.retain(bytes));}
    auto imported=collect_public(store,manifest.at("plan"),manifest.at("batch"),false,[&](const std::string& url){
        require(pages.contains(url),"Native collector requested an uncaptured page: "+url);
        const auto& page=pages.at(url);
        return HttpResponse{200,true,blobs.at(page.at("body").at("sha256")),page.at("headers"),page};
    },true);
    for(const auto& ref:artifacts)imported["artifacts"].push_back(ref);
    imported["capture_manifest"]=artifacts[0];
    return imported;
}
}
