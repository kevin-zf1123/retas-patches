#include "retas/score_sound_playback.hpp"
#include "../io/score_sound_math.hpp"
#include <algorithm>
#include <fstream>

namespace retas {
namespace {
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Score sound playback cancelled");}
void require(bool yes,const char* why){if(!yes)throw std::invalid_argument(why);}
struct Entry {bool& busy;explicit Entry(bool& b):busy(b){if(b)throw std::logic_error("Reentrant Score sound player");busy=true;}~Entry(){busy=false;}};
std::uint64_t alignment(const ScoreSoundPcm& p){
    require(p.fixed_rate&&(p.channels==1||p.channels==2)&&(p.bits_per_sample==8||p.bits_per_sample==16),"Invalid Score PCM format");
    const auto n=std::uint64_t(p.channels)*(p.bits_per_sample/8);
    require(p.bytes.size()<=score_sound_max_file_bytes&&p.bytes.size()%n==0,"Invalid Score PCM payload");return n;
}
}
ScoreSoundPlayRequest score_sound_play_request(const ScoreDocument& d,ScoreSoundSelection s,std::optional<std::uint32_t> fixed){
    const auto& g=d.data().general;require(g.fps!=0,"Score sound Play requires positive FPS");
    if(s.first>s.last)std::swap(s.first,s.last);
    require(s.first>=0&&s.last<g.frame_count,"Score sound selection outside frame domain");
    if(s.first==s.last)s.last=g.frame_count;
    const auto ticks=600u/g.fps;return {1,std::uint32_t(s.first)*ticks,std::uint32_t(s.last)*ticks,fixed};
}
ScoreSoundPcm decode_score_sound_pcm(const ScoreSoundBinding& binding,ScoreSoundPlayRequest request,std::stop_token stop){
    poll(stop);const auto& m=binding.metadata;const auto block=score_sound_detail::frame_bytes(m);
    require(request.mode<=1,"Score sound Play mode must be0 or1");
    require(m.data_bytes<=score_sound_max_file_bytes&&m.data_offset<=score_sound_max_file_bytes-m.data_bytes&&m.data_bytes%block==0,"Invalid cached Score PCM extent");
    ScoreSoundPcm result{score_sound_fixed_rate(m,request.native_fixed_rate),m.channels,m.bits_per_sample,{}};
    const auto begin=request.mode?std::min(m.data_bytes,score_sound_detail::byte_offset(request.start_ticks,result.fixed_rate,block)):0;
    const auto end=request.mode?std::min(m.data_bytes,score_sound_detail::byte_offset(request.end_ticks,result.fixed_rate,block)):m.data_bytes;
    if(begin>=end)return result;
    require(std::filesystem::is_regular_file(std::filesystem::symlink_status(binding.path)),"Score PCM requires a direct regular file");
    std::ifstream file(binding.path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open Score PCM source");
    const auto size=file.tellg();require(size>=0&&std::uint64_t(size)<=score_sound_max_file_bytes&&m.data_offset+end<=std::uint64_t(size),"Truncated or oversized Score PCM source");
    result.bytes.resize(std::size_t(end-begin));file.seekg(std::streamoff(m.data_offset+begin));
    for(std::size_t at=0;at<result.bytes.size();){poll(stop);const auto count=std::min<std::size_t>(65536,result.bytes.size()-at);
        if(!file.read(reinterpret_cast<char*>(result.bytes.data()+at),std::streamsize(count)))throw std::runtime_error("Short Score PCM read");at+=count;
    }
    if(m.format==ScoreSoundFormat::Aiff){
        if(m.bits_per_sample==8){for(std::size_t i=0;i<result.bytes.size();++i){if(!(i&65535))poll(stop);result.bytes[i]=std::uint8_t(result.bytes[i]+128);}}
        else for(std::size_t i=0;i<result.bytes.size();i+=2){if(!(i&65535))poll(stop);std::swap(result.bytes[i],result.bytes[i+1]);}
    }
    poll(stop);return result;
}
ScoreSoundPcm resample_score_sound_pcm(const ScoreSoundPcm& pcm,std::uint32_t rate,std::stop_token stop){
    poll(stop);const auto block=alignment(pcm);require(rate>=1&&rate<=65535,"Invalid Score PCM output rate");
    ScoreSoundPcm out{rate*65536,pcm.channels,pcm.bits_per_sample,{}};
    const auto frames=pcm.bytes.size()/block;
    const auto count=std::uint64_t(frames)*out.fixed_rate/pcm.fixed_rate;
    if(count>score_sound_max_file_bytes/block)throw std::length_error("Resampled Score PCM exceeds512MiB");
    out.bytes.resize(std::size_t(count*block));
    for(std::uint64_t i=0;i<count;++i){if(!(i&16383))poll(stop);const auto source=std::min<std::uint64_t>(frames-1,i*pcm.fixed_rate/out.fixed_rate);
        std::copy_n(pcm.bytes.begin()+std::ptrdiff_t(source*block),std::size_t(block),out.bytes.begin()+std::ptrdiff_t(i*block));
    }
    return out;
}
Bytes encode_score_sound_wave(const ScoreSoundPcm& pcm){
    const auto block=alignment(pcm);require((pcm.fixed_rate&65535)==0,"WAV output needs an integer rate; resample first");
    if(pcm.bytes.size()>score_sound_max_file_bytes-44-(pcm.bytes.size()&1))throw std::length_error("Score WAV container exceeds512MiB");
    const auto rate=pcm.fixed_rate/65536;Writer w(Endian::Little);w.raw(ascii("RIFF"));w.u32(std::uint32_t(36+pcm.bytes.size()+(pcm.bytes.size()&1)));w.raw(ascii("WAVEfmt "));w.u32(16);w.u16(1);w.u16(pcm.channels);w.u32(rate);w.u32(std::uint32_t(rate*block));w.u16(std::uint16_t(block));w.u16(pcm.bits_per_sample);w.raw(ascii("data"));w.u32(std::uint32_t(pcm.bytes.size()));w.raw(pcm.bytes);if(pcm.bytes.size()&1)w.u8(0);return std::move(w.data);
}
ScoreSoundPlayer::ScoreSoundPlayer(ScoreSoundOutput output):output_(std::move(output)){
    require(bool(output_.start)&&bool(output_.is_playing)&&bool(output_.stop),"Incomplete Score sound output");
}
ScoreSoundPlayer::~ScoreSoundPlayer(){try{stop();}catch(...) {}}
ScoreSoundPlayer& global_score_sound_player(){static ScoreSoundPlayer player(make_score_sound_device_output());return player;}
bool ScoreSoundPlayer::is_playing(){Entry guard(busy_);try{const bool playing=output_.is_playing();if(!playing)clip_.reset();return playing;}catch(...){try{output_.stop();}catch(...){}clip_.reset();throw;}}
bool ScoreSoundPlayer::stop(){Entry guard(busy_);try{const bool playing=output_.is_playing();if(playing)output_.stop();clip_.reset();return playing;}catch(...){try{output_.stop();}catch(...){}clip_.reset();throw;}}
bool ScoreSoundPlayer::play(const ScoreSoundBinding& binding,ScoreSoundPlayRequest request,std::stop_token stop){
    Entry guard(busy_);
    try{
        // Output callbacks may clear/rebind the caller's Score without
        // reentering this player. Own input before even is_playing/stop.
        const auto source=binding;
        if(output_.is_playing())output_.stop();clip_.reset();
        auto pcm=std::make_shared<const ScoreSoundPcm>(decode_score_sound_pcm(source,request,stop));if(pcm->bytes.empty())return false;
        poll(stop);clip_=std::move(pcm);output_.start(clip_);return true;
    }catch(...){try{output_.stop();}catch(...){}clip_.reset();throw;}
}
bool query_score_sound_command(int command,const ScoreDocument& document,ScoreSoundPlayer& player,ScoreSoundSelection selection,bool available,int guard){
    if(guard>0)return false;
    if(command==41042)return available&&document.sound_binding().has_value()&&selection.mode!=0&&selection.mode!=16;
    if(command==41043)return available&&player.is_playing();return false;
}
bool execute_score_sound_command(int command,const ScoreDocument& document,ScoreSoundPlayer& player,ScoreSoundSelection selection,bool available,int guard,std::stop_token stop,std::optional<std::uint32_t> fixed){
    if(guard>0)return false;
    if(command==41042){if(query_score_sound_command(command,document,player,selection,available))player.play(*document.sound_binding(),score_sound_play_request(document,selection,fixed),stop);return true;}
    if(command==41043){if(player.is_playing())player.stop();return true;}return false;
}
}
