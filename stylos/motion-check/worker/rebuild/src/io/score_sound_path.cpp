#include "retas/score_sound.hpp"
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace retas {
namespace {
void require(bool yes,const char* why){if(!yes)throw std::invalid_argument(why);}
const char* codec(std::uint32_t page){
    switch(page){case 20127:return "ASCII";case 932:return "CP932";case 936:return "CP936";
    case 949:return "CP949";case 950:return "CP950";case 1252:return "CP1252";case 65001:return "UTF-8";}
    throw std::invalid_argument("Unsupported Score sound code page");
}
std::string convert(std::string_view input,std::uint32_t from,std::uint32_t to){
    require(input.size()<=262140,"Score sound path text exceeds conversion limit");
    if(input.empty())return {};
    if(from==20127||to==20127){
        require(std::all_of(input.begin(),input.end(),[](unsigned char c){return c<128;}),"Score sound path is not ASCII");
        return std::string(input);
    }
#ifdef _WIN32
    const auto n=int(input.size());
    const int count=::MultiByteToWideChar(from,MB_ERR_INVALID_CHARS,input.data(),n,nullptr,0);
    require(count>0,"Invalid Score sound path encoding");std::wstring wide(std::size_t(count),L'\0');
    require(::MultiByteToWideChar(from,MB_ERR_INVALID_CHARS,input.data(),n,wide.data(),count)==count,"Cannot decode Score sound path");
    BOOL substituted{};const auto flags=to==65001?WC_ERR_INVALID_CHARS:WC_NO_BEST_FIT_CHARS;auto* used=to==65001?nullptr:&substituted;
    const int bytes=::WideCharToMultiByte(to,flags,wide.data(),count,nullptr,0,nullptr,used);
    require(bytes>0&&!substituted,"Score sound path is not representable in selected ACP");std::string out(std::size_t(bytes),'\0');
    require(::WideCharToMultiByte(to,flags,wide.data(),count,out.data(),bytes,nullptr,used)==bytes&&!substituted,"Cannot encode Score sound path");return out;
#else
    const auto descriptor=iconv_open(codec(to),codec(from));
    require(descriptor!=reinterpret_cast<iconv_t>(-1),"Score sound code page unavailable");
    struct Close {iconv_t value;~Close(){iconv_close(value);}} close{descriptor};
    std::string out(input.size()*4+16,'\0');char* in=const_cast<char*>(input.data());auto left=input.size();char* dest=out.data();auto room=out.size();
    const auto result=iconv(descriptor,&in,&left,&dest,&room);
    require(result!=std::size_t(-1)&&!left&&result==0,"Invalid or lossy Score sound path conversion");out.resize(out.size()-room);return out;
#endif
}
void component(std::string_view text){
    require(!text.empty(),"Empty Score sound path component");
    for(unsigned char c:text)require(c>=32&&c!=127&&c!=':'&&c!='/'&&c!='\\',"Invalid Score sound path component");
}
std::string utf8(const std::filesystem::path& p){const auto text=p.u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
std::filesystem::path physical(const std::filesystem::path& p){return std::filesystem::weakly_canonical(std::filesystem::absolute(p));}
}
ScoreSoundPathEncoding normalize_score_sound_path_encoding(ScoreSoundPathEncoding encoding){
#ifdef _WIN32
    if(!encoding.code_page)encoding.code_page=::GetACP();
#endif
    (void)codec(encoding.code_page);return encoding;
}
Bytes score_sound_relative_reference(const std::filesystem::path& score_file,const std::filesystem::path& audio_file,ScoreSoundPathEncoding encoding){
    encoding=normalize_score_sound_path_encoding(encoding);
    const auto folder=physical(score_file.parent_path().empty()?std::filesystem::current_path():score_file.parent_path());
    // Resolve the parent, retaining a cached filename even after file removal.
    const auto target=physical(audio_file.parent_path().empty()?std::filesystem::current_path():audio_file.parent_path())/audio_file.filename();
    const auto relative=target.lexically_relative(folder);require(!relative.empty()&&!relative.is_absolute(),"Cannot form relative Score sound path");
    Bytes result;for(const auto& part:relative){const auto text=utf8(part);component(text);const auto encoded=convert(text,65001,encoding.code_page);
        require(convert(encoded,encoding.code_page,65001)==text,"Score sound path conversion does not round trip");
        if(!result.empty())result.push_back(':');result.insert(result.end(),encoded.begin(),encoded.end());
    }
    require(result.size()<=65535,"Score sound reference exceeds RCString limit");return result;
}
std::filesystem::path resolve_score_sound_reference(std::span<const std::uint8_t> reference,const std::filesystem::path& score_file,ScoreSoundPathEncoding encoding){
    encoding=normalize_score_sound_path_encoding(encoding);require(!reference.empty()&&reference.size()<=65535,"Invalid Score sound reference length");
    const std::string raw(reference.begin(),reference.end());const auto decoded=convert(raw,encoding.code_page,65001);
    require(convert(decoded,65001,encoding.code_page)==raw,"Score sound reference conversion does not round trip");
    std::filesystem::path relative;std::size_t begin{};
    for(std::size_t i=0;i<=decoded.size();++i)if(i==decoded.size()||decoded[i]==':'){
        const auto part=std::string_view(decoded).substr(begin,i-begin);component(part);relative/=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(part.data()),part.size()));begin=i+1;
    }
    require(!relative.is_absolute(),"Unsupported absolute Score sound reference");return physical(score_file.parent_path()/relative);
}
}
