#include <cmath>
#include "retas/score.hpp"
#include "retas/score_csv.hpp"
#include "retas/tsf.hpp"
#include "retas/document.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <list>
#include <map>
#include <unordered_map>
#include <type_traits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace retas {
namespace {
static_assert(std::is_nothrow_move_assignable_v<ScoreData> && std::is_nothrow_move_assignable_v<std::optional<ScoreSoundBinding>>);
struct BusyGuard {
    bool& busy;
    explicit BusyGuard(bool& value):busy(value){if(busy)throw std::logic_error("Reentrant Score transaction");busy=true;}
    ~BusyGuard(){busy=false;}
};
struct SoundToken {std::uint64_t device{},file{},file_high{};bool operator==(const SoundToken&)const=default;};
#ifdef _WIN32
struct SoundHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit SoundHandle(HANDLE handle):value(handle){}
    SoundHandle(const SoundHandle&)=delete;SoundHandle& operator=(const SoundHandle&)=delete;
    ~SoundHandle(){if(value!=INVALID_HANDLE_VALUE)::CloseHandle(value);}
};
[[noreturn]] void sound_error(DWORD error,const char* message){throw std::system_error(int(error),std::system_category(),message);}
bool sound_missing(DWORD error){return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND;}
SoundToken handle_token(HANDLE handle){
    BY_HANDLE_FILE_INFORMATION info{};
    if(!::GetFileInformationByHandle(handle,&info))sound_error(::GetLastError(),"Cannot identify Score sound file");
    if(::GetFileType(handle)!=FILE_TYPE_DISK||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("Score sound file is not a regular owned file");
    // ReFS requires the full 128-bit ID; the older 64-bit index is not unique.
    FILE_ID_INFO identity{};
    if(!::GetFileInformationByHandleEx(handle,FileIdInfo,&identity,sizeof(identity)))sound_error(::GetLastError(),"Cannot read Score sound file identity");
    SoundToken token{identity.VolumeSerialNumber};
    std::memcpy(&token.file,identity.FileId.Identifier,sizeof(token.file));
    std::memcpy(&token.file_high,identity.FileId.Identifier+sizeof(token.file),sizeof(token.file_high));
    return token;
}
std::optional<SoundToken> file_token(const std::filesystem::path& path){
    SoundHandle handle(::CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(handle.value==INVALID_HANDLE_VALUE){const auto error=::GetLastError();if(sound_missing(error))return std::nullopt;sound_error(error,"Cannot inspect Score sound file");}
    return handle_token(handle.value);
}
void remove_sound_file(const std::filesystem::path& path,SoundToken expected){
    // Deny rename/delete sharing while checking and deleting this exact handle.
    SoundHandle handle(::CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(handle.value==INVALID_HANDLE_VALUE){const auto error=::GetLastError();if(sound_missing(error))return;sound_error(error,"Cannot open bound Score sound for deletion");}
    if(handle_token(handle.value)!=expected)throw std::runtime_error("Bound Score sound file was replaced");
    FILE_DISPOSITION_INFO disposition{TRUE};
    if(!::SetFileInformationByHandle(handle.value,FileDispositionInfo,&disposition,sizeof(disposition)))sound_error(::GetLastError(),"Cannot remove bound Score sound file");
}
#else
std::optional<SoundToken> file_token(const std::filesystem::path& path){
    struct stat info{};if(::lstat(path.c_str(),&info)!=0){if(errno==ENOENT)return std::nullopt;throw std::system_error(errno,std::generic_category(),"Cannot inspect Score sound file");}
    if(!S_ISREG(info.st_mode))throw std::runtime_error("Score sound file is not a regular owned file");return SoundToken{std::uint64_t(info.st_dev),std::uint64_t(info.st_ino)};
}
void remove_sound_file(const std::filesystem::path& path,SoundToken expected){
    const auto actual=file_token(path);if(!actual)return;if(*actual!=expected)throw std::runtime_error("Bound Score sound file was replaced");
    if(::unlink(path.c_str())!=0&&errno!=ENOENT)throw std::system_error(errno,std::generic_category(),"Cannot remove bound Score sound file");
}
#endif
// Only files whose identity was created by this operation are rollback targets.
// Operations assume serial filesystem access; identity checks also protect
// against accidental replacement between independent document commands.
struct SoundCopy {
    std::filesystem::path path;SoundToken token{};bool active{};
    explicit SoundCopy(std::filesystem::path p):path(std::move(p)){}
    SoundCopy(const SoundCopy&)=delete;SoundCopy& operator=(const SoundCopy&)=delete;
    ~SoundCopy(){if(active){try{remove_sound_file(path,token);}catch(...) {}}}
};
void stage_sound(const std::filesystem::path& source,SoundCopy& staged){
#ifdef _WIN32
    SoundHandle output(::CreateFileW(staged.path.c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(output.value==INVALID_HANDLE_VALUE)sound_error(::GetLastError(),"Cannot stage Score sound copy");
    try{staged.token=handle_token(output.value);staged.active=true;}
    catch(...){FILE_DISPOSITION_INFO disposition{TRUE};(void)::SetFileInformationByHandle(output.value,FileDispositionInfo,&disposition,sizeof(disposition));throw;}
    std::ifstream input(source,std::ios::binary);if(!input)throw std::runtime_error("Cannot read Score sound source");std::array<char,65536> buffer{};std::uint64_t copied{};
    while(input){input.read(buffer.data(),std::streamsize(buffer.size()));const auto count=std::size_t(input.gcount());if(count>score_sound_max_file_bytes-copied)throw std::length_error("Score sound copy exceeds supported limit");copied+=count;
        for(std::size_t at=0;at<count;){DWORD written{};if(!::WriteFile(output.value,buffer.data()+at,DWORD(count-at),&written,nullptr))sound_error(::GetLastError(),"Cannot write staged Score sound");if(!written)throw std::runtime_error("Short staged Score sound write");at+=written;}}
    if(!input.eof())throw std::runtime_error("Cannot finish Score sound source read");
    const auto closing=output.value;output.value=INVALID_HANDLE_VALUE;if(!::CloseHandle(closing))sound_error(::GetLastError(),"Cannot close staged Score sound");
#else
    const int fd=::open(staged.path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)throw std::system_error(errno,std::generic_category(),"Cannot stage Score sound copy");
    struct stat info{};if(::fstat(fd,&info)!=0){const auto error=errno;::close(fd);::unlink(staged.path.c_str());throw std::system_error(error,std::generic_category(),"Cannot identify staged Score sound");}
    staged.token={std::uint64_t(info.st_dev),std::uint64_t(info.st_ino)};staged.active=true;
    int output=fd;
    try {
        std::ifstream input(source,std::ios::binary);if(!input)throw std::runtime_error("Cannot read Score sound source");std::array<char,65536> buffer{};std::uint64_t copied{};
        while(input){input.read(buffer.data(),std::streamsize(buffer.size()));const auto count=std::size_t(input.gcount());if(count>score_sound_max_file_bytes-copied)throw std::length_error("Score sound copy exceeds supported limit");copied+=count;
            for(std::size_t at=0;at<count;){const auto n=::write(output,buffer.data()+at,count-at);if(n<0){if(errno==EINTR)continue;throw std::system_error(errno,std::generic_category(),"Cannot write staged Score sound");}if(!n)throw std::runtime_error("Short staged Score sound write");at+=std::size_t(n);}}
        if(!input.eof())throw std::runtime_error("Cannot finish Score sound source read");
        const int closing=output;output=-1;if(::close(closing)!=0)throw std::system_error(errno,std::generic_category(),"Cannot close staged Score sound");
    }catch(...){if(output>=0)::close(output);throw;}
#endif
}
void replace_sound_copy(const std::filesystem::path& staged,const std::filesystem::path& destination){
#ifdef _WIN32
    if(!::MoveFileExW(staged.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING))sound_error(::GetLastError(),"Cannot replace owned Score sound copy");
#else
    std::filesystem::rename(staged,destination);
#endif
}
std::vector<ScoreLayer>& bank(ScoreData& data,ScoreBank which) {
    switch(which){case ScoreBank::Primary:return data.primary_layers;case ScoreBank::Derived:return data.derived_layers;}
    throw std::invalid_argument("Invalid Score bank");
}
void range(const ScoreLayer& layer,std::size_t first,std::size_t count) {
    if(first>layer.cells.size()||count>layer.cells.size()-first)throw std::out_of_range("Score exposure range");
}
bool empty_objects(const Bytes& objects) {return objects.size()==8&&std::all_of(objects.begin(),objects.end(),[](auto byte){return byte==0;});}
std::int32_t wrapped_add(std::int32_t a,std::int32_t b){return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(a)+std::bit_cast<std::uint32_t>(b));}
std::int32_t wrapped_subtract(std::int32_t a,std::int32_t b){return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(a)-std::bit_cast<std::uint32_t>(b));}
bool adjust_insert_motion(ScoreMotionCommon& motion,std::int32_t index,std::int32_t request) {
    const bool reversed=motion.frame1<motion.frame0;const auto low=reversed?motion.frame1:motion.frame0,high=reversed?motion.frame0:motion.frame1;
    if(high<index)return false;
    const auto shifted_high=std::max(0,wrapped_add(high,request));
    if(low>=index){const auto shifted_low=std::max(0,wrapped_add(low,request));motion.frame0=reversed?shifted_high:shifted_low;motion.frame1=reversed?shifted_low:shifted_high;}
    else if(reversed)motion.frame0=shifted_high;else motion.frame1=shifted_high;
    return true;
}
enum class MotionAdjustment {Keep,Move,Delete};
void charge_motion_history(std::size_t& used,const ScoreMotionObject& motion,std::size_t fixed,unsigned copies) {
    auto add=[&](std::size_t size){if(size>score_motion_adjust_max_history_bytes-used)throw std::length_error("Adjusted Score motion history exceeds supported limit");used+=size;};add(fixed);
    for(unsigned i=0;i<copies;++i){add(motion.common.suffix.size());if(const auto* arrow=std::get_if<ScoreArrowAttributes>(&motion.attributes))add(arrow->suffix.size());else if(const auto* text=std::get_if<ScoreTextAttributes>(&motion.attributes)){add(text->text.size());add(text->suffix.size());}}
}
MotionAdjustment adjust_delete_motion(ScoreMotionCommon& motion,std::int32_t start,std::int32_t end,std::int32_t request,std::int32_t frames) {
    const bool reversed=motion.frame1<motion.frame0;const auto low=reversed?motion.frame1:motion.frame0,high=reversed?motion.frame0:motion.frame1;
    auto pair=[&](std::int32_t a,std::int32_t b){motion.frame0=reversed?b:a;motion.frame1=reversed?a:b;};
    auto points=[&]{const auto y_low=reversed?motion.y1:motion.y0,y_high=reversed?motion.y0:motion.y1;if(y_high<y_low){if(reversed)std::swap(motion.x0,motion.x1);std::swap(motion.y0,motion.y1);}};
    if(low>end){pair(std::max(0,wrapped_subtract(low,request)),std::max(0,wrapped_subtract(high,request)));points();return MotionAdjustment::Move;}
    if(low>=start&&high>end){pair(start,std::max(0,wrapped_subtract(high,request)));return MotionAdjustment::Move;}
    if(low<start&&high>=start&&high<=end){pair(low,std::max(low,start-1));points();return MotionAdjustment::Move;}
    if(low>=start&&high<=end)return MotionAdjustment::Delete;
    if(low<start&&high>end){const auto shifted_end=wrapped_subtract(high,request);pair(low,shifted_end>=frames?frames-1:shifted_end);return MotionAdjustment::Move;}
    return MotionAdjustment::Keep;
}
void validate_layer_name(const Bytes& name) {if(name.size()>65535||std::find(name.begin(),name.end(),0)!=name.end())throw std::invalid_argument("Invalid Score layer name");}
Bytes unused_layer_name(const std::vector<ScoreLayer>& primary) {
    auto folded=[](std::uint8_t byte){return byte>='a'&&byte<='z'?std::uint8_t(byte-'a'+'A'):byte;};
    for(std::size_t i=0;i<std::size_t(score_max_layers);++i) {
        const Bytes name=i<26?Bytes{std::uint8_t('A'+i)}:Bytes{std::uint8_t('@'+i/26),std::uint8_t('A'+i%26)};
        // Native CompareNoCase uses NUL-terminated multibyte strings. The
        // generated namespace is ASCII; keep raw bytes beyond NUL untouched.
        const auto used=std::any_of(primary.begin(),primary.end(),[&](const auto& layer){const auto end=std::find(layer.name.begin(),layer.name.end(),0);return std::size_t(end-layer.name.begin())==name.size()&&std::equal(layer.name.begin(),end,name.begin(),[&](auto a,auto b){return folded(a)==folded(b);});});
        if(!used)return name;
    }
    throw std::logic_error("No unused native Score layer name");
}
std::int32_t label_number(const ScoreCell& cell) {
    std::size_t i{};auto space=[](std::uint8_t c){return c==' '||(c>='\t'&&c<='\r');};
    while(i<cell.label.size()&&space(cell.label[i]))++i;
    bool negative{};if(i<cell.label.size()&&(cell.label[i]=='+'||cell.label[i]=='-'))negative=cell.label[i++]=='-';
    std::int32_t number{};while(i<cell.label.size()&&cell.label[i]>='0'&&cell.label[i]<='9')number=number*10+(cell.label[i++]-'0');
    return negative?-number:number;
}
void put_number(ScoreCell& cell,std::int32_t number) {
    std::array<char,12> text{};const auto result=std::to_chars(text.data(),text.data()+text.size(),number);
    if(result.ec!=std::errc{})throw std::overflow_error("Score cel number cannot be formatted");
    cell.label.fill(0);std::copy_n(text.begin(),std::min<std::size_t>(8,result.ptr-text.data()),cell.label.begin());cell.marker=1;
}
void put_blank(ScoreCell& cell) {cell.marker=8;cell.label[0]=0;}
void copy_row(const std::vector<ScoreCell>& primary,std::vector<ScoreCell>& derived,const std::function<void()>& visit) {
    // Native 4FC140 retains its min/max trackers when earlier cells are
    // raised. Logical map/counter values also survive label truncation; only
    // the prefix-bump helper parses the already-written eight-byte labels.
    std::map<std::int32_t,std::int32_t> mapping;
    std::int32_t previous{},current{},saved{},maximum{},minimum=std::numeric_limits<std::int32_t>::max();std::size_t last{};
    auto bump_prefix=[&](std::size_t end){for(std::size_t k=0;k<end;++k){visit();if(derived[k].marker&1){const auto number=label_number(derived[k]);if(number>0)put_number(derived[k],number+1);}}};
    auto bump_mapping=[&]{for(auto& [key,value]:mapping){(void)key;visit();++value;}};
    for(std::size_t frame=0;frame<primary.size();++frame) {
        visit();if(!(primary[frame].marker&1))continue;const auto source=label_number(primary[frame]);if(source<=0)continue;
        if(!previous) {
            previous=source;maximum=minimum=source;
            std::int32_t delta{},minimum_delta=1;bool stepped{},last_down{};
            for(std::size_t k=0;k<frame;++k){visit();const auto marker=primary[k].marker;if(marker&2){++delta;stepped=true;last_down=false;}else if(marker&4){--delta;stepped=true;last_down=true;minimum_delta=std::min(minimum_delta,delta);}}
            if(stepped) { // Without +/- in this prefix, even blank cells stay untouched.
                auto end_delta=delta+(last_down?-1:1);minimum_delta=std::min(minimum_delta,end_delta);
                std::int32_t prefix{};if(minimum_delta<=0){prefix=1-minimum_delta;end_delta+=prefix;}
                if(end_delta>previous)previous=end_delta;else if(end_delta<previous)prefix+=previous-end_delta;
                for(std::size_t k=0;k<frame;++k){visit();const auto marker=primary[k].marker;if(marker&2){put_number(derived[k],++prefix);maximum=std::max(maximum,prefix);}else if(marker&4){put_number(derived[k],--prefix);minimum=std::min(minimum,prefix);}else if(marker&8)put_blank(derived[k]);}
            }
            current=saved=previous;last=frame;put_number(derived[frame],current);maximum=std::max(maximum,current);minimum=std::min(minimum,current);mapping[source]=current;continue;
        }
        bool last_up{},last_down{};
        for(std::size_t k=last+1;k<frame;++k) {
            visit();const auto marker=primary[k].marker;
            if(marker&2){put_number(derived[k],++current);last_up=true;last_down=false;}
            else if(marker&4){if(current==1){bump_prefix(k);bump_mapping();}else if(current>1)--current;put_number(derived[k],current);last_up=false;last_down=true;}
            else if(marker&8){put_blank(derived[k]);last_up=last_down=false;}
            maximum=std::max(maximum,current);minimum=std::min(minimum,current);
        }
        const auto found=mapping.find(source);
        if(found!=mapping.end())current=found->second;
        else {
            current=last_up?maximum+1:((last_down||previous>source)?minimum-1:maximum+1);
            if(current<1){bump_prefix(frame);bump_mapping();current=1;}
        }
        put_number(derived[frame],current);previous=source;last=frame;saved=current;mapping[source]=current;
        maximum=std::max(maximum,current);minimum=std::min(minimum,current);
    }
    if(current)for(std::size_t k=last+1;k<primary.size();++k){visit();const auto marker=primary[k].marker;if(marker&2){put_number(derived[k],++saved);}else if(marker&4){if(saved==1)bump_prefix(k);else if(saved>1)--saved;put_number(derived[k],saved);}else if(marker&8)put_blank(derived[k]);}
}
}
ScoreDocument::ScoreDocument(ScoreData initial,ScoreSoundPathEncoding encoding)
    :data_(std::move(initial)),sound_encoding_(normalize_score_sound_path_encoding(encoding)){validate_score(data_);}
ScoreDocument::ScoreDocument(const ScoreDocument& other)
    :data_(other.data_),object_cache_(other.object_cache_),undo_(other.undo_),redo_(other.redo_),undo_next_(other.undo_next_),path_(other.path_),
     revision_(other.revision_),saved_(other.saved_),next_(other.next_),generation_(other.generation_),
     sound_binding_(other.sound_binding_),bound_sound_identity_(other.bound_sound_identity_),sound_dirty_(other.sound_dirty_),sound_encoding_(other.sound_encoding_) {
    if(other.busy_)throw std::logic_error("Cannot copy an active Score transaction");
}
void ScoreDocument::swap_contents(ScoreDocument& other) noexcept {
    using std::swap;swap(data_,other.data_);swap(object_cache_,other.object_cache_);undo_.swap(other.undo_);redo_.swap(other.redo_);path_.swap(other.path_);
    swap(revision_,other.revision_);swap(saved_,other.saved_);swap(next_,other.next_);
    swap(undo_next_,other.undo_next_);
    swap(sound_binding_,other.sound_binding_);swap(bound_sound_identity_,other.bound_sound_identity_);swap(sound_dirty_,other.sound_dirty_);swap(sound_encoding_,other.sound_encoding_);
}
ScoreDocument::ScoreDocument(ScoreDocument&& other) {
    if(other.busy_)throw std::logic_error("Cannot move an active Score transaction");
    if(other.generation_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Score generation exhausted");
    swap_contents(other);generation_=other.generation_;++other.generation_;
}
ScoreDocument& ScoreDocument::operator=(const ScoreDocument& other) {
    if(busy_)throw std::logic_error("Cannot replace an active Score transaction");
    if(this==&other)return *this;
    if(generation_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Score generation exhausted");
    ScoreDocument replacement(other);swap_contents(replacement);++generation_;return *this;
}
ScoreDocument& ScoreDocument::operator=(ScoreDocument&& other) {
    if(busy_)throw std::logic_error("Cannot replace an active Score transaction");
    if(this==&other)return *this;
    if(generation_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Score generation exhausted");
    ScoreDocument replacement(std::move(other));swap_contents(replacement);++generation_;return *this;
}
ScoreDocument ScoreDocument::open(const std::filesystem::path& path,ScoreSoundPathEncoding encoding) {
    const auto absolute_path=std::filesystem::absolute(path);
    std::ifstream input(absolute_path,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Cannot open Score file");
    const auto size=input.tellg();
    if(size<0||size>std::streamoff(score_max_file_bytes))throw FormatError(0,"Score file exceeds supported limit");
    Bytes bytes(static_cast<std::size_t>(size));input.seekg(0);
    if(!input.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))throw std::runtime_error("Short Score file read");
    ScoreDocument document(decode_score(bytes),encoding);document.path_=absolute_path;
    if(!document.data_.sound_reference.empty())try {
        auto audio=resolve_score_sound_reference(document.data_.sound_reference,absolute_path,document.sound_encoding_);
        auto info=read_score_sound_metadata(audio);auto identity=sound_identity(audio);document.sound_binding_=ScoreSoundBinding{std::move(audio),info,false};document.bound_sound_identity_=identity;
    }catch(const std::exception&){/* Native OPEN ignores sound binding failure. Keep the opaque FF06 bytes. */}
    return document;
}
ScoreDocument ScoreDocument::create(ScoreGeneral accepted,bool add_background) {
    if(accepted.frame_count<4||accepted.frame_count>score_max_frames||accepted.layer_count<1||accepted.layer_count>score_max_layers||!accepted.fps)
        throw std::invalid_argument("Invalid new Score dimensions or frame rate");
    if(accepted.settings.size()!=6||accepted.visibility.size()!=7||!accepted.suffix.empty())
        throw std::invalid_argument("New Score requires six explicit settings, seven visibility slots and no General extension");
    accepted.current_frame=accepted.current_layer=0;
    const std::array<std::uint16_t,5> defaults{1,4,8,2,16};std::copy(defaults.begin(),defaults.end(),accepted.settings.begin());
    accepted.visibility={1,1,1,0,1,1,1};ScoreData created;created.general=std::move(accepted);created.direction_mode=0;
    created.primary_layers.reserve(created.general.layer_count);created.derived_layers.reserve(created.general.layer_count);
    for(int i=0;i<created.general.layer_count;++i) {
        ScoreLayer layer;
        if(add_background&&i==0)layer.name=ascii("-BG");
        else {const int index=i-(add_background?1:0);if(index<26)layer.name={std::uint8_t('A'+index)};else layer.name={std::uint8_t('@'+index/26),std::uint8_t('A'+index%26)};}
        layer.cells.resize(created.general.frame_count);created.primary_layers.push_back(layer);created.derived_layers.push_back(std::move(layer));
    }
    created.voice.assign(created.general.frame_count,Bytes(6));created.objects=Bytes(8);return ScoreDocument(std::move(created));
}
ScoreDocument::Metadata ScoreDocument::metadata(const ScoreGeneral& g) noexcept {return {g.fps,g.scene,g.cut,g.frame_display,g.guide_line,g.page};}
void ScoreDocument::apply_metadata(ScoreGeneral& g,const Metadata& m) noexcept {g.fps=m.fps;g.scene=m.scene;g.cut=m.cut;g.frame_display=m.frame_display;g.guide_line=m.guide_line;g.page=m.page;}
ScoreDocument::ObjectState ScoreDocument::project_objects(const ScoreData& data) {
    ObjectState state;state.objects=decode_score_objects(data.objects);state.identities.reserve(state.objects.motion.size());
    for(std::size_t i=0;i<state.objects.motion.size();++i)state.identities.push_back(i+1);return state;
}
void ScoreDocument::mirror_objects(ScoreData& data,const ObjectState& objects) {data.objects=encode_score_objects(objects.objects,ScoreObjectsWriteMode::NativeCanonical);}
const ScoreObjects& ScoreDocument::typed_objects() const {if(!object_cache_)object_cache_=project_objects(data_);return object_cache_->objects;}
bool ScoreDocument::transact(const std::string& label,const std::function<void(ScoreData&)>& operation) {
    return transact_impl(label,operation,false);
}
bool ScoreDocument::transact_impl(const std::string& label,const std::function<void(ScoreData&)>& operation,bool direction_command) {
    BusyGuard guard(busy_);auto candidate=data_;operation(candidate);validate_score(candidate);
    if(candidate.general.frame_count!=data_.general.frame_count||candidate.general.layer_count!=data_.general.layer_count)
        throw std::invalid_argument("Score geometry changes require a dedicated structural command");
    if(candidate.objects!=data_.objects||candidate.sound_reference!=data_.sound_reference||candidate.sound_suffix!=data_.sound_suffix)
        throw std::invalid_argument("Opaque Score objects and sound binding require dedicated semantic commands");
    if(candidate.general.visibility!=data_.general.visibility)throw std::invalid_argument("Use the Score lane visibility command");
    if(candidate.direction_suffix!=data_.direction_suffix)
        throw std::invalid_argument("Opaque Score direction extensions cannot be edited");
    if(!direction_command&&candidate.direction_mode!=data_.direction_mode)
        throw std::invalid_argument("Use the Score direction command to change its nonhistory mode");
    if(!direction_command&&candidate==data_)return false;
    Revision before{direction_command?RevisionPayload(data_.direction):RevisionPayload(Snapshot{data_,object_cache_}),label,revision_};undo_.push_back(std::move(before));
    data_=std::move(candidate);redo_.clear();undo_next_=true;revision_=next_++;++generation_;return true;
}
bool ScoreDocument::set_exposure(ScoreBank which,std::size_t layer,std::size_t first,std::size_t count,std::span<const std::uint8_t> label) {
    if(label.size()>8||std::find(label.begin(),label.end(),0)!=label.end())throw std::invalid_argument("Score exposure label must contain at most eight non-NUL bytes");
    std::array<std::uint8_t,8> bytes{};std::copy(label.begin(),label.end(),bytes.begin());
    return transact("Set exposure",[&](auto& data){auto& target=bank(data,which).at(layer);range(target,first,count);for(std::size_t i=first;i<first+count;++i){target.cells[i].marker=1;target.cells[i].label=bytes;}});
}
bool ScoreDocument::clear_exposure(ScoreBank which,std::size_t layer,std::size_t first,std::size_t count) {
    return transact("Clear exposure",[&](auto& data){auto& target=bank(data,which).at(layer);range(target,first,count);for(std::size_t i=first;i<first+count;++i)target.cells[i].marker=0;});
}
void ScoreDocument::auto_input(ScoreBank which,int first_layer,int last_layer,int first_frame,int last_frame,
                               int start,int end,int spacing,int repeats) {
    BusyGuard guard(busy_);
    if(first_layer>last_layer)std::swap(first_layer,last_layer);
    if(first_frame>last_frame)std::swap(first_frame,last_frame);
    if(first_layer<0||last_layer>=data_.general.layer_count||first_frame<0||last_frame>=data_.general.frame_count)
        throw std::out_of_range("Invalid Score auto-input selection");
    if(start<=0||end<=0||spacing<=0||repeats<0)throw std::invalid_argument("Invalid Score auto-input parameters");
    auto& layers=bank(data_,which);
    CellRange before{which,std::size_t(first_layer),std::size_t(first_frame),{}};
    std::vector<std::vector<ScoreCell>> prepared;
    for(int layer=first_layer;layer<=last_layer;++layer){
        const auto& cells=layers.at(layer).cells;
        range(layers[layer],first_frame,last_frame-first_frame+1);
        auto& original=before.rows.emplace_back(cells.begin()+first_frame,cells.begin()+last_frame+1);
        auto& row=prepared.emplace_back(original);
        for(auto& cell:row)cell.marker=0;
        int number=start,remaining=repeats;
        for(std::size_t i=0;i<row.size();i+=std::size_t(spacing)){
            put_number(row[i],number);
            if(number!=end)number+=start<end?1:-1;
            else {
                if(start==end||remaining==1)break;
                if(remaining>1)--remaining;
                number=start;
            }
        }
    }
    undo_.push_back({std::move(before),"Auto input",revision_});
    for(auto& row:prepared)std::move(row.begin(),row.end(),layers[first_layer++].cells.begin()+first_frame);
    redo_.clear();undo_next_=true;revision_=next_++;++generation_;
}
bool ScoreDocument::copy_cel_numbers(const std::function<bool()>& cancelled) {
    BusyGuard guard(busy_);validate_score(data_);auto candidate=data_;std::size_t visits{};
    auto visit=[&]{if(++visits>score_copy_max_cell_visits)throw std::length_error("Score cel-number copy exceeds work limit");if(visits%512==0&&cancelled&&cancelled())throw ScoreCopyCancelled();};
    for(std::size_t layer=0;layer<candidate.derived_layers.size();++layer){if(cancelled&&cancelled())throw ScoreCopyCancelled();if(candidate.derived_layers[layer].field)copy_row(candidate.primary_layers[layer].cells,candidate.derived_layers[layer].cells,visit);}
    validate_score(candidate);if(cancelled&&cancelled())throw ScoreCopyCancelled();
    CellRange before{ScoreBank::Derived,0,0,{}};before.rows.reserve(data_.derived_layers.size());for(const auto& layer:data_.derived_layers)before.rows.push_back(layer.cells);
    Revision revision{std::move(before),"Copy cel numbers",revision_};undo_.push_back(std::move(revision));
    data_=std::move(candidate);redo_.clear();undo_next_=true;revision_=next_++;++generation_;return true;
}
bool ScoreDocument::rename_layer(std::size_t layer,Bytes name) {
    validate_layer_name(name);
    return transact("Rename layer",[&](auto& data){data.primary_layers.at(layer).name=name;data.derived_layers.at(layer).name=name;});
}
void ScoreDocument::attach_pair(ScoreData& data,std::size_t index,const LayerPair& pair) {
    if(data.general.layer_count>=score_max_layers||index>data.primary_layers.size()||index>data.derived_layers.size())
        throw std::logic_error("Score layer attach outside supported geometry");
    std::size_t growth{};auto account=[&](std::size_t size){if(size>score_max_file_bytes-growth)throw std::length_error("Score layer attach exceeds file limit");growth+=size;};
    for(const auto* layer:{&pair.primary,&pair.derived}){account(14+layer->name.size());for(const auto& cell:layer->cells){account(10);account(cell.suffix.size());}account(layer->suffix.size());}
    if(growth>score_max_file_bytes-encode_score(data).size())throw std::length_error("Score layer attach exceeds file limit");
    data.primary_layers.insert(data.primary_layers.begin()+std::ptrdiff_t(index),pair.primary);
    data.derived_layers.insert(data.derived_layers.begin()+std::ptrdiff_t(index),pair.derived);++data.general.layer_count;
}
ScoreDocument::LayerPair ScoreDocument::detach_pair(ScoreData& data,std::size_t index) {
    if(data.general.layer_count<=1||index>=data.primary_layers.size()||index>=data.derived_layers.size())
        throw std::logic_error("Score layer detach outside supported geometry");
    LayerPair pair{data.primary_layers[index],data.derived_layers[index]};
    data.primary_layers.erase(data.primary_layers.begin()+std::ptrdiff_t(index));data.derived_layers.erase(data.derived_layers.begin()+std::ptrdiff_t(index));
    --data.general.layer_count;if(data.general.current_layer>=data.general.layer_count)data.general.current_layer=data.general.layer_count-1;
    return pair;
}
void ScoreDocument::set_cursor(std::int32_t frame,std::int32_t layer){
    BusyGuard guard(busy_);
    if(frame<0||frame>=data_.general.frame_count||layer<0||layer>=data_.general.layer_count)throw std::out_of_range("Score cursor outside table");
    if(data_.general.current_frame==frame&&data_.general.current_layer==layer)return;
    data_.general.current_frame=frame;data_.general.current_layer=layer;++generation_;
}
bool ScoreDocument::insert_layer(std::int32_t index,Bytes name) {
    BusyGuard guard(busy_);validate_layer_name(name);validate_score(data_);
    if(index<-1||index>data_.general.layer_count)throw std::out_of_range("Score insert layer index");
    if(data_.general.layer_count>=score_max_layers)return false;
    const auto resolved=index==-1?data_.primary_layers.size():std::size_t(index);
    ScoreLayer layer;layer.name=std::move(name);layer.cells.resize(data_.general.frame_count);LayerPair pair{layer,std::move(layer)};
    auto candidate=data_;attach_pair(candidate,resolved,pair);validate_score(candidate);
    Revision before{LayerChange{resolved,false,std::nullopt},"Insert layer",revision_};undo_.push_back(std::move(before));
    data_=std::move(candidate);redo_.clear();undo_next_=true;revision_=next_++;++generation_;return true;
}
bool ScoreDocument::delete_layer(std::size_t index) {
    BusyGuard guard(busy_);validate_score(data_);if(index>=data_.primary_layers.size())throw std::out_of_range("Score delete layer index");
    if(data_.general.layer_count<=1)return false;
    auto candidate=data_;auto pair=detach_pair(candidate,index);validate_score(candidate);
    Revision before{LayerChange{index,true,std::move(pair)},"Delete layer",revision_};undo_.push_back(std::move(before));
    data_=std::move(candidate);redo_.clear();undo_next_=true;revision_=next_++;++generation_;return true;
}
ScoreDocument::FrameSlice ScoreDocument::frame_slice(const ScoreData& data,std::size_t index,std::size_t count) {
    FrameSlice saved;
    for(auto entry:{std::pair{&data.primary_layers,&saved.primary},std::pair{&data.derived_layers,&saved.derived}})
        for(const auto& layer:*entry.first)entry.second->emplace_back(layer.cells.begin()+std::ptrdiff_t(index),layer.cells.begin()+std::ptrdiff_t(index+count));
    saved.voice.assign(data.voice.begin()+std::ptrdiff_t(index),data.voice.begin()+std::ptrdiff_t(index+count));return saved;
}
void ScoreDocument::insert_frame_range(ScoreData& data,std::size_t index,std::size_t count,const FrameSlice* saved) {
    const auto frames=std::size_t(data.general.frame_count);
    if(!count||index>frames||count>std::size_t(score_max_frames)-frames)throw std::logic_error("Score frame insertion outside supported geometry");
    // Check serialized growth before allocating extension-filled records.
    // A valid old document can still have a large per-frame opaque stride.
    std::size_t stride=data.voice_stride;for(const auto* rows:{&data.primary_layers,&data.derived_layers})for(const auto& layer:*rows)stride+=layer.cell_stride;
    const auto old_size=encode_score(data).size();
    if(count>(score_max_file_bytes-old_size)/stride)throw std::length_error("Score frame insertion exceeds file limit");
    if(saved&&(saved->primary.size()!=data.primary_layers.size()||saved->derived.size()!=data.derived_layers.size()||saved->voice.size()!=count))
        throw std::logic_error("Score saved frame geometry changed");
    for(auto entry:{std::pair{&data.primary_layers,saved?&saved->primary:nullptr},std::pair{&data.derived_layers,saved?&saved->derived:nullptr}}) {
        for(std::size_t row=0;row<entry.first->size();++row) {
            auto& layer=(*entry.first)[row];const auto position=layer.cells.begin()+std::ptrdiff_t(index);
            if(entry.second){const auto& cells=entry.second->at(row);if(cells.size()!=count)throw std::logic_error("Score saved frame count changed");layer.cells.insert(position,cells.begin(),cells.end());}
            else {ScoreCell blank;blank.suffix.resize(layer.cell_stride-10);layer.cells.insert(position,count,blank);}
        }
    }
    const auto position=data.voice.begin()+std::ptrdiff_t(index);
    if(saved)data.voice.insert(position,saved->voice.begin(),saved->voice.end());else data.voice.insert(position,count,Bytes(data.voice_stride));
    data.general.frame_count+=std::int32_t(count);
}
void ScoreDocument::delete_frame_range(ScoreData& data,std::size_t index,std::size_t count) {
    const auto frames=std::size_t(data.general.frame_count);
    if(!count||index>frames||count>frames-index||count>frames-4)throw std::logic_error("Score frame deletion outside supported geometry");
    for(auto* rows:{&data.primary_layers,&data.derived_layers})for(auto& layer:*rows)layer.cells.erase(layer.cells.begin()+std::ptrdiff_t(index),layer.cells.begin()+std::ptrdiff_t(index+count));
    data.voice.erase(data.voice.begin()+std::ptrdiff_t(index),data.voice.begin()+std::ptrdiff_t(index+count));data.general.frame_count-=std::int32_t(count);
    if(data.general.current_frame>=data.general.frame_count)data.general.current_frame=data.general.frame_count-1;
}
bool ScoreDocument::insert_frames(std::int32_t index,std::size_t count){return insert_frames(index,count,false);}
bool ScoreDocument::insert_frames(std::int32_t index,std::size_t count,bool adjust_motion) {
    BusyGuard guard(busy_);validate_score(data_);
    if(!count)throw std::invalid_argument("Score frame insertion requires positive count");
    if(index<-1||index>data_.general.frame_count)throw std::out_of_range("Score insert frame index");
    if(adjust_motion&&count>std::size_t(std::numeric_limits<std::int32_t>::max()))throw std::invalid_argument("Adjusted Score frames require a positive int32 request");
    const auto frames=std::size_t(data_.general.frame_count);const auto actual=std::min(count,std::size_t(score_max_frames)-frames);if(!actual)return false;
    const auto resolved=index==-1?frames:std::size_t(index);auto candidate=data_;auto objects=object_cache_;RevisionPayload history;
    if(adjust_motion&&!objects)objects=project_objects(data_);insert_frame_range(candidate,resolved,actual,nullptr);
    if(adjust_motion){if(objects->identities.size()!=objects->objects.motion.size())throw std::logic_error("Score motion identity count changed");Compound compound;compound.reverse=true;compound.children.emplace_back(FrameChange{resolved,actual,false,std::nullopt});std::size_t history_bytes{};for(std::size_t i=0;i<objects->objects.motion.size();++i){auto& motion=objects->objects.motion[i];if(std::max(motion.common.frame0,motion.common.frame1)>=std::int32_t(resolved)){charge_motion_history(history_bytes,motion,sizeof(CompoundChild),2);auto before=motion;adjust_insert_motion(motion.common,std::int32_t(resolved),std::int32_t(count));compound.children.emplace_back(MotionMove{objects->identities[i],true,std::move(before),std::nullopt});}}mirror_objects(candidate,*objects);history=std::move(compound);}
    else history=FrameChange{resolved,actual,false,std::nullopt};
    validate_score(candidate);Revision before{std::move(history),"Insert frames",revision_};undo_.push_back(std::move(before));
    data_=std::move(candidate);object_cache_=std::move(objects);redo_.clear();undo_next_=true;revision_=next_++;++generation_;return true;
}
bool ScoreDocument::delete_frames(std::int32_t index,std::size_t count){return delete_frames(index,count,false);}
bool ScoreDocument::delete_frames(std::int32_t index,std::size_t count,bool adjust_motion) {
    BusyGuard guard(busy_);validate_score(data_);
    if(!count)throw std::invalid_argument("Score frame deletion requires positive count");
    if(index<-1||index>=data_.general.frame_count)throw std::out_of_range("Score delete frame index");
    if(adjust_motion&&count>std::size_t(std::numeric_limits<std::int32_t>::max()))throw std::invalid_argument("Adjusted Score frames require a positive int32 request");
    const auto frames=std::size_t(data_.general.frame_count);auto actual=std::min(count,frames-4);
    if(index!=-1)actual=std::min(actual,frames-std::size_t(index));if(!actual)return false;
    const auto resolved=index==-1?frames-actual:std::size_t(index);
    auto objects=object_cache_?*object_cache_:project_objects(data_);if(objects.identities.size()!=objects.objects.motion.size())throw std::logic_error("Score motion identity count changed");auto saved=frame_slice(data_,resolved,actual);auto candidate=data_;delete_frame_range(candidate,resolved,actual);
    Compound compound;compound.reverse=true;compound.children.emplace_back(FrameChange{resolved,actual,true,std::move(saved)});
    std::vector<ScoreMotionObject> surviving;std::vector<std::uint64_t> identities;surviving.reserve(objects.objects.motion.size());identities.reserve(objects.identities.size());
    const auto operation_start=std::min(std::int32_t(resolved),candidate.general.frame_count-1);const auto operation_end=wrapped_add(operation_start,std::int32_t(std::min(count,frames-4))-1);
    std::size_t history_bytes{};
    for(std::size_t i=0;i<objects.objects.motion.size();++i) {
        auto motion=std::move(objects.objects.motion[i]);auto action=MotionAdjustment::Keep;std::optional<ScoreMotionObject> before;
        if(adjust_motion){const std::array original{motion.common.frame0,motion.common.frame1,motion.common.x0,motion.common.y0,motion.common.x1,motion.common.y1};action=adjust_delete_motion(motion.common,operation_start,operation_end,std::int32_t(count),candidate.general.frame_count);if(action!=MotionAdjustment::Keep)charge_motion_history(history_bytes,motion,sizeof(CompoundChild),action==MotionAdjustment::Move?2:1);if(action==MotionAdjustment::Move){before=motion;auto& common=before->common;common.frame0=original[0];common.frame1=original[1];common.x0=original[2];common.y0=original[3];common.x1=original[4];common.y1=original[5];}}
        else {const bool first=motion.common.frame0>=candidate.general.frame_count,second=motion.common.frame1>=candidate.general.frame_count;if(first&&second)action=MotionAdjustment::Delete;else if(first||second){before=motion;action=MotionAdjustment::Move;if(first)motion.common.frame0=candidate.general.frame_count-1;if(second)motion.common.frame1=candidate.general.frame_count-1;}}
        if(action==MotionAdjustment::Delete)compound.children.emplace_back(MotionDelete{objects.identities[i],true,std::move(motion)});
        else {if(action==MotionAdjustment::Move)compound.children.emplace_back(MotionMove{objects.identities[i],true,std::move(*before),std::nullopt});surviving.push_back(std::move(motion));identities.push_back(objects.identities[i]);}
    }
    objects.objects.motion=std::move(surviving);objects.identities=std::move(identities);
    mirror_objects(candidate,objects);validate_score(candidate);Revision before{std::move(compound),"Delete frames",revision_};undo_.push_back(std::move(before));
    data_=std::move(candidate);object_cache_=std::move(objects);redo_.clear();undo_next_=true;revision_=next_++;++generation_;return true;
}
bool ScoreDocument::order_layers(const std::vector<std::size_t>& order) {
    if(order.size()!=data_.primary_layers.size())throw std::invalid_argument("Score layer order requires a complete permutation");
    std::vector<bool> seen(order.size());bool changed{};
    for(std::size_t i=0;i<order.size();++i){if(order[i]>=order.size()||seen[order[i]])throw std::invalid_argument("Invalid Score layer permutation");seen[order[i]]=true;changed|=order[i]!=i;}
    if(!changed)return false;
    if(!empty_objects(data_.objects)||!data_.primary_suffix.empty()||!data_.derived_suffix.empty())throw std::invalid_argument("Cannot relocate opaque Score object or layer-table references");
    return transact("Order layers",[&](auto& data){
        auto primary=data.primary_layers,derived=data.derived_layers;
        for(std::size_t i=0;i<order.size();++i){data.primary_layers[i]=std::move(primary[order[i]]);data.derived_layers[i]=std::move(derived[order[i]]);if(order[i]==std::size_t(data_.general.current_layer))data.general.current_layer=std::int32_t(i);}
    });
}
bool ScoreDocument::set_general(ScoreGeneral accepted) {
    BusyGuard guard(busy_);
    validate_score(data_);
    if(accepted.frame_count<4||accepted.frame_count>score_max_frames||accepted.layer_count<1||accepted.layer_count>score_max_layers||!accepted.fps)
        throw std::invalid_argument("Invalid Score General dimensions or frame rate");
    if(accepted.settings!=data_.general.settings||accepted.visibility!=data_.general.visibility||accepted.suffix!=data_.general.suffix)
        throw std::invalid_argument("Score General command cannot change session settings, visibility or opaque extensions");
    const bool dimensions=accepted.frame_count!=data_.general.frame_count||accepted.layer_count!=data_.general.layer_count;
    auto candidate=data_;auto objects=object_cache_;RevisionPayload history;
    if(!dimensions){history=metadata(data_.general);candidate.general=std::move(accepted);}
    else {
        if(!objects)objects=project_objects(data_);
        Compound compound;compound.reverse=true;const auto layer_difference=accepted.layer_count-data_.general.layer_count;compound.children.reserve(std::size_t(layer_difference<0?-layer_difference:layer_difference)+2);
        if(accepted.layer_count>candidate.general.layer_count) {
            auto size=encode_score(candidate).size();
            while(candidate.general.layer_count<accepted.layer_count) {
                auto name=unused_layer_name(candidate.primary_layers);const auto growth=2*(14+name.size()+std::size_t(candidate.general.frame_count)*10);
                if(growth>score_max_file_bytes-size)throw std::length_error("Score General layer growth exceeds file limit");size+=growth;
                ScoreLayer layer;layer.name=std::move(name);layer.cells.resize(candidate.general.frame_count);LayerPair pair{layer,std::move(layer)};
                const auto index=candidate.primary_layers.size();attach_pair(candidate,index,pair);compound.children.emplace_back(LayerChange{index,false,std::nullopt});
            }
        }
        else while(candidate.general.layer_count>accepted.layer_count) {
            const auto index=candidate.primary_layers.size()-1;auto pair=detach_pair(candidate,index);compound.children.emplace_back(LayerChange{index,true,std::move(pair)});
        }
        if(accepted.frame_count>candidate.general.frame_count) {
            const auto index=std::size_t(candidate.general.frame_count),count=std::size_t(accepted.frame_count-candidate.general.frame_count);
            insert_frame_range(candidate,index,count,nullptr);compound.children.emplace_back(FrameChange{index,count,false,std::nullopt});
        }
        else if(accepted.frame_count<candidate.general.frame_count) {
            const auto index=std::size_t(accepted.frame_count),count=std::size_t(candidate.general.frame_count-accepted.frame_count);auto saved=frame_slice(candidate,index,count);
            delete_frame_range(candidate,index,count);compound.children.emplace_back(FrameChange{index,count,true,std::move(saved)});
        }
        compound.children.emplace_back(metadata(candidate.general));apply_metadata(candidate.general,metadata(accepted));
        candidate.general.current_frame=std::min(candidate.general.current_frame,candidate.general.frame_count-1);
        candidate.general.current_layer=std::min(candidate.general.current_layer,candidate.general.layer_count-1);
        compound.children.emplace_back(PencilChange{true,objects->objects.pencils,std::nullopt});cleanup_pencils(objects->objects,candidate.general.frame_count,candidate.general.layer_count);mirror_objects(candidate,*objects);history=std::move(compound);
    }
    validate_score(candidate);Revision before{std::move(history),"Set general information",revision_};undo_.push_back(std::move(before));
    data_=std::move(candidate);object_cache_=std::move(objects);redo_.clear();undo_next_=true;revision_=next_++;++generation_;return true;
}
bool ScoreDocument::set_direction(Bytes text,std::int32_t mode) {
    if(text.size()>65535||std::find(text.begin(),text.end(),0)!=text.end())throw std::invalid_argument("Invalid Score direction string");
    if(data_.general.visibility.size()<6)throw std::invalid_argument("Missing Score direction visibility field");
    const auto changed=transact_impl("Set direction",[&](auto& data){data.direction=std::move(text);data.direction_mode=mode;},true);
    const auto revealed=set_lane_visible(ScoreLane::Direction,true);return changed||revealed;
}
bool ScoreDocument::set_lane_visible(ScoreLane lane,bool visible) {
    if(busy_)throw std::logic_error("Reentrant Score visibility edit");
    const auto index=static_cast<std::size_t>(lane);
    if(index>=7||index>=data_.general.visibility.size())throw std::out_of_range("Missing Score lane visibility field");
    auto& value=data_.general.visibility[index];const std::int32_t new_value=visible?1:0;
    if(value==new_value)return false;value=new_value;++generation_;return true;
}
namespace {
void cut_pencil_point(std::vector<ScorePencilObject>& pencils,std::size_t i,std::size_t invalid){
    auto& pencil=pencils[i];
    const auto count=pencil.points.size();
            if(count<=2)pencils.erase(pencils.begin()+std::ptrdiff_t(i));
            else if(invalid==0)pencil.points.erase(pencil.points.begin());
            else if(invalid==count-1)pencil.points.pop_back();
            else {
                std::optional<ScorePencilObject> tail;
                if(count-invalid-1>=2){tail=pencil;tail->points.assign(pencil.points.begin()+std::ptrdiff_t(invalid+1),pencil.points.end());}
                // Native keeps the invalid point temporarily, restarts, then
                // trims that final point; a one-point head is dropped.
                if(invalid>=2)pencil.points.resize(invalid+1);else pencils.erase(pencils.begin()+std::ptrdiff_t(i));
                if(tail)pencils.push_back(std::move(*tail));
            }
}
}
void ScoreDocument::publish_pencils(ObjectState objects,const std::string& label){
    auto candidate=data_;
    mirror_objects(candidate,objects);validate_score(candidate);
    Compound history;history.reverse=true;
    history.children.emplace_back(PencilChange{true,typed_objects().pencils,std::nullopt});
    undo_.push_back({std::move(history),label,revision_});
    data_=std::move(candidate);object_cache_=std::move(objects);
    redo_.clear();undo_next_=true;revision_=next_++;++generation_;
}
bool ScoreDocument::add_pencil(ScorePencilObject stroke){
    BusyGuard guard(busy_);
    if(stroke.points.size()>score_objects_max_points)throw std::length_error("Score pen sample limit");
    if(stroke.points.empty())return false;
    std::size_t kept=1;
    for(std::size_t i=1;i<stroke.points.size();++i){
        const auto a=stroke.points[kept-1],b=stroke.points[i];
        if(std::hypot(double(a.x)-b.x,double(a.y)-b.y)>=2)stroke.points[kept++]=b;
    }
    if(kept<2)return false;
    stroke.points.resize(kept);
    auto objects=object_cache_?*object_cache_:project_objects(data_);
    objects.objects.pencils.push_back(std::move(stroke));
    publish_pencils(std::move(objects),"Score pen");return true;
}
bool ScoreDocument::erase_pencil(std::uint16_t mode,std::span<const ScorePencilPoint> centers,int width,int height){
    BusyGuard guard(busy_);
    if(centers.empty()||width<=0||height<=0)return false;
    if(centers.size()>score_objects_max_points)throw std::length_error("Score eraser sample limit");
    auto objects=object_cache_?*object_cache_:project_objects(data_);
    auto& pencils=objects.objects.pencils;
    bool changed{};std::size_t work{};
    const auto visit=[&](std::size_t count){
        if(count>score_pencil_cleanup_max_work-work)throw std::length_error("Score eraser work limit");
        work+=count;
    };
    for(const auto point:centers){
        const auto left=std::int64_t(point.x)-width/2,top=std::int64_t(point.y)-height/2;
        for(std::size_t i=0;i<pencils.size();){
            visit(1);const auto& stroke=pencils[i];
            if(stroke.mode!=mode){++i;continue;}
            const auto found=std::find_if(stroke.points.begin(),stroke.points.end(),[&](auto p){
                visit(1);return p.x>=left&&p.x<left+width&&p.y>=top&&p.y<top+height;
            });
            if(found==stroke.points.end()){++i;continue;}
            visit(stroke.points.size()+pencils.size());
            cut_pencil_point(pencils,i,std::size_t(found-stroke.points.begin()));
            i=0;changed=true;
        }
    }
    if(changed)publish_pencils(std::move(objects),"Score eraser");
    return changed;
}
void ScoreDocument::cleanup_pencils(ScoreObjects& objects,std::int32_t frames,std::int32_t layers) {
    std::size_t work{};auto visit=[&](std::size_t count){if(count>score_pencil_cleanup_max_work-work)throw std::length_error("Score Pencil cleanup exceeds work limit");work+=count;};
    for(;;) {
        bool changed{};
        for(std::size_t i=0;i<objects.pencils.size();++i) {
            visit(1);auto& pencil=objects.pencils[i];std::int32_t width{};switch(pencil.mode){case 1:case 2:width=32*layers;break;case 4:width=40;break;case 8:width=52;break;case 16:width=192;break;default:break;}
            std::size_t invalid{};while(invalid<pencil.points.size()){visit(1);const auto& point=pencil.points[invalid];if(point.x>=width||point.y>=14*frames)break;++invalid;}
            if(invalid==pencil.points.size())continue;
            visit(pencil.points.size()+objects.pencils.size());
            cut_pencil_point(objects.pencils,i,invalid);
            visit(objects.motion.size()+objects.pencils.size());for(const auto& value:objects.pencils)visit(value.points.size());for(const auto& value:objects.motion)if(const auto* text=std::get_if<ScoreTextAttributes>(&value.attributes))visit(text->text.size());
            validate_score_objects(objects,ScoreObjectsWriteMode::NativeCanonical);changed=true;break;
        }
        if(!changed)return;
    }
}
ScoreDocument::CompoundChild ScoreDocument::replay_child(ScoreData& data,std::optional<ObjectState>& objects,const CompoundChild& child) {
    if(const auto* info=std::get_if<Metadata>(&child)){const auto opposite=metadata(data.general);apply_metadata(data.general,*info);return opposite;}
    if(const auto* layer=std::get_if<LayerChange>(&child)) {
        if(layer->attach){if(!layer->pair)throw std::logic_error("Missing detached Score layer pair");attach_pair(data,layer->index,*layer->pair);return LayerChange{layer->index,false,std::nullopt};}
        return LayerChange{layer->index,true,detach_pair(data,layer->index)};
    }
    if(const auto* frames=std::get_if<FrameChange>(&child)){auto opposite=*frames;opposite.insert=!frames->insert;if(frames->insert)insert_frame_range(data,frames->index,frames->count,frames->original?&*frames->original:nullptr);else delete_frame_range(data,frames->index,frames->count);return opposite;}
    if(!objects)objects=project_objects(data);
    if(const auto* pencil=std::get_if<PencilChange>(&child)){auto opposite=*pencil;opposite.restore_before=!pencil->restore_before;if(pencil->restore_before){if(!opposite.after)opposite.after=objects->objects.pencils;objects->objects.pencils=pencil->before;}else{if(!pencil->after)throw std::logic_error("Missing Score Pencil poststate");objects->objects.pencils=*pencil->after;}return opposite;}
    throw std::logic_error("Score motion history requires ordered batch replay");
}
void ScoreDocument::replay(std::vector<Revision>& from,std::vector<Revision>& to) {
    BusyGuard guard(busy_);if(from.empty())return;
    auto candidate=data_;auto objects=object_cache_;Revision current{Snapshot{},from.back().label,revision_};const auto& payload=from.back().payload;
    if(const auto* snapshot=std::get_if<Snapshot>(&payload)) {candidate=snapshot->data;objects=snapshot->objects;candidate.general.visibility=data_.general.visibility;candidate.direction_mode=data_.direction_mode;candidate.sound_reference=data_.sound_reference;candidate.sound_suffix=data_.sound_suffix;current.payload=Snapshot{data_,object_cache_};}
    else if(const auto* direction=std::get_if<Bytes>(&payload)){candidate.direction=*direction;current.payload=data_.direction;}
    else if(const auto* cells=std::get_if<CellRange>(&payload)) {
        auto& layers=bank(candidate,cells->bank);
        CellRange opposite{cells->bank,cells->layer,cells->frame,{}};
        for(std::size_t i=0;i<cells->rows.size();++i){
            auto& layer=layers.at(cells->layer+i);
            const auto& row=cells->rows[i];
            range(layer,cells->frame,row.size());
            auto first=layer.cells.begin()+cells->frame;
            opposite.rows.emplace_back(first,first+row.size());
            std::copy(row.begin(),row.end(),first);
        }
        current.payload=std::move(opposite);
    }
    else if(const auto* saved_metadata=std::get_if<Metadata>(&payload)){current.payload=std::get<Metadata>(replay_child(candidate,objects,*saved_metadata));}
    else if(const auto* layer_change=std::get_if<LayerChange>(&payload)){current.payload=std::get<LayerChange>(replay_child(candidate,objects,*layer_change));}
    else if(const auto* frame_change=std::get_if<FrameChange>(&payload)){current.payload=std::get<FrameChange>(replay_child(candidate,objects,*frame_change));}
    else {
        const auto& compound=std::get<Compound>(payload);Compound opposite;opposite.reverse=!compound.reverse;opposite.children.resize(compound.children.size());
        // Stable private identities and an ordered list avoid quadratic vector
        // erasure/search when a large motion collection is removed or restored.
        struct MotionTable {using Rows=std::list<std::pair<std::uint64_t,ScoreMotionObject>>;Rows rows;std::unordered_map<std::uint64_t,Rows::iterator> lookup;};std::optional<MotionTable> motions;
        auto motion_child=[&](const CompoundChild& child)->CompoundChild {
            if(!objects)objects=project_objects(candidate);
            if(!motions){motions.emplace();if(objects->identities.size()!=objects->objects.motion.size())throw std::logic_error("Score motion identity count changed");motions->lookup.reserve(objects->identities.size());for(std::size_t i=0;i<objects->identities.size();++i){motions->rows.emplace_back(objects->identities[i],std::move(objects->objects.motion[i]));if(!motions->lookup.emplace(objects->identities[i],std::prev(motions->rows.end())).second)throw std::logic_error("Duplicate Score motion identity");}objects->objects.motion.clear();objects->identities.clear();}
            const auto identity=std::holds_alternative<MotionDelete>(child)?std::get<MotionDelete>(child).identity:std::get<MotionMove>(child).identity;const auto found=motions->lookup.find(identity);
            if(const auto* deletion=std::get_if<MotionDelete>(&child)) {
                if(deletion->restore){if(found!=motions->lookup.end()||!deletion->removed)throw std::logic_error("Invalid Score motion restoration");motions->rows.emplace_back(identity,*deletion->removed);motions->lookup.emplace(identity,std::prev(motions->rows.end()));return MotionDelete{identity,false,std::nullopt};}
                if(found==motions->lookup.end())throw std::logic_error("Missing Score motion identity");auto removed=std::move(found->second->second);motions->rows.erase(found->second);motions->lookup.erase(found);return MotionDelete{identity,true,std::move(removed)};
            }
            if(found==motions->lookup.end())throw std::logic_error("Missing Score motion identity");const auto& move=std::get<MotionMove>(child);auto result=move;result.restore_before=!move.restore_before;auto& current_motion=found->second->second;
            if(move.restore_before){if(!result.after)result.after=current_motion;current_motion=move.before;}else{if(!move.after)throw std::logic_error("Missing Score motion poststate");current_motion=*move.after;}return result;
        };
        auto flush_motions=[&]{if(motions){objects->objects.motion.reserve(motions->rows.size());objects->identities.reserve(motions->rows.size());for(auto& [identity,motion]:motions->rows){objects->identities.push_back(identity);objects->objects.motion.push_back(std::move(motion));}motions.reset();}};
        for(std::size_t step=0;step<compound.children.size();++step){const auto index=compound.reverse?compound.children.size()-1-step:step;const auto& child=compound.children[index];
            // Structural allocation budgets must see objects already replayed
            // by earlier children, including a restored Pencil prestate. Flush
            // motion batches once at this boundary, never per motion child.
            if(objects&&(std::holds_alternative<FrameChange>(child)||std::holds_alternative<LayerChange>(child))){flush_motions();mirror_objects(candidate,*objects);}
            opposite.children[index]=std::holds_alternative<MotionDelete>(child)||std::holds_alternative<MotionMove>(child)?motion_child(child):replay_child(candidate,objects,child);
        }
        flush_motions();
        if(objects)mirror_objects(candidate,*objects);
        current.payload=std::move(opposite);
    }
    validate_score(candidate);
    to.push_back(std::move(current));
    const auto revision=from.back().id;data_=std::move(candidate);object_cache_=std::move(objects);from.pop_back();revision_=revision;++generation_;
}
void ScoreDocument::undo(){const auto generation=generation_;replay(undo_,redo_);if(generation_!=generation)undo_next_=false;}
void ScoreDocument::redo(){const auto generation=generation_;replay(redo_,undo_);if(generation_!=generation)undo_next_=true;}
void ScoreDocument::undo_or_redo(){if(undo_next_)undo();else redo();}
std::optional<ScoreDocument::SoundFileIdentity> ScoreDocument::sound_identity(const std::filesystem::path& path){
    const auto token=file_token(path);if(token)return SoundFileIdentity{token->device,token->file,token->file_high};return std::nullopt;
}
void ScoreDocument::delete_owned_sound(const std::filesystem::path& path,SoundFileIdentity expected){
    remove_sound_file(path,{expected.device,expected.file,expected.file_high});
}
bool ScoreDocument::bind_sound(const std::filesystem::path& source,ScoreSoundBindMode mode,const std::function<bool()>& cancelled){
    BusyGuard guard(busy_);
    if(mode!=ScoreSoundBindMode::CopyIntoScoreFolder&&mode!=ScoreSoundBindMode::Reference)throw std::invalid_argument("Invalid Score sound binding mode");
    if(path_.empty())throw std::invalid_argument("Score sound binding requires a saved Score folder");
    const auto selected=std::filesystem::weakly_canonical(std::filesystem::absolute(source));
    if(sound_binding_&&selected==sound_binding_->path)return false;
    const auto selected_identity=sound_identity(selected);
    auto info=read_score_sound_metadata(selected);
    const auto folder=std::filesystem::weakly_canonical(path_.parent_path());
    const bool copy=mode==ScoreSoundBindMode::CopyIntoScoreFolder&&selected.parent_path()!=folder;
    auto destination=copy?folder/selected.filename():selected;
    auto candidate=data_;candidate.sound_reference=score_sound_relative_reference(path_,destination,sound_encoding_);candidate.general.visibility.at(std::size_t(ScoreLane::Sound))=1;validate_score(candidate);
    if(cancelled&&cancelled())throw ScoreSoundBindCancelled();
    if(generation_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Score generation exhausted");
    std::optional<SoundFileIdentity> identity;
    if(copy){
        const auto existing=sound_identity(destination);
        const bool replace_owned=existing&&sound_binding_&&sound_binding_->owned_copy&&destination==sound_binding_->path&&bound_sound_identity_&&*existing==*bound_sound_identity_;
        if(existing&&!replace_owned)throw std::runtime_error("Score sound copy destination already exists");
        static std::atomic<std::uint64_t> sequence{};
        const auto staged_path=folder/(".paintman-sound-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++)+selected.extension().string());
        SoundCopy staged(staged_path);stage_sound(selected,staged);info=read_score_sound_metadata(staged.path);
        ScoreSoundBinding binding{destination,info,true};identity=SoundFileIdentity{staged.token.device,staged.token.file,staged.token.file_high};
        SoundCopy published(destination);published.token=staged.token;
        if(cancelled&&cancelled())throw ScoreSoundBindCancelled();
        if(replace_owned){
            if(sound_identity(destination)!=bound_sound_identity_)throw std::runtime_error("Owned Score sound destination changed");
            replace_sound_copy(staged.path,destination);staged.active=false;
        }else{
            // Same-folder exclusive link publication rejects a late collision.
            std::filesystem::create_hard_link(staged.path,destination);published.active=true;
            if(sound_binding_&&sound_binding_->owned_copy&&bound_sound_identity_&&sound_binding_->path!=destination)delete_owned_sound(sound_binding_->path,*bound_sound_identity_);
        }
        data_=std::move(candidate);sound_binding_=std::move(binding);bound_sound_identity_=identity;sound_dirty_=true;++generation_;published.active=false;return true;
    }
    identity=selected_identity;ScoreSoundBinding binding{destination,info,false};
    if(cancelled&&cancelled())throw ScoreSoundBindCancelled();
    if(sound_identity(destination)!=identity)throw std::runtime_error("Selected Score sound file changed during binding");
    if(sound_binding_&&sound_binding_->owned_copy&&bound_sound_identity_)delete_owned_sound(sound_binding_->path,*bound_sound_identity_);
    data_=std::move(candidate);sound_binding_=std::move(binding);bound_sound_identity_=identity;sound_dirty_=true;++generation_;return true;
}
bool ScoreDocument::remove_sound(bool delete_owned_copy){
    if(delete_owned_copy)return remove_sound(ScoreSoundRemoveMode::OwnedCopyOnly);
    BusyGuard guard(busy_);if(!sound_binding_&&data_.sound_reference.empty())return false;auto candidate=data_;candidate.sound_reference.clear();validate_score(candidate);
    if(generation_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Score generation exhausted");
    data_=std::move(candidate);sound_binding_.reset();bound_sound_identity_.reset();sound_dirty_=true;++generation_;return true;
}
bool ScoreDocument::remove_sound(ScoreSoundRemoveMode mode){
    BusyGuard guard(busy_);if(mode!=ScoreSoundRemoveMode::OwnedCopyOnly&&mode!=ScoreSoundRemoveMode::DeleteBoundFile)throw std::invalid_argument("Invalid Score sound removal mode");
    if(!sound_binding_&&data_.sound_reference.empty())return false;auto candidate=data_;candidate.sound_reference.clear();validate_score(candidate);
    if(generation_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Score generation exhausted");
    if(sound_binding_&&(mode==ScoreSoundRemoveMode::DeleteBoundFile||sound_binding_->owned_copy)){
        if(!bound_sound_identity_)throw std::runtime_error("Bound Score sound file has no verified identity");delete_owned_sound(sound_binding_->path,*bound_sound_identity_);
    }
    data_=std::move(candidate);sound_binding_.reset();bound_sound_identity_.reset();sound_dirty_=true;++generation_;return true;
}
void ScoreDocument::save(const std::filesystem::path& path,const std::function<bool()>& cancelled) {
    if(busy_)throw std::logic_error("Cannot save an active Score transaction");
    auto prepared_path=std::filesystem::absolute(path);
    if(!sound_binding_&&!data_.sound_reference.empty()&&(path_.empty()||
       std::filesystem::weakly_canonical(path_.parent_path())!=std::filesystem::weakly_canonical(prepared_path.parent_path())))
        throw std::invalid_argument("Cannot relocate an opaque Score sound reference to another folder");
    const auto generation=generation_;auto prepared=data_;
    if(sound_binding_)prepared.sound_reference=score_sound_relative_reference(prepared_path,sound_binding_->path,sound_encoding_);
    auto bytes=encode_score(prepared);
    if(cancelled&&cancelled())throw ScoreSaveCancelled();
    if(generation!=generation_)throw std::runtime_error("Score changed during save preparation");
    write_file_atomic(prepared_path,bytes);path_.swap(prepared_path);data_.sound_reference.swap(prepared.sound_reference);saved_=revision_;sound_dirty_=false;++generation_;
}
void ScoreDocument::save_interchange(const std::filesystem::path& path,const std::function<bool()>& cancelled){
    if(busy_)throw std::logic_error("Cannot save an active Score transaction");
    auto extension=path.extension().string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
    const auto generation=generation_;Bytes bytes;
    if(extension==".csv")bytes=encode_score_csv_native(data_);else if(extension==".tsf")bytes=encode_tsf(make_tsf_score(data_));else throw std::invalid_argument("Score interchange Save requires CSV or TSF");
    if(cancelled&&cancelled())throw ScoreSaveCancelled();if(generation!=generation_)throw std::runtime_error("Score changed during save preparation");
    if(generation_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Score generation exhausted");
    write_file_atomic(std::filesystem::absolute(path),bytes);saved_=revision_;sound_dirty_=false;++generation_;
}

}
