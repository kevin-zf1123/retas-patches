#include "retas/score_csv.hpp"
#include "score_native_text.hpp"
#include <algorithm>
#include <charconv>

namespace retas {
namespace {
using Text=std::span<const std::uint8_t>;
constexpr std::array<std::uint8_t,4> primary{0xd4,0xad,0xbb,0xad},derived{0xb6,0xaf,0xbb,0xad},voice{0xcc,0xa8,0xb4,0xca};
constexpr std::array<std::uint8_t,2> open_circle{0xa1,0xf0},filled_circle{0xa1,0xf1},cross{0xa1,0xc1};
struct Resources {Text primary,derived,voice,open,filled,cross;};
Text prefix(Text bytes){return bytes.first(std::size_t(std::find(bytes.begin(),bytes.end(),0)-bytes.begin()));}
Text resource(const Bytes& bytes){if(bytes.size()>65535)throw std::invalid_argument("Score CSV resource exceeds65535 bytes");return prefix(bytes);}
Text name(const Bytes& bytes,bool native){
    const auto text=prefix(bytes);
    if(!native&&std::find(text.begin(),text.end(),0x0d)!=text.end())throw std::invalid_argument("Score CSV consumed layer name requires unclosed native CR/DBCS projection");
    return text;
}
Text cell(const ScoreCell& value,bool primary_bank,const Resources& r){
    if(primary_bank){if(value.marker&2)return r.open;if(value.marker&4)return r.filled;}
    if(value.marker&8)return r.cross;
    return prefix(value.label);
}
struct Count {
    std::size_t size{};
    void add(std::size_t n){if(n>score_csv_max_output_bytes-size)throw std::length_error("Score CSV output exceeds128MiB");size+=n;}
    void raw(Text text){add(text.size());}
    void byte(std::uint8_t){add(1);}
};
struct Output {
    Bytes& bytes;
    void raw(Text text){bytes.insert(bytes.end(),text.begin(),text.end());}
    void byte(std::uint8_t value){bytes.push_back(value);}
};
template<class Sink>void quoted(Sink& out,Text text,bool double_quotes=false){
    out.byte('"');
    if(double_quotes){for(auto c:text){out.byte(c);if(c=='"')out.byte(c);}}
    else out.raw(text);
    out.byte('"');
}
template<class Sink>void line_end(Sink& out){out.byte('\r');out.byte('\n');}
template<class Sink>void emit(Sink& out,const ScoreData& score,const Resources& r,bool native){
    constexpr std::array<std::uint8_t,5> frame{'F','r','a','m','e'};
    const auto layers=std::size_t(score.general.layer_count),frames=std::size_t(score.general.frame_count);
    quoted(out,frame);out.byte(',');quoted(out,r.primary);out.byte(',');
    for(std::size_t i=1;i<layers;++i){quoted(out,{});out.byte(',');}
    quoted(out,r.voice);out.byte(',');quoted(out,r.derived);out.byte(',');
    // Native L1 retains the comma above. L>1 ends with a final empty field.
    for(std::size_t i=1;i<layers;++i){quoted(out,{});if(i+1<layers)out.byte(',');}
    line_end(out);
    quoted(out,{});out.byte(',');
    for(const auto& layer:score.primary_layers){quoted(out,name(layer.name,native));out.byte(',');}
    quoted(out,{});out.byte(',');
    for(std::size_t i=0;i<layers;++i){quoted(out,name(score.derived_layers[i].name,native));if(i+1<layers)out.byte(',');}
    line_end(out);
    for(std::size_t row=0;row<frames;++row){
        std::array<char,10> decimal{};const auto result=std::to_chars(decimal.data(),decimal.data()+decimal.size(),row+1);
        quoted(out,Text(reinterpret_cast<const std::uint8_t*>(decimal.data()),std::size_t(result.ptr-decimal.data())));out.byte(',');
        for(const auto& layer:score.primary_layers){quoted(out,cell(layer.cells[row],true,r));out.byte(',');}
        quoted(out,prefix(Text(score.voice[row]).first(6)),true);out.byte(',');
        for(std::size_t column=0;column<layers;++column){quoted(out,cell(score.derived_layers[column].cells[row],false,r));if(column+1<layers)out.byte(',');}
        line_end(out);
    }
}
Bytes encode(const ScoreData& score,const Resources& resources,bool native=false){
    // Public Score admission checks all wire geometry, opaque payload framing
    // and its128MiB source budget without serializing/copying the whole model.
    validate_score(score);
    Count count;emit(count,score,resources,native);
    Bytes result;result.reserve(count.size);
    Output output{result};emit(output,score,resources,native);return result;
}
}
ScoreCsvResources native_score_csv_resources(){return {{primary.begin(),primary.end()},{derived.begin(),derived.end()},{voice.begin(),voice.end()},{open_circle.begin(),open_circle.end()},{filled_circle.begin(),filled_circle.end()},{cross.begin(),cross.end()}};}
Bytes encode_score_csv(const ScoreData& score){return encode(score,{primary,derived,voice,open_circle,filled_circle,cross});}
Bytes encode_score_csv_native(const ScoreData& score){
    validate_score(score);auto live=score;for(auto* bank:{&live.primary_layers,&live.derived_layers})for(auto& row:*bank)row.name=score_native_detail::runtime_string(row.name);
    return encode(live,{primary,derived,voice,open_circle,filled_circle,cross},true);
}
Bytes encode_score_csv(const ScoreData& score,const ScoreCsvResources& r){return encode(score,{resource(r.primary_header),resource(r.derived_header),resource(r.voice_header),resource(r.open_circle),resource(r.filled_circle),resource(r.cross)});}
}
