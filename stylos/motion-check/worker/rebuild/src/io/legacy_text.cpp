#include "retas/legacy_text.hpp"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <iconv.h>
#endif
namespace retas {
namespace {
#ifdef _WIN32
std::string convert(std::string_view input,unsigned source,unsigned target){
    if(input.empty())return {};
    int length=MultiByteToWideChar(source,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),nullptr,0);
    if(!length)throw std::runtime_error("Invalid encoded text");
    std::wstring wide(length,L'\0');
    MultiByteToWideChar(source,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),wide.data(),length);
    BOOL substituted=FALSE;BOOL* used=target==CP_UTF8?nullptr:&substituted;
    const DWORD flags=target==CP_UTF8?WC_ERR_INVALID_CHARS:WC_NO_BEST_FIT_CHARS;
    length=WideCharToMultiByte(target,flags,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,used);
    if(!length||substituted)throw std::runtime_error("Text is not representable in the legacy encoding");
    std::string output(length,'\0');
    WideCharToMultiByte(target,flags,wide.data(),static_cast<int>(wide.size()),output.data(),length,nullptr,used);
    if(substituted)throw std::runtime_error("Text is not representable in the legacy encoding");
    return output;
}
#else
std::string convert(std::string_view input,const char* source,const char* target){
    auto codec=iconv_open(target,source);
    if(codec==reinterpret_cast<iconv_t>(-1))throw std::runtime_error("Legacy text codec unavailable");
    std::string output(input.size()*4+16,'\0');
    char* in=const_cast<char*>(input.data());std::size_t in_size=input.size();
    char* out=output.data();std::size_t out_size=output.size();
    const auto result=iconv(codec,&in,&in_size,&out,&out_size);iconv_close(codec);
    if(result==std::size_t(-1)||in_size)throw std::runtime_error("Text is invalid or not representable in the legacy encoding");
    output.resize(output.size()-out_size);return output;
}
#endif
}
std::string decode_cp936(std::span<const std::uint8_t> bytes){
    auto input=std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size());
#ifdef _WIN32
    return convert(input,936,CP_UTF8);
#else
    return convert(input,"CP936","UTF-8");
#endif
}
Bytes encode_cp936(std::string_view input){
#ifdef _WIN32
    auto output=convert(input,CP_UTF8,936);
#else
    auto output=convert(input,"UTF-8","CP936");
#endif
    return {output.begin(),output.end()};
}
}
