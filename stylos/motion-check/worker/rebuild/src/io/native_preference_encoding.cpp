#include "native_preference_encoding.hpp"
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
namespace native_preference_detail {
void validate_encoding(NativePreferenceEncoding e){switch(e.code_page){case 20127:case 932:case 936:case 949:case 950:case 1361:case 1252:case 65001:return;default:throw std::invalid_argument("Unsupported explicit native preference code page");}}
bool lead(std::uint8_t c,NativePreferenceEncoding e){switch(e.code_page){case 932:return (c>=0x81&&c<=0x9f)||(c>=0xe0&&c<=0xfc);case 936:case 949:case 950:return c>=0x81&&c<=0xfe;case 1361:return (c>=0x81&&c<=0xd3)||(c>=0xd8&&c<=0xde)||(c>=0xe0&&c<=0xf9);default:return false;}}
bool trail(std::uint8_t c,NativePreferenceEncoding e){switch(e.code_page){case 932:return (c>=0x40&&c<=0x7e)||(c>=0x80&&c<=0xfc);case 936:return c>=0x40&&c<=0xfe;case 949:return c>=0x41&&c<=0xfe;case 950:return (c>=0x40&&c<=0x7e)||(c>=0xa1&&c<=0xfe);case 1361:return (c>=0x31&&c<=0x7e)||(c>=0x81&&c<=0xfe);default:return false;}}
bool double_byte(std::string_view s,std::size_t i,NativePreferenceEncoding e){
    if(i>=s.size())return false;
    if(i+1<s.size()&&lead(std::uint8_t(s[i]),e)&&trail(std::uint8_t(s[i+1]),e))return true;
    if(!i||!lead(std::uint8_t(s[i-1]),e)||!trail(std::uint8_t(s[i]),e))return false;
    return i==1||!lead(std::uint8_t(s[i-2]),e)||!trail(std::uint8_t(s[i-1]),e);
}
bool equal(std::span<const std::uint8_t> a,std::span<const std::uint8_t> b,NativePreferenceEncoding e){
    const auto next=[e](auto s,std::size_t& i){if(i==s.size())return 0u;unsigned c=s[i++];if(lead(std::uint8_t(c),e)){if(i==s.size()||!s[i])return 0u;c=(c<<8)|s[i++];}return c;};
    std::size_t i{},j{};for(;;){const auto x=next(a,i),y=next(b,j);if(x!=y)return false;if(!x)return true;}
}
int compare_c_locale(std::string_view a,std::string_view b){
    const auto byte=[](std::string_view s,std::size_t i){
        const auto c=i<s.size()?std::uint8_t(s[i]):std::uint8_t{};
        return c>='A'&&c<='Z'?std::uint8_t(c+('a'-'A')):c;
    };
    for(std::size_t i=0;;++i){const auto x=byte(a,i),y=byte(b,i);if(x!=y)return x<y?-1:1;if(!x)return 0;}
}
}
namespace {
#ifndef _WIN32
const char* codec(unsigned cp){switch(cp){case 20127:return "ASCII";case 932:return "CP932";case 936:return "CP936";case 949:return "CP949";case 950:return "CP950";case 1361:return "JOHAB";case 1252:return "CP1252";case 65001:return "UTF-8";}throw std::invalid_argument("Unsupported native preference encoding");}
#endif
std::string convert(std::string_view input,unsigned from,unsigned to,std::size_t limit=262140){
    if(input.size()>limit)throw std::length_error("Native preference text conversion limit");
    if(input.empty())return {};
    if(from==20127&&std::ranges::any_of(input,[](unsigned char c){return c>=128;}))throw std::invalid_argument("Native preference text is not ASCII");
#ifdef _WIN32
    const int n=int(input.size()),count=MultiByteToWideChar(from,MB_ERR_INVALID_CHARS,input.data(),n,nullptr,0);
    if(count<=0)throw std::invalid_argument("Invalid native preference encoded text");std::wstring wide(std::size_t(count),L'\0');
    if(MultiByteToWideChar(from,MB_ERR_INVALID_CHARS,input.data(),n,wide.data(),count)!=count)throw std::invalid_argument("Cannot decode native preference text");
    BOOL substituted{};auto* used=to==65001?nullptr:&substituted;const DWORD flags=to==65001?WC_ERR_INVALID_CHARS:WC_NO_BEST_FIT_CHARS;
    const int bytes=WideCharToMultiByte(to,flags,wide.data(),count,nullptr,0,nullptr,used);
    if(bytes<=0||substituted)throw std::invalid_argument("Unrepresentable native preference text");std::string out(std::size_t(bytes),'\0');
    if(WideCharToMultiByte(to,flags,wide.data(),count,out.data(),bytes,nullptr,used)!=bytes||substituted)throw std::invalid_argument("Cannot encode native preference text");return out;
#else
    auto descriptor=iconv_open(codec(to),codec(from));if(descriptor==reinterpret_cast<iconv_t>(-1))throw std::runtime_error("Native preference text codec unavailable");
    struct Close{iconv_t value;~Close(){iconv_close(value);}} close{descriptor};
    std::string out(input.size()*4+16,'\0');char* in=const_cast<char*>(input.data());auto left=input.size();char* dest=out.data();auto room=out.size();
    const auto status=iconv(descriptor,&in,&left,&dest,&room);if(status==std::size_t(-1)||status||left)throw std::invalid_argument("Invalid or lossy native preference text conversion");out.resize(out.size()-room);return out;
#endif
}
std::string encode_utf8(std::string_view text,unsigned cp,std::size_t limit=262140){
    auto encoded=convert(text,65001,cp,limit);
#ifdef __APPLE__
    // Darwin iconv can report success and output NUL for representable
    // JOHAB fullwidth letters. Its decoder handles those pairs correctly.
    if(cp==1361&&encoded.find('\0')!=std::string::npos&&convert(encoded,cp,65001,limit*4)!=text){
        encoded.clear();
        for(std::size_t i=0;i<text.size();){
            const auto lead=std::uint8_t(text[i]);
            const auto length=lead<128?1u:lead<224?2u:lead<240?3u:4u;
            const auto ch=text.substr(i,length);auto bytes=convert(ch,65001,cp);
            if(bytes.size()==1&&bytes[0]==0&&ch!=std::string_view("\0",1)){
                const auto ks=convert(ch,65001,949);
                if(ks.size()!=2)throw std::invalid_argument("Unrepresentable JOHAB character");
                const unsigned row=std::uint8_t(ks[0]),col=std::uint8_t(ks[1]);
                const bool symbol=row>=0xa1&&row<=0xac,hanja=row>=0xca&&row<=0xfd;
                if((!symbol&&!hanja)||col<0xa1||col>0xfe)throw std::invalid_argument("Invalid JOHAB code unit");
                const unsigned offset=row-(symbol?0xa1:0xca);
                const unsigned trail=(offset%2)?col:(col<0xef?col-0x70:col-0x5e);
                bytes={char((symbol?0xd9:0xe0)+offset/2),char(trail)};
            }
            encoded+=bytes;i+=ch.size();
        }
    }
#endif
    return encoded;
}

}
Bytes encode_native_preference_text(std::string_view input,NativePreferenceEncoding e){native_preference_detail::validate_encoding(e);auto raw=encode_utf8(input,e.code_page);if(convert(raw,e.code_page,65001,4*262140)!=input)throw std::invalid_argument("Native preference encoding does not round trip");return {raw.begin(),raw.end()};}
std::string decode_native_preference_text(std::span<const std::uint8_t> input,NativePreferenceEncoding e){native_preference_detail::validate_encoding(e);const std::string_view raw(reinterpret_cast<const char*>(input.data()),input.size());auto utf8=convert(raw,e.code_page,65001);if(encode_utf8(utf8,e.code_page,4*262140)!=raw)throw std::invalid_argument("Native preference decoding does not round trip");return utf8;}
}
