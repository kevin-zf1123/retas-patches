#pragma once
#include "retas/vector_face_archive.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <stop_token>
#include <stdexcept>

namespace retas::vector_material_detail {
inline constexpr std::uint64_t max_bytes=512ull*1024*1024,max_work=128ull*1024*1024;
inline void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Vector material edit cancelled");}
inline void charge(std::uint64_t& count,std::uint64_t value,std::uint64_t limit,const char* message){if(value>limit||count>limit-value)throw std::length_error(message);count+=value;}
// Native pool headers only preallocate pages. Live IDs reconstruct cursor/H;
// deleting H lowers it by one only inside the cursor-lowering branch.
struct VectorSlots {
    std::set<int> live;
    int cursor{},high{-1};
    int lowest(std::uint64_t& work,std::stop_token stop)const{
        for(int id=0;;++id){poll(stop);charge(work,32,max_work,"Vector material slot scan work exceeds128Mi units");if(!live.contains(id))return id;}
    }
    template<class T> VectorSlots(const std::vector<VectorPoolRecord<T>>& pool,std::uint64_t& work,std::stop_token stop,std::optional<int> previous){
        for(const auto& r:pool){poll(stop);live.insert(r.index);high=std::max(high,r.index);}if(previous){if(*previous<high||*previous<-1)throw std::invalid_argument("Invalid live Vector material pool H");high=*previous;}cursor=lowest(work,stop);
    }
    int allocate(std::uint64_t& work,std::stop_token stop){
        const int id=cursor;live.insert(id);
        if(std::int64_t(cursor)==std::int64_t(high)+1){high=cursor;++cursor;}else cursor=lowest(work,stop);
        return id;
    }
    void release(int id){if(!live.erase(id))return;if(cursor>id){cursor=id;if(high==id)--high;}}
};
struct PaintLists {
    std::array<std::vector<int>,3> ids;
    Bytes tail;
    explicit PaintLists(const Plane& p){Reader r(p.specific);for(auto& list:ids){const auto n=r.i32();r.require_count(n,4,8192);list.reserve(std::size_t(n));for(int i=0;i<n;++i)list.push_back(r.i32());}tail=r.rest();}
    void publish(Plane& p)const{Writer w;for(const auto& list:ids){w.i32(int(list.size()));for(auto id:list)w.i32(id);}w.raw(tail);p.specific=std::move(w.data);}
};
struct MaterialDetach {
    VectorPaintArchive& archive;PaintLists& lists;Layer& layer;
    std::uint64_t& nodes;std::uint64_t& references;std::uint64_t& retained;std::uint64_t& work;std::stop_token stop;
    std::array<std::optional<VectorSlots>,3> slots; // GradNode, Gradation, Texture
    std::set<std::pair<unsigned,int>> inspected;
    bool plane_changed{};
    void prefix(const Bytes& raw,unsigned kind,int id){
        // Check both lists before the typed codec reserves them. Texture image
        // bytes stay framed and opaque; replacement never samples/decompresses.
        poll(stop);charge(work,4*raw.size()+32,max_work,"Vector material detach work exceeds128Mi units");
        charge(retained,2*raw.size()+128,max_bytes,"Vector material detach storage exceeds512MiB");
        Reader r(raw);if(kind==4){const auto image=r.u32();r.take(image);(void)r.string();}r.u32();
        const auto list=[&]{const auto n=r.i32();r.require_count(n,4,8192);r.take(std::size_t(n)*4);return std::uint64_t(n);};
        const auto count=kind==2?list()+list():list();
        if(inspected.emplace(kind,id).second)charge(references,count,8192,"Vector material material references exceed8192");
    }
    template<class T> void erase(std::vector<VectorPoolRecord<T>>& pool,int id,unsigned slot){
        if(!slots[slot])slots[slot].emplace(pool,work,stop,layer.runtime_vector_high_water[9+slot]);
        const auto i=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);
        if(i==pool.end())throw std::invalid_argument("Vector material detach has a dead child record");
        slots[slot]->release(id);pool.erase(i);--nodes;
        charge(work,32*pool.size(),max_work,"Vector material material pool ordering exceeds128Mi units");
        std::ranges::sort(pool,{},&VectorPoolRecord<T>::index);archive.face_high_water[3+slot]=slots[slot]->high;layer.runtime_vector_high_water[9+slot]=slots[slot]->high;
    }
    void unlink_owner(int id,unsigned kind){
        auto& list=lists.ids[kind==2?1:2];const auto i=std::ranges::find(list,id);
        if(i==list.end())throw std::invalid_argument("Vector material material has no target-plane owner");
        list.erase(i);--references;plane_changed=true;
    }
    void detach(int face_id,VectorArchiveFace& face){
        const auto kind=face.flags&15;if((kind!=2&&kind!=4)||face.material<0)return;
        auto& pool=kind==2?archive.gradations:archive.textures;
        const auto it=std::ranges::find(pool,face.material,&VectorPoolRecord<Bytes>::index);
        if(it==pool.end())throw std::invalid_argument("Vector material material is missing");
        const int id=face.material;prefix(it->value,kind,id);
        auto gradient=kind==2?decode_vector_gradation(it->value):VectorArchiveGradation{};
        auto texture=kind==4?decode_vector_texture(it->value):VectorArchiveTexture{};
        auto& back=kind==2?gradient.faces:texture.faces;const auto backlink=std::ranges::find(back,face_id);
        //598E80/598ED0 remove only the first matching fresh pointer node.
        // An absent backlink still clears the forward pointer; null/duplicate
        // nodes count as remaining consumers for the native empty-list test.
        if(backlink!=back.end()){
            back.erase(backlink);--references;
            if(back.empty()){
                if(kind==2){
                    std::set<int> children;for(auto node:gradient.stops){poll(stop);if(node<0||!children.insert(node).second)throw std::invalid_argument("Vector material gradient deletion requires exclusive live stops");}
                    // Do not destroy a node still named by another gradient.
                    // Unknown inactive gradients stay opaque for color writes;
                    // a child-deleting operation must prove their ownership.
                    if(!children.empty())for(const auto& other:archive.gradations)if(other.index!=id){poll(stop);Reader r(other.value);r.u32();const auto n=r.i32();r.require_count(n,4,8192);charge(work,32*std::uint64_t(n),max_work,"Vector material gradient ownership work exceeds128Mi units");for(int j=0;j<n;++j)if(children.contains(r.i32()))throw std::invalid_argument("Vector material gradient stops have competing owners");}
                    unlink_owner(id,kind);for(auto node:gradient.stops){poll(stop);erase(archive.gradnodes,node,0);--references;}erase(archive.gradations,id,1);
                }else{unlink_owner(id,kind);erase(archive.textures,id,2);}
            }else it->value=kind==2?encode_vector_gradation(gradient):encode_vector_texture(texture);
        }
        face.material=-1;
    }
};
}
