#include "axthena/core.hpp"
#include <cctype>

namespace axthena {
namespace {
std::string utf8(unsigned n){
    require(n>0&&n<=0x10ffff&&!(n>=0xd800&&n<=0xdfff),"Invalid HTML character reference");
    std::string out;
    if(n<128)out+=char(n);
    else if(n<2048){out+=char(0xc0|(n>>6));out+=char(0x80|(n&63));}
    else if(n<65536){out+=char(0xe0|(n>>12));out+=char(0x80|((n>>6)&63));out+=char(0x80|(n&63));}
    else{out+=char(0xf0|(n>>18));out+=char(0x80|((n>>12)&63));out+=char(0x80|((n>>6)&63));out+=char(0x80|(n&63));}
    return out;
}
std::string entity(const std::string& key){
    static const std::map<std::string,std::string> named={
        {"amp","&"},{"lt","<"},{"gt",">"},{"quot","\""},{"apos","'"},{"nbsp"," "},
        {"ndash","–"},{"mdash","—"},{"lsquo","‘"},{"rsquo","’"},{"ldquo","“"},{"rdquo","”"},
        {"hellip","…"},{"copy","©"},{"reg","®"},{"trade","™"},{"bull","•"},{"times","×"},
        {"euro","€"},{"pound","£"},{"yen","¥"},{"cent","¢"},{"deg","°"},{"plusmn","±"},
        {"laquo","«"},{"raquo","»"},{"eacute","é"},{"Eacute","É"},{"aacute","á"},
        {"ouml","ö"},{"uuml","ü"},{"auml","ä"},{"ntilde","ñ"},{"szlig","ß"}};
    if(named.contains(key))return named.at(key);
    if(key.starts_with("#")){
        bool hex=key.size()>2&&(key[1]=='x'||key[1]=='X');auto digits=key.substr(hex?2:1);
        require(!digits.empty()&&digits.find_first_not_of(hex?"0123456789abcdefABCDEF":"0123456789")==std::string::npos,"Invalid HTML numeric entity");
        auto n=std::stoul(digits,nullptr,hex?16:10);require(n<=0x10ffff,"Oversized HTML entity");
        return n==160?" ":utf8(unsigned(n));
    }
    // Preserve unknown named references literally instead of inventing text.
    return "&"+key+";";
}
}
std::string publisher_text(const std::string& html){
    static const std::set<std::string> blocks={"p","div","section","article","h1","h2","h3","h4","h5","h6","ul","ol","li","blockquote","figure","figcaption","table","tr","br","hr"};
    static const std::set<std::string> omitted={"script","style","nav","aside","form","button","noscript","iframe","svg","video","audio"};
    std::string text,skip;size_t i=0;
    auto boundary=[&](){if(!text.empty()&&text.back()!='\n')text+='\n';};
    while(i<html.size()){
        if(html.compare(i,4,"<!--")==0){auto end=html.find("-->",i+4);require(end!=std::string::npos,"Unterminated HTML comment");i=end+3;continue;}
        if(html[i]=='<'){
            size_t end=i+1;char quote=0;
            for(;end<html.size();++end){char c=html[end];if(quote){if(c==quote)quote=0;}else if(c=='\''||c=='\"')quote=c;else if(c=='>')break;}
            require(end<html.size(),"Unterminated HTML tag");
            size_t pos=i+1;bool closing=pos<end&&html[pos]=='/';if(closing)++pos;
            while(pos<end&&std::isspace(static_cast<unsigned char>(html[pos])))++pos;
            std::string tag;while(pos<end&&std::isalnum(static_cast<unsigned char>(html[pos])))tag+=char(std::tolower(static_cast<unsigned char>(html[pos++])));
            if(!skip.empty()){if(closing&&tag==skip){skip.clear();boundary();}}
            else if(!closing&&omitted.contains(tag)){skip=tag;boundary();}
            else if(blocks.contains(tag))boundary();
            else if(tag=="td"||tag=="th")text+=' ';
            i=end+1;continue;
        }
        if(!skip.empty()){++i;continue;}
        if(html[i]=='&'){
            auto end=html.find(';',i+1);
            if(end!=std::string::npos&&end-i<=32){text+=entity(html.substr(i+1,end-i-1));i=end+1;continue;}
        }
        unsigned char c=html[i++];text+=(c<128&&std::isspace(c))?' ':char(c);
    }
    require(skip.empty(),"Unclosed omitted HTML element");
    std::string out;bool space=false,newline=false;
    for(unsigned char c:text){
        if(c=='\n'){newline=!out.empty();space=false;continue;}
        if(c<128&&std::isspace(c)){space=!out.empty();continue;}
        if(newline)out+='\n';else if(space)out+=' ';
        out+=char(c);space=false;newline=false;
    }
    return out;
}
}
