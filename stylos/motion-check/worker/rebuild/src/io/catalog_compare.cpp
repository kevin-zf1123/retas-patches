#include "catalog_compare.hpp"
#include "native_preference_encoding.hpp"
#include <memory>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace retas::cut_folder_detail {
namespace {
#ifndef _WIN32
// Portable uppercase projections of round-tripping double-byte characters.
// Each row is an inclusive interval of packed ACP units and its signed offset.
// Single-byte inputs follow the separate CRT table walk.
struct Mapping {unsigned first,last;int delta;};
constexpr Mapping cp932[]{
    {0x8281,0x829a,-33},
    {0x83bf,0x83d6,-32},
    {0x8470,0x847e,-48},
    {0x8480,0x8491,-49},
    {0xeeef,0xeef8,-26523}};
constexpr Mapping cp936[]{
    {0xa2a1,0xa2aa,80},
    {0xa3e1,0xa3fa,-32},
    {0xa6c1,0xa6d8,-32},
    {0xa7d1,0xa7f1,-48}};
constexpr Mapping cp949[]{
    {0xa3e1,0xa3fa,-32},
    {0xa5a1,0xa5aa,15},
    {0xa5e1,0xa5f8,-32},
    {0xa9a1,0xa9a1,-256},
    {0xa9a3,0xa9a3,-257},
    {0xa9a4,0xa9a4,-256},
    {0xa9a5,0xa9a5,-43356},
    {0xa9a6,0xa9a6,-256},
    {0xa9a8,0xa9ab,-256},
    {0xa9ac,0xa9ac,-22105},
    {0xa9ad,0xa9af,-256},
    {0xacd1,0xacf1,-48}};
constexpr Mapping cp950[]{
    {0xa2e9,0xa2fe,-26},
    {0xa340,0xa343,-91},
    {0xa35c,0xa373,-24},
    {0xc7cc,0xc7d5,-27},
    {0xc7dc,0xc7e8,-33}};
constexpr Mapping cp1361[]{
    {0xda71,0xda7e,-32},
    {0xda91,0xda9c,-50},
    {0xdb31,0xdb3a,15},
    {0xdb71,0xdb7e,-32},
    {0xdb91,0xdb9a,-50},
    {0xdd31,0xdd31,-144},
    {0xdd33,0xdd33,-145},
    {0xdd34,0xdd34,-144},
    {0xdd35,0xdd35,-56556},
    {0xdd36,0xdd36,-144},
    {0xdd38,0xdd3b,-144},
    {0xdd3c,0xdd3c,-35305},
    {0xdd3d,0xdd3f,-144},
    {0xded1,0xdef1,-48}};
unsigned uppercase(unsigned unit,unsigned cp){
    std::span<const Mapping> ranges;
    switch(cp){case 932:ranges=cp932;break;case 936:ranges=cp936;break;case 949:ranges=cp949;break;
        case 950:ranges=cp950;break;case 1361:ranges=cp1361;break;default:return unit;}
    for(auto r:ranges)if(unit>=r.first&&unit<=r.last)return unsigned(int(unit)+r.delta);
    return unit;
}
#else
unsigned uppercase(unsigned unit,unsigned cp){
    const char bytes[]{char(unit>>8),char(unit)};
    wchar_t in[2],out[4];
    const int count=MultiByteToWideChar(cp,MB_ERR_INVALID_CHARS,bytes,2,in,2);
    const LCID locale=cp==932?0x411:cp==936?0x804:cp==949?0x412:cp==950?0x404:LOCALE_USER_DEFAULT;
    if(count<=0)return unit;
    const int mapped=LCMapStringW(locale,LCMAP_UPPERCASE,in,count,out,4);
    if(mapped<=0)return unit;
    unsigned char narrow[2];BOOL substituted=FALSE;
    const int size=WideCharToMultiByte(cp,WC_NO_BEST_FIT_CHARS,out,mapped,reinterpret_cast<char*>(narrow),2,nullptr,&substituted);
    if(substituted||size<1)return unit;
    return size==1?narrow[0]:(unsigned(narrow[0])<<8)|narrow[1];
}
#endif
}
std::u16string comparison_key(std::string_view raw,NativePreferenceEncoding encoding){
    std::u16string result;result.reserve(raw.size());
    for(std::size_t i=0;i<raw.size();){
        unsigned unit=std::uint8_t(raw[i++]);if(!unit)break;
        if(native_preference_detail::lead(std::uint8_t(unit),encoding)){
            if(i==raw.size()||!raw[i])break;
            unit=uppercase((unit<<8)|std::uint8_t(raw[i++]),encoding.code_page);
        }else if(unit>='A'&&unit<='Z')unit+='a'-'A';
        result.push_back(char16_t(unit));
    }
    return result;
}
std::optional<int> shell_compare(const std::filesystem::path& a,const std::filesystem::path& b){
#ifdef _WIN32
    const auto hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(hr)&&hr!=RPC_E_CHANGED_MODE)return {};
    struct Apartment {bool active;~Apartment(){if(active)CoUninitialize();}} apartment{SUCCEEDED(hr)};
    const auto parse=[](const auto& path){
        PIDLIST_ABSOLUTE id=nullptr;SHParseDisplayName(path.c_str(),nullptr,&id,0,nullptr);
        return std::unique_ptr<ITEMIDLIST,decltype(&CoTaskMemFree)>(id,&CoTaskMemFree);
    };
    const auto left=parse(a),right=parse(b);if(!left||!right)return {};
    IShellFolder* parent=nullptr;
    PCUIDLIST_RELATIVE x=left.get(),y=right.get();
    if(a.parent_path()==b.parent_path()){
        PCUITEMID_CHILD child=nullptr;
        if(FAILED(SHBindToParent(left.get(),IID_PPV_ARGS(&parent),&child)))return {};
        x=child;y=ILFindLastID(right.get());
    }else if(FAILED(SHGetDesktopFolder(&parent)))return {};
    const auto result=parent->CompareIDs(0,x,y);parent->Release();
    if(SUCCEEDED(result))return std::int16_t(HRESULT_CODE(result));
#else
    (void)a;(void)b;
#endif
    return {};
}
}
