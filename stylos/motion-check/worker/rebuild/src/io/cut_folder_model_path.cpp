#include "retas/cut_folder_model_path.hpp"
#include "native_ascii_string.hpp"
#include <algorithm>

namespace retas {
namespace {
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("CutFolder model path operation cancelled");}
std::string prefix(std::string_view value){
    if(value.size()>65535)throw std::length_error("CutFolder model path input exceeds65535 bytes");value=value.substr(0,value.find('\0'));
    if(value.size()>259)throw std::length_error("CutFolder model path exceeds259 bytes");
    if(std::ranges::any_of(value,[](unsigned char c){return c<32||c>=127||c=='/';}))throw std::invalid_argument("CutFolder model path requires printable ASCII and backslash delimiters");return std::string(value);
}
void bounded(std::string_view value){
    if(value.size()>259)throw std::length_error("CutFolder model path exceeds259 bytes");
    for(std::size_t at=0;at<value.size();){const auto end=value.find('\\',at);const auto n=(end==std::string_view::npos?value.size():end)-at;if(n>255)throw std::length_error("CutFolder model path component exceeds255 bytes");if(end==std::string_view::npos)break;at=end+1;}
}
char lower(char c){return c>='A'&&c<='Z'?char(c+32):c;}
std::string folded(std::string value){for(auto& c:value)c=lower(c);return value;}
void strip_back(std::string& value){while(!value.empty()&&value.back()=='\\')value.pop_back();}
bool drive_root(std::string_view value){return value.size()==3&&value[1]==':'&&value[2]=='\\'&&((value[0]>='A'&&value[0]<='Z')||(value[0]>='a'&&value[0]<='z'));}
void unc(std::string_view value){
    if(!value.starts_with("\\\\"))return;const auto end=value.find('\\',2);const auto server=value.substr(2,end==std::string_view::npos?value.size()-2:end-2);
    if(server.empty()||server=="."||server=="?"||end==std::string_view::npos||end+1==value.size()||value[end+1]=='\\')throw std::invalid_argument("CutFolder model path requires an ordinary UNC server/share");
}
std::string full_path(std::string value){
    if(value.empty()||value.back()!='\\')return value;
    if(drive_root(value)||value=="\\")return value;
    if(value.starts_with("\\\\"))throw std::invalid_argument("CutFolder model trailing UNC path requires legacy PathIsRootA");
    strip_back(value);return value;
}
std::string parent(std::string value){
    strip_back(value);const auto delimiter=value.rfind('\\');if(delimiter!=std::string::npos)return value.substr(0,delimiter+1);
    return value.size()>=2&&value[1]==':'?value.substr(0,2):std::string{};
}
void append(std::string& base,std::string_view component){
    if(base.empty()){base=component;bounded(base);return;}if(component.empty())return;strip_back(base);while(!component.empty()&&component.front()=='\\')component.remove_prefix(1);base+='\\';base+=component;strip_back(base);bounded(base);
}
std::string remake(std::string root,std::string path){
    if((path.size()>=2&&path[1]==':')||path.starts_with("\\\\")){bounded(path);return path;}
    // The native normalized pszPath temporary is not the working owner.
    // Remake copies the original root; empty appends retain its delimiters.
    while(path.starts_with("..\\")){root=parent(std::move(root));path.erase(0,3);}
    for(std::size_t at=0;at<path.size();){const auto end=path.find('\\',at);const auto size=(end==std::string::npos?path.size():end)-at;append(root,std::string_view(path).substr(at,size));if(end==std::string::npos)break;at=end+1;}bounded(root);return root;
}
// MakeRelativePath copies both strings, then unconditionally removes their
// trailing backslashes. Drive roots would leave its index3 read outside the
// initialized string; the bounded profile rejects that native unsafe domain.
std::string relative(std::string base,std::string target){
    strip_back(base);strip_back(target);if(base.size()<3||target.size()<3)throw std::invalid_argument("CutFolder relative paths require initialized native prefixes");
    if(base.front()!=target.front())return target;std::size_t at=3;
    if(base.starts_with("\\\\")&&target.starts_with("\\\\")){
        unc(base);unc(target);at=2;while(at<base.size()&&at<target.size()&&base[at]!='\\'&&target[at]!='\\'&&lower(base[at])==lower(target[at]))++at;
        if(at==base.size()||at==target.size()||base[at]!='\\'||target[at]!='\\')return target;
    }else if(base.size()<4||target.size()<4||base[1]!=':'||target[1]!=':'||base[2]!='\\'||target[2]!='\\')throw std::invalid_argument("CutFolder relative paths require non-root drive paths or ordinary UNC paths");
    while(at<base.size()&&at<target.size()&&lower(base[at])==lower(target[at]))++at;
    if(at==base.size()&&at==target.size())return {};
    if(at==base.size()&&at<target.size()&&target[at]=='\\')return target.substr(at+1);
    const bool target_parent=at==target.size()&&at<base.size()&&base[at]=='\\';
    std::size_t ups=at==base.size()||base[at]!='\\';ups+=std::count(base.begin()+std::ptrdiff_t(at),base.end(),'\\');std::string out;
    for(std::size_t n=0;n<ups;++n){out+="..\\";bounded(out);}if(!target_parent){const auto previous=target.rfind('\\',at?at-1:0);if(previous==std::string::npos)throw std::invalid_argument("CutFolder relative path has no initialized delimiter");out+=target.substr(previous+1);}bounded(out);return out;
}
}
std::string load_cut_folder_model_path(std::string_view root,std::span<const std::uint8_t> wire,std::int32_t flag,std::stop_token stop){
    poll(stop);if(wire.size()>65535)throw std::length_error("CutFolder model wire path exceeds65535 bytes");
    struct Work {std::uint64_t used{};std::stop_token stop;void visit(std::uint64_t n){poll(stop);constexpr auto cap=128ull*1024*1024;if(n>cap-used)throw std::length_error("CutFolder model path exceeds128Mi work");used+=n;}} work{0,stop};
    const auto decoded=native_ascii_detail::decode_string(wire,work);auto path=prefix(std::string_view(reinterpret_cast<const char*>(decoded.data()),decoded.size()));if(path.empty())return path;
    if(flag){for(std::size_t n=0;n<path.size();++n)if(path[n]==':'&&!(n==1&&path.size()>2&&path[2]==':'))path[n]='\\';auto base=prefix(root);bounded(base);path=remake(std::move(base),std::move(path));}
    bounded(path);poll(stop);return path;
}
CutFolderModelPath save_cut_folder_model_path(std::string_view root,std::string_view binding,std::stop_token stop){
    poll(stop);auto target=full_path(prefix(binding));bounded(target);if(target.empty())return {};const auto base=full_path(prefix(root));bounded(base);if(base.empty())throw std::invalid_argument("CutFolder model saving requires a nonempty root");
    std::int32_t flag{};if(folded(target).find(folded(base))!=std::string::npos){target=relative(base,std::move(target));flag=1;std::replace(target.begin(),target.end(),'\\',':');}poll(stop);return {ascii(target),flag};
}
}
