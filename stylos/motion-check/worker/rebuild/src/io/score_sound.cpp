#include "retas/score_sound.hpp"
#include "score_sound_math.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace retas {
namespace {
void require(bool yes,const char* why){if(!yes)throw FormatError(0,why);}
template<class Read> ScoreSoundMetadata inspect(std::uint64_t size,Read read,ScoreSoundFormat format){
    require(size<=score_sound_max_file_bytes,"Score sound exceeds modern file limit");
    require(format==ScoreSoundFormat::Wave||format==ScoreSoundFormat::Aiff,"Unsupported Score sound format");
    const auto endian=format==ScoreSoundFormat::Wave?Endian::Little:Endian::Big;
    auto prefix=read(0,12);Reader header(prefix);const auto magic=header.u32();Reader count{std::span(prefix).subspan(4,4),endian};
    require(size>=12&&std::uint64_t(count.u32())+8==size,"Incomplete or trailing sound container");
    require(magic==(format==ScoreSoundFormat::Wave?0x52494646u:0x464f524du)&&header.take(4).size()==4,"Invalid sound container");
    require(header.u32()==(format==ScoreSoundFormat::Wave?0x57415645u:0x41494646u),"Unsupported WAVE/AIFF container type");
    ScoreSoundMetadata result;result.format=format;bool common{},data{};std::uint32_t wave_rate{},average{},alignment{};std::uint64_t frames{};
    std::size_t chunks{};std::uint64_t offset=12;
    while(offset<size){
        require(++chunks<=score_sound_max_chunks,"Score sound exceeds modern chunk limit");require(size-offset>=8,"Truncated sound chunk header");
        const auto chunk=read(offset,8);Reader id(chunk);const auto type=id.u32();Reader length{std::span(chunk).subspan(4),endian};const auto n=std::uint64_t(length.u32());offset+=8;
        require(n<=size-offset,"Truncated sound chunk payload");const auto next=offset+n+(n&1);require(next<=size,"Missing sound chunk pad");
        if(format==ScoreSoundFormat::Wave&&type==0x666d7420u){
            require(!common&&n>=16,"Duplicate or short WAV fmt chunk");const auto payload=read(offset,16);Reader p(payload,Endian::Little);
            require(p.u16()==1,"Score sound WAV profile requires PCM1");result.channels=p.u16();wave_rate=p.u32();average=p.u32();alignment=p.u16();result.bits_per_sample=p.u16();result.sample_rate=wave_rate;common=true;
        }else if(format==ScoreSoundFormat::Wave&&type==0x64617461u){
            require(!data,"Duplicate WAV data chunk");result.data_offset=offset;result.data_bytes=n;data=true;
        }else if(format==ScoreSoundFormat::Aiff&&type==0x434f4d4du){
            require(!common&&n==18,"Duplicate or unsupported AIFF COMM chunk");const auto payload=read(offset,18);Reader p(payload);result.channels=p.u16();frames=p.u32();result.bits_per_sample=p.u16();const auto rate=p.take(10);std::copy(rate.begin(),rate.end(),result.aiff_rate.begin());
            Reader extended(rate);const auto exponent=extended.u16();const auto mantissa=extended.u64();require(!(exponent&0x8000)&&exponent&&exponent!=0x7fff&&(mantissa>>63),"Unsupported AIFF extended rate");
            result.sample_rate=std::ldexp(double(mantissa),int(exponent)-16383-63);common=true;
        }else if(format==ScoreSoundFormat::Aiff&&type==0x53534e44u){
            require(!data&&n>=8,"Duplicate or short AIFF SSND chunk");const auto payload=read(offset,8);Reader p(payload);const auto skip=std::uint64_t(p.u32());require(p.u32()==0,"Score sound AIFF profile requires blockSize0");require(skip<=n-8,"AIFF sound offset outside SSND");
            result.data_offset=offset+8+skip;result.data_bytes=n-8-skip;data=true;
        }
        offset=next;
    }
    require(common,"Missing sound format metadata");require(result.channels==1||result.channels==2,"Score sound profile requires mono/stereo");require(result.bits_per_sample==8||result.bits_per_sample==16,"Score sound profile requires8/16-bit samples");
    require(std::isfinite(result.sample_rate)&&result.sample_rate>0&&(format==ScoreSoundFormat::Wave?result.sample_rate<=65535:result.sample_rate<65536),"Score sound rate outside fixed-rate safe profile");
    const auto frame_bytes=std::uint64_t(result.channels)*(result.bits_per_sample/8);
    if(format==ScoreSoundFormat::Wave){require(data,"Missing WAV data chunk");require(alignment==frame_bytes&&std::uint64_t(average)==std::uint64_t(wave_rate)*frame_bytes,"Inconsistent WAV alignment/byte rate");require(result.data_bytes%frame_bytes==0,"Partial WAV sample frame");result.sample_frames=result.data_bytes/frame_bytes;}
    else {require(data||frames==0,"Missing AIFF sound data");require(result.data_bytes==frames*frame_bytes,"AIFF frame/data byte count mismatch");result.sample_frames=frames;}
    result.duration_seconds=double(result.sample_frames)/result.sample_rate;
    require(std::isfinite(result.duration_seconds),"Score sound duration exceeds finite metadata profile");return result;
}

}
ScoreSoundMetadata inspect_score_sound(std::span<const std::uint8_t> bytes,ScoreSoundFormat format){
    auto read=[&](std::uint64_t offset,std::size_t n){require(offset<=bytes.size()&&n<=bytes.size()-std::size_t(offset),"Truncated sound read");return Bytes(bytes.begin()+std::ptrdiff_t(offset),bytes.begin()+std::ptrdiff_t(offset+n));};
    return inspect(bytes.size(),read,format);
}
ScoreSoundMetadata read_score_sound_metadata(const std::filesystem::path& path){
    auto extension=path.extension().string();for(auto& c:extension)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
    require(extension==".wav"||extension==".aif"||extension==".aiff","Unsupported Score sound extension");
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open Score sound file");const auto end=file.tellg();require(end>=0&&std::uint64_t(end)<=score_sound_max_file_bytes,"Score sound exceeds modern file limit");const auto size=std::uint64_t(end);
    auto read=[&](std::uint64_t offset,std::size_t n){require(offset<=size&&n<=size-offset,"Truncated sound read");Bytes data(n);file.seekg(std::streamoff(offset));if(!file.read(reinterpret_cast<char*>(data.data()),std::streamsize(n)))throw std::runtime_error("Short Score sound file read");return data;};
    return inspect(size,read,extension==".wav"?ScoreSoundFormat::Wave:ScoreSoundFormat::Aiff);
}
std::uint32_t score_sound_fixed_rate(const ScoreSoundMetadata& metadata,std::optional<std::uint32_t> native_fixed){
    if(native_fixed){require(*native_fixed!=0,"Zero native fixed sample rate");return *native_fixed;}
    require(std::isfinite(metadata.sample_rate)&&metadata.sample_rate>0&&(metadata.format==ScoreSoundFormat::Wave?metadata.sample_rate<=65535:metadata.sample_rate<65536),"Invalid Score sound sample rate");
    const auto fixed=std::floor(metadata.sample_rate*65536.0);require(fixed>=1&&fixed<=UINT32_MAX,"Sample rate outside unsigned16.16");return std::uint32_t(fixed);
}
std::optional<ScoreSoundSample> sample_score_sound(const ScoreSoundBinding& binding,std::uint32_t ticks600,std::optional<std::uint32_t> native_fixed){
    const auto& m=binding.metadata;
    const auto frame_bytes=score_sound_detail::frame_bytes(m);
    const auto offset=score_sound_detail::byte_offset(ticks600,score_sound_fixed_rate(m,native_fixed),frame_bytes);
    if(offset>m.data_bytes||frame_bytes>m.data_bytes-offset||m.data_offset>score_sound_max_file_bytes||offset>score_sound_max_file_bytes-m.data_offset)return std::nullopt;
    const auto position=m.data_offset+offset;
    std::ifstream file(binding.path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open bound Score sound file");const auto end=file.tellg();
    if(end<0||std::uint64_t(end)>score_sound_max_file_bytes||position>std::uint64_t(end)||frame_bytes>std::uint64_t(end)-position)return std::nullopt;
    std::array<std::uint8_t,4> bytes{};file.seekg(std::streamoff(position));if(!file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(frame_bytes)))throw std::runtime_error("Short bound Score waveform read");
    auto value=[&](std::size_t channel){const auto at=channel*(m.bits_per_sample/8);if(m.bits_per_sample==8){const int raw=bytes[at];return std::uint8_t(m.format==ScoreSoundFormat::Wave?raw:128+(raw>127?raw-255:raw));}
        const auto raw=m.format==ScoreSoundFormat::Wave?std::uint16_t(bytes[at]|(std::uint16_t(bytes[at+1])<<8)):std::uint16_t((std::uint16_t(bytes[at])<<8)|bytes[at+1]);
        const int signed_sample=raw>=0x8000?int(raw)-65536:int(raw);return std::uint8_t(128+signed_sample/256);};
    return ScoreSoundSample{value(0),m.channels==2?value(1):std::uint8_t(128)};
}
std::optional<ScoreSoundSample> sample_score_sound(const std::filesystem::path& path,std::uint32_t ticks600,std::optional<std::uint32_t> native_fixed){return sample_score_sound(ScoreSoundBinding{path,read_score_sound_metadata(path),false},ticks600,native_fixed);}
}
