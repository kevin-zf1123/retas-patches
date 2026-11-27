#include "retas/management_file.hpp"
#include <algorithm>
#include <charconv>

namespace retas {
namespace {
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Management file operation cancelled");}
void bounded(std::size_t size){if(size>management_max_bytes)throw std::length_error("Management file exceeds64MiB");}
struct Search {std::optional<std::size_t> position;std::size_t records;};
Search find(std::span<const std::uint8_t> bytes,std::uint32_t tag,std::stop_token stop){
    bounded(bytes.size());std::size_t at{},records{};
    while(at<bytes.size()){
        poll(stop);if(records++==management_max_records)throw std::length_error("Management search exceeds100000 records");
        Reader r(bytes.subspan(at),Endian::Little,at);if(r.u32()==tag)return {at+4,records};
        const auto length=r.u32();
        // Native rtSeekFile uses a signed LONG. Reject backtracking and integer
        // wrap rather than reproducing its unbounded malformed-input loop.
        if(length>INT32_MAX)throw FormatError(at+4,"Negative management seek length");
        if(length>r.remaining())return {{},records};at+=8+length;
    }
    poll(stop);return {{},records};
}
std::optional<std::string> string(std::span<const std::uint8_t> bytes,std::uint32_t tag,std::stop_token stop){
    const auto at=find(bytes,tag,stop).position;if(!at)return {};
    Reader r(bytes.subspan(*at),Endian::Little,*at);const auto length=r.u32();
    if(length>255)throw std::length_error("Management string exceeds255 wire bytes");
    auto payload=r.take(length);const auto end=std::find(payload.begin(),payload.end(),0);
    return std::string(payload.begin(),end);
}
}
ManagementArchive decode_management_archive(std::span<const std::uint8_t> bytes,std::stop_token stop){
    bounded(bytes.size());poll(stop);Reader r(bytes,Endian::Little);ManagementArchive records;
    while(r.remaining()){
        poll(stop);if(records.size()==management_max_records)throw std::length_error("Management archive exceeds100000 records");
        const auto tag=r.u32(),length=r.u32();auto payload=r.bytes(length);records.push_back({tag,std::move(payload)});
    }
    return records;
}
Bytes encode_management_archive(std::span<const ManagementRecord> records,std::stop_token stop){
    poll(stop);if(records.size()>management_max_records)throw std::length_error("Management archive exceeds100000 records");
    std::size_t size{};for(const auto& record:records){poll(stop);if(record.payload.size()>management_max_bytes-8||size>management_max_bytes-8-record.payload.size())throw std::length_error("Management file exceeds64MiB");size+=8+record.payload.size();}
    Writer w(Endian::Little);w.data.reserve(size);for(const auto& record:records){poll(stop);w.u32(record.tag);w.u32(std::uint32_t(record.payload.size()));w.raw(record.payload);}return std::move(w.data);
}
Bytes make_management_file(){return {0,0,0,0,1,0,0,0,'0'};}
std::optional<std::string> management_string(std::span<const std::uint8_t> bytes,std::uint32_t tag,std::stop_token stop){return string(bytes,tag,stop);}
bool management_integer(std::span<const std::uint8_t> bytes,std::uint32_t tag,std::int32_t& current,std::stop_token stop){
    const auto value=string(bytes,tag,stop);if(!value)return false;
    if(value->size()>99)throw std::length_error("Management integer exceeds99 C-string bytes");
    std::string_view text=*value;const auto whitespace=[](char c){return c==' '||(c>='\t'&&c<='\r');};
    while(!text.empty()&&whitespace(text.front()))text.remove_prefix(1);
    bool negative{};if(!text.empty()&&(text.front()=='+'||text.front()=='-')){negative=text.front()=='-';text.remove_prefix(1);}
    if(text.empty()||text.front()<'0'||text.front()>'9')return true;
    const std::uint64_t limit=negative?2147483648ull:2147483647ull;std::uint64_t magnitude{};
    for(const char c:text){if(c<'0'||c>'9')break;const auto digit=std::uint64_t(c-'0');if(magnitude>(limit-digit)/10)throw std::out_of_range("Management decimal exceeds int32");magnitude=magnitude*10+digit;}
    current=negative?std::int32_t(-std::int64_t(magnitude)):std::int32_t(magnitude);return true;
}
void set_management_string(Bytes& bytes,std::uint32_t tag,std::string_view value,std::stop_token stop){
    poll(stop);value=value.substr(0,value.find('\0'));if(value.size()>255)throw std::length_error("Management string exceeds255 C-string bytes");
    const auto search=find(bytes,tag,stop);const auto found=search.position;const auto at=found.value_or(bytes.size());
    if(!found&&search.records==management_max_records)throw std::length_error("Management append exceeds100000 records");
    const auto added=(found?4:8)+value.size();if(added>management_max_bytes-at)throw std::length_error("Management file exceeds64MiB");
    Writer w(Endian::Little);if(!found)w.u32(tag);w.u32(std::uint32_t(value.size()));w.raw(ascii(value));
    poll(stop);if(at+w.data.size()>bytes.size())bytes.resize(at+w.data.size());
    std::copy(w.data.begin(),w.data.end(),bytes.begin()+std::ptrdiff_t(at));
}
void set_management_integer(Bytes& bytes,std::uint32_t tag,std::int32_t value,std::stop_token stop){
    char buffer[12];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value);
    set_management_string(bytes,tag,std::string_view(buffer,std::size_t(result.ptr-buffer)),stop);
}
}
