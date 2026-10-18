#include "axthena/core.hpp"
#include <llama.h>
#include <cmath>

namespace axthena {
struct Encoder::Impl {
    llama_model* model=nullptr;llama_context* context=nullptr;Json config;
    std::map<std::pair<bool,std::string>,std::vector<float>> cached;
    ~Impl(){if(context)llama_free(context);if(model)llama_model_free(model);llama_backend_free();}
    std::vector<llama_token> tokens(const std::string& text,bool query){
        auto input=config.at(query?"query_prefix":"document_prefix").get<std::string>()+text;
        auto* vocab=llama_model_get_vocab(model);int count=llama_tokenize(vocab,input.data(),int(input.size()),nullptr,0,true,true);
        require(count<0,"Encoder tokenizer count failed");count=-count;
        require(count<=config.at("max_tokens").get<int>(),"Encoder input too long; no silent truncation");
        std::vector<llama_token> tokens(static_cast<size_t>(count));
        int used=llama_tokenize(vocab,input.data(),int(input.size()),tokens.data(),count,true,true);
        require(used==count,"Encoder tokenizer inconsistent");return tokens;
    }
};
Encoder::Encoder(const Json& config):impl_(std::make_unique<Impl>()){
    impl_->config=config;auto path=component/config.at("file").get<std::string>();
    require(sha256(read_file(path))==config.at("sha256").get<std::string>(),"Encoder artifact digest mismatch");
    llama_log_set([](ggml_log_level,const char*,void*){},nullptr);llama_backend_init();
    auto model_params=llama_model_default_params();model_params.n_gpu_layers=0;
    impl_->model=llama_model_load_from_file(path.c_str(),model_params);require(impl_->model,"Cannot load native encoder");
    auto params=llama_context_default_params();params.n_ctx=config.at("max_tokens");params.n_batch=params.n_ctx;params.n_ubatch=params.n_ctx;
    params.n_seq_max=1;params.embeddings=true;params.pooling_type=LLAMA_POOLING_TYPE_MEAN;params.attention_type=LLAMA_ATTENTION_TYPE_NON_CAUSAL;
    params.n_threads=config.at("threads");params.n_threads_batch=params.n_threads;params.offload_kqv=false;params.op_offload=false;
    impl_->context=llama_init_from_model(impl_->model,params);require(impl_->context,"Cannot initialize encoder context");
    require(llama_model_n_embd_out(impl_->model)==config.at("dimensions").get<int>(),"Unexpected embedding dimensions");
}
Encoder::~Encoder()=default;
Json Encoder::tokenize(const std::string& text,bool query){return impl_->tokens(text,query);}
std::vector<float> Encoder::encode(const std::string& text,bool query){
    auto key=std::pair{query,text};if(impl_->cached.contains(key))return impl_->cached.at(key);
    auto tokens=impl_->tokens(text,query);auto batch=llama_batch_init(int(tokens.size()),0,1);batch.n_tokens=int(tokens.size());
    for(int i=0;i<batch.n_tokens;i++){batch.token[i]=tokens[static_cast<size_t>(i)];batch.pos[i]=i;batch.n_seq_id[i]=1;batch.seq_id[i][0]=0;batch.logits[i]=1;}
    if(auto memory=llama_get_memory(impl_->context))llama_memory_clear(memory,true);
    int result=llama_model_has_encoder(impl_->model)?llama_encode(impl_->context,batch):llama_decode(impl_->context,batch);
    llama_batch_free(batch);require(result==0,"Native embedding inference failed");
    const float* ptr=llama_get_embeddings_seq(impl_->context,0);require(ptr,"No pooled sequence embedding");
    std::vector<float> out(ptr,ptr+impl_->config.at("dimensions").get<int>());double sum=0;for(float value:out){require(std::isfinite(value),"Non-finite vector");sum+=double(value)*value;}
    require(sum>0,"Zero embedding");for(float& value:out)value=static_cast<float>(value/std::sqrt(sum));impl_->cached.emplace(key,out);return out;
}
}
