#include "vector_boundary_intersections.hpp"
#include "vector_curve_query.hpp"
#include <algorithm>
#include <set>

namespace retas::vector_boundary_detail {
namespace {
using namespace vector_graph_detail;
using vector_query_detail::Hit;
// Pool objects occupy increasing offsets inside a4096-slot native page.
// Page backing addresses are absent from the archive. Never infer their order.
struct PointerLess {
    bool operator()(int a,int b) const {
        if(a<0||b<0)throw std::invalid_argument("Boundary tree has a nullable identity");
        if((a>>12)!=(b>>12))throw std::invalid_argument("Boundary cross-page pointer ordering is unsupported");
        return a<b;
    }
};
struct PairLess {
    bool operator()(std::pair<int,int> a,std::pair<int,int> b) const {
        const auto less=[](int first,int second){
            if(first<0||second<0)throw std::invalid_argument("Boundary pair has a nullable Region");
            //562DD0 allocates32 Regions/page, stride0x68.
            if((first>>5)!=(second>>5))throw std::invalid_argument("Boundary cross-page Region ordering is unsupported");
            return first<second;
        };
        return less(a.first,b.first)||(a.first==b.first&&less(a.second,b.second));
    }
};
struct Event {int model,sub;float t;int vertex,other;};
using Events=std::multimap<int,Event,PointerLess>;
using Vertices=std::set<int,PointerLess>;
using Pairs=std::map<std::pair<int,int>,Vertices,PairLess>;

VectorBounds saved(const VectorModelCurve& m){return {m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]};}
void point_domain(P p){
    if(std::abs(std::int64_t(p.x))>134217727||std::abs(std::int64_t(p.y))>134217727)
        throw std::invalid_argument("Boundary point exceeds bounded raw domain");
}
struct Kernel {
    Editor& e;
    const std::map<int,std::size_t>& line_planes;
    const std::map<int,std::size_t>& face_planes;
    Pairs pairs;
    Vertices visited;
    RepairState result;
    //59CBF0 frees the bitmap slot, but leaves its MC cell address readable.
    //5B9B70 has moved the SC/CP heads before that release:5A0D20's null-head
    //gate therefore preserves the old saved boxes, flags and owner pointers.
    //Only raw occurrence-array consumers read these cells. Live lookup wins
    //after5456D0 reuses the slot; unknown dead wire identities still reject.
    std::map<int,VectorModelCurve> retired_models;

    const VectorModelCurve& raw_model(int id){
        e.budget.charge(e.a.models.size());
        const auto live=std::ranges::find(e.a.models,id,&VectorPoolRecord<VectorModelCurve>::index);
        if(live!=e.a.models.end())return live->value;
        e.budget.charge(retired_models.size()+1);
        const auto old=retired_models.find(id);
        if(old==retired_models.end())throw std::invalid_argument("Boundary raw MC slot has no phase lifetime");
        return old->second;
    }
    Editor::Split split(int mc,int sc,float q,int vertex){
        //Snapshot only the retained fixed cell. Copying the old SC vector
        //would allocate uncharged transient storage for an already-null head.
        VectorModelCurve old;
        {const auto& cell=e.model(mc);old.flags=cell.flags;old.line=cell.line;
            old.target_kinds=cell.target_kinds;old.targets=cell.targets;old.scalars=cell.scalars;}
        old.crosspoints={-1,-1};
        const auto cut=e.split_at_vertex(mc,sc,q,vertex);
        if(cut.cut){
            e.budget.charge(retired_models.size()+32);
            constexpr auto cell_bytes=sizeof(std::pair<const int,VectorModelCurve>)+4*sizeof(void*);
            if(!retired_models.contains(mc))charge(e.storage,cell_bytes,512ull*1024*1024,"Boundary retired MC cell storage limit");
            retired_models[mc]=std::move(old);
        }
        return cut;
    }

    P point(int id){const auto v=e.vertex(id);const P p{v.x,v.y};point_domain(p);return p;}
    Curve curve(int id){
        const auto s=e.sub(id);const auto m=e.model(s.model);
        Curve c{point(s.vertex_a),{s.control_x,s.control_y},point(s.vertex_b),bool(m.flags&1)};
        if(!c.linear)point_domain(c.control);
        return c;
    }
    static VectorBounds box(const Curve& c){
        VectorBounds b{std::min(c.start.x,c.end.x),std::min(c.start.y,c.end.y),
            std::max(c.start.x,c.end.x)+1,std::max(c.start.y,c.end.y)+1};
        if(!c.linear){b.left=std::min(b.left,c.control.x);b.top=std::min(b.top,c.control.y);
            b.right=std::max(b.right,c.control.x+1);b.bottom=std::max(b.bottom,c.control.y+1);}
        return b;
    }
    static bool equal_geometry(const Curve& a,const Curve& b){
        //5B8750 tests both EDX (first) endpoints against ECX (second).
        return a.control==b.control&&(a.start==b.start||a.start==b.end)&&(a.end==b.start||a.end==b.end);
    }
    static bool endpoint(const VectorSubcurve& s,int v){return s.vertex_a==v||s.vertex_b==v;}
    void append(Events& events,Event event){
        e.budget.charge(128);
        charge(e.storage,64,512ull*1024*1024,"Boundary event storage limit");
        if(events.size()>=8192)throw std::length_error("Boundary event occurrence limit");
        events.emplace(event.model,event);
    }
    void append_vertex(std::vector<int>& vertices,int v){
        e.budget.charge();charge(e.storage,4,512ull*1024*1024,"Boundary vertex work-list storage limit");
        if(vertices.size()>=8192)throw std::length_error("Boundary vertex occurrence limit");
        vertices.push_back(v);
    }
    void insert_vertex(Vertices& vertices,int v){
        if(vertices.contains(v))return;
        charge(e.storage,64,512ull*1024*1024,"Boundary Vertex tree storage limit");
        if(vertices.size()>=8192)throw std::length_error("Boundary Vertex tree limit");
        vertices.insert(v);
    }
    int owner(const VectorModelCurve& m,int lane){
        const auto id=m.targets[lane];if(id<0)throw std::invalid_argument("Boundary event has a missing side Loop");
        return m.target_kinds[lane]?e.value(e.paint->regions,id).outer.region:e.value(e.paint->loops,id).region;
    }
    int bridge(int first,int second){
        const auto target=point(second);
        for(int v:{first,second}){
            const auto wanted=v==first?target:point(first);
            for(int id:e.vertex(v).subcurves){e.budget.charge();const auto s=e.sub(id);
                if(!endpoint(s,v))throw std::invalid_argument("Boundary adjacency is nonincident");
                if(point(s.vertex_a==v?s.vertex_b:s.vertex_a)==wanted)return id;
            }
        }
        return -1;
    }
    static void remap_vertices(Events& events,int from,int to){
        for(auto& [key,event]:events)if(event.vertex==from)event.vertex=to;
    }
    int resolve(Hit& h,int a_id,int b_id,Events& a,Events* b,bool& changed){
        //5B9880 chooses one endpoint per curve using its stored parameter.
        const auto sa=e.sub(a_id),sb=e.sub(b_id);
        const int ca=h.target_parameter<.5f?sa.vertex_a:sa.vertex_b;
        const int cb=h.probe_parameter<.5f?sb.vertex_a:sb.vertex_b;
        const int da=sq(h.point,point(ca)),db=sq(h.point,point(cb));
        if(da>16&&(h.target_parameter<=0||h.target_parameter>=1))return -1;
        if(db>16&&(h.probe_parameter<=0||h.probe_parameter>=1))return -1;
        int chosen=da<=16?ca:-1;
        bool return_immediately{};
        if(db<=16){
            if(chosen<0)chosen=cb;
            else if(chosen!=cb){
                const int link=bridge(cb,chosen);
                if(link>=0){
                    if(link==a_id||link==b_id)return_immediately=true;
                    else{if(da<=0||db<=0)return -1;chosen=da<=db?chosen:cb;return_immediately=true;}
                }else{
                    const int dropped=chosen;
                    weld(e,cb,dropped);chosen=cb;changed=true;
                    remap_vertices(a,dropped,chosen);if(b&&b!=&a)remap_vertices(*b,dropped,chosen);
                }
            }
        }
        if(chosen>=0){h.point=point(chosen);if(return_immediately)return chosen;}
        if(chosen>=0)return chosen;
        for(const auto& [key,event]:a){e.budget.charge();if(sq(h.point,point(event.vertex))<=16)return event.vertex;}
        point_domain(h.point);
        chosen=e.create(e.a.vertices,0,VectorVertex{h.point.x,h.point.y,{}});
        e.owner.vertices.push_back(chosen);e.reference();return chosen;
    }
    std::vector<Hit> intersections(const Curve& a,const Curve& b,bool equal_endpoints,bool scaled=true){
        std::vector<Hit> out;
        if(equal_endpoints){
            const bool same=b.start==a.start;
            out.push_back({a.start,0,same?0.f:1.f});
            out.push_back({a.end,1,same?1.f:0.f});return out;
        }
        out=scaled?vector_query_detail::cross_mode1(a,b,e.budget):vector_query_detail::cross_mode0(a,b,e.budget);
        //5B8AF0: append original endpoint coordinates; a coincident nearest
        //projection suppresses the farther endpoint, with END winning ties.
        const auto augment=[&](const Curve& own,const Curve& peer,bool swap){
            struct Candidate {bool valid{},append{};int distance{};vector_query_detail::Nearest nearest;};
            std::array<Candidate,2> candidates;
            const std::array<P,2> ends{own.start,own.end};
            for(unsigned i=0;i<2;++i){auto& c=candidates[i];const auto p=ends[i];
                if(p==peer.start||p==peer.end){c.valid=true;c.nearest.point=p;continue;}
                c.nearest=vector_query_detail::nearest(peer,p,e.budget);c.distance=sq(p,c.nearest.point);
                c.valid=c.append=c.distance<=4;
            }
            if(candidates[0].valid&&candidates[1].valid&&candidates[0].nearest.point==candidates[1].nearest.point){
                if(candidates[0].distance<candidates[1].distance)candidates[1].append=false;
                else candidates[0].append=false;
            }
            for(unsigned i=0;i<2;++i)if(candidates[i].append){
                if(out.size()>=8192)throw std::length_error("Boundary CROSS hit limit");
                const auto q=candidates[i].nearest.parameter;
                out.push_back({ends[i],swap?q:float(i),swap?float(i):q});
            }
        };
        augment(a,b,false);augment(b,a,true);return out;
    }
    bool initial_pair(int a_id,int b_id,Events& a,Events& b){
        bool generated{};
        const auto as=e.model(a_id).subcurves,bs=e.model(b_id).subcurves;
        std::vector<VectorBounds> peer_bounds;for(int sc:bs)peer_bounds.push_back(box(curve(sc)));
        for(std::size_t i=0;i<as.size();++i){
            const int sa=as[i];const auto ca=curve(sa);const auto bounds=box(ca);
            for(std::size_t j=a_id==b_id?i+1:0;j<bs.size();++j){
                e.budget.charge(128);const int sb=bs[j];
                if(!overlap(bounds,peer_bounds[j]))continue;
                const auto cb=curve(sb);
                // Only physically adjacent spans sharing the endpoint skip.
                const auto aa=e.sub(sa),bb=e.sub(sb);
                if(a_id==b_id&&((i+1==j&&aa.vertex_b==bb.vertex_a)||(j+1==i&&bb.vertex_b==aa.vertex_a)))continue;
                const bool equal=bounds==peer_bounds[j]&&equal_geometry(curve(sa),cb);
                for(auto h:intersections(equal?curve(sa):ca,cb,equal)){
                    bool changed{};const int v=resolve(h,sa,sb,a,&b,changed);if(v<0)continue;
                    append(a,{a_id,sa,h.target_parameter,v,b_id});append(b,{b_id,sb,h.probe_parameter,v,a_id});generated=true;
                }
            }
        }
        return generated;
    }
    bool discover(const std::vector<int>& copied,const std::vector<int>& affected,Events& a,Events& b){
        bool generated{};
        for(int first:copied){
            //5BBCF0 snapshots this source MC saved box once. A weld during
            //one peer comparison must not recruit peers against its new box.
            const auto source_bounds=saved(e.model(first));
            for(int second:affected){e.budget.charge(32);
                if(overlap(source_bounds,saved(e.model(second))))generated|=initial_pair(first,second,a,b);}
        }
        return generated;
    }
    void register_pair(int vertex,int a,int b){
        const auto first=e.model(a),second=e.model(b);
        for(int x=0;x<2;++x)for(int y=0;y<2;++y){
            const int r0=owner(first,x),r1=owner(second,y);
            e.budget.charge(32);const auto key=std::pair{r0,r1};
            if(!pairs.contains(key))charge(e.storage,128,512ull*1024*1024,"Boundary Region pair tree storage limit");
            insert_vertex(pairs[key],vertex);
        }
    }
    void split_events(int id,Events& events,std::vector<int>& output){
        const auto original=raw_model(id);
        //5BBE7D/5BBEA0 accept null heads.5BC406 still appends var6C to
        //the residual array, including a released raw slot occurrence.
        if(original.subcurves.empty()){append_vertex(output,id);return;}
        const int start=e.sub(original.subcurves.front()).vertex_a,end=e.sub(original.subcurves.back()).vertex_b;
        std::vector<Event> ordered;
        const auto range=events.equal_range(id);
        for(int sc:original.subcurves){
            std::map<float,Event> by_parameter;
            for(auto it=range.first;it!=range.second;++it){auto& event=it->second;e.budget.charge();if(event.sub!=sc)continue;
                const auto s=e.sub(sc);if(event.vertex==s.vertex_a)event.t=0;else if(event.vertex==s.vertex_b)event.t=1;
                if(!std::isfinite(event.t)||event.t<0||event.t>1)throw std::invalid_argument("Boundary split parameter is invalid");
                by_parameter[event.t]=event;
            }
            for(const auto& [key,event]:by_parameter){charge(e.storage,sizeof(Event),512ull*1024*1024,"Boundary ordered split event storage limit");ordered.push_back(event);}
        }
        int current=id,previous_sub=-1;float previous_t{};bool at_end{};
        for(std::size_t i=0;i<ordered.size();++i){
            e.budget.charge(128);const auto event=ordered[i];int sc;float q;
            if(event.sub==previous_sub){if(at_end)continue;sc=e.model(current).subcurves.front();q=f32(binary64(binary64(double(event.t)-previous_t)/binary64(1-double(previous_t))));}
            else{sc=event.sub;q=event.t;at_end=false;}
            const auto m=e.model(current);
            if(i==0&&event.vertex==start&&sc==m.subcurves.front())continue;
            if(i+1==ordered.size()&&event.vertex==end&&sc==m.subcurves.back())continue;
            if(point(event.vertex)==point(e.sub(sc).vertex_b))at_end=true;
            const auto cut=split(current,sc,q,event.vertex);if(!cut.cut)continue;
            current=cut.right;
            const auto left=e.model(cut.left);
            if(e.sub(left.subcurves.front()).vertex_a==e.sub(left.subcurves.back()).vertex_b){
                const int tail=left.subcurves.back();const auto extra=split(cut.left,tail,0,e.sub(tail).vertex_a);
                if(extra.cut){append_vertex(output,extra.left);append_vertex(output,extra.right);}
            }else append_vertex(output,cut.left);
            previous_sub=event.sub;previous_t=event.t;
        }
        append_vertex(output,current);
    }
    bool suppress(const Events& events,int sc,int vertex){
        const auto s=e.sub(sc);if(endpoint(s,vertex))return false;
        const auto range=events.equal_range(s.model);
        for(auto it=range.first;it!=range.second;++it){e.budget.charge();if(it->second.sub==sc&&sq(point(vertex),point(it->second.vertex))<=16)return true;}
        return false;
    }
    bool expand(const std::vector<int>& vertices,const std::vector<int>& models,Events& events){
        bool generated{};
        for(int v:vertices){
            struct Star {int sub;VectorBounds bounds;};
            std::vector<Star> star;VectorBounds fan{};
            for(int sc:e.vertex(v).subcurves){const auto b=box(curve(sc));charge(e.storage,sizeof(Star),512ull*1024*1024,"Boundary fan snapshot storage limit");star.push_back({sc,b});unite(fan,b);}
            for(int mc:models){
                const auto& raw=raw_model(mc);
                if(!overlap(saved(raw),fan))continue;
                const auto subs=raw.subcurves;
                for(int sc:subs){
                    const auto c=curve(sc);const auto b=box(c);if(!overlap(b,fan))continue;
                    for(const auto& ray:star){
                        e.budget.charge(128);if(sc==ray.sub||!overlap(b,ray.bounds))continue;
                        const auto live=curve(sc),peer=curve(ray.sub);
                        if(b==ray.bounds&&equal_geometry(live,peer))continue;
                        for(auto h:intersections(live,peer,false)){
                            bool changed{};const int at=resolve(h,sc,ray.sub,events,nullptr,changed);if(at<0)continue;
                            const auto current=e.sub(sc),other=e.sub(ray.sub);
                            if(endpoint(current,at)&&endpoint(other,at)&&!changed)continue;
                            if(suppress(events,sc,at)||suppress(events,ray.sub,at))continue;
                            append(events,{current.model,sc,h.target_parameter,at,other.model});
                            append(events,{other.model,ray.sub,h.probe_parameter,at,current.model});generated=true;
                        }
                    }
                }
            }
        }
        return generated;
    }
    double peak(const VectorModelCurve& m){
        if(m.line<0)return 0;
        const float base=std::bit_cast<float>(e.line(m.line).half_width_bits);
        if(!std::isfinite(base)||base<0)throw std::invalid_argument("Boundary ranking base width is invalid");
        if(m.flags&2)return base;
        float maximum{};for(int sc:m.subcurves)for(int node:e.sub(sc).scales){const float w=std::bit_cast<float>(e.value(e.a.scales,node).scale_bits);
            if(!std::isfinite(w)||w<0)throw std::invalid_argument("Boundary ranking width is invalid");if(w>maximum)maximum=w;}
        return binary64(double(base)*maximum);
    }
    bool rank(int mc,int& index,float& width){
        const auto m=e.model(mc);bool improved{};
        if(m.line>=0){
            const auto it=line_planes.find(m.line);const int plane=it==line_planes.end()?-1:int(it->second);
            if(plane>index){index=plane;width=f32(peak(m));improved=true;}
            //5B88FD compares the x87 result against savedF32 before FSTP.
            else if(plane==index){const auto w=peak(m);if(w>double(width)){width=f32(w);improved=true;}}
        }
        for(int lane=0;lane<2;++lane){if(m.targets[lane]<0)continue;
            for(int face:e.value(e.paint->regions,owner(m,lane)).faces){e.budget.charge();const auto it=face_planes.find(face);const int plane=it==face_planes.end()?-1:int(it->second);
                if(plane>index){index=plane;width=0;improved=true;}}
        }
        return improved;
    }
    void redirects(){
        for(int v:result.global_vertices){
            std::multimap<int,int,PointerLess> grouped;
            for(int sc:e.vertex(v).subcurves){const auto s=e.sub(sc);charge(e.storage,64,512ull*1024*1024,"Boundary opposite Vertex tree storage limit");grouped.emplace(s.vertex_a==v?s.vertex_b:s.vertex_a,sc);}
            for(auto it=grouped.begin();it!=grouped.end();){
                const auto range=grouped.equal_range(it->first);const auto second=std::next(range.first);it=range.second;
                if(second==range.second||(e.model(e.sub(second->second).model).flags&0x03000000))continue;
                int index{},winner=-1;float width{};
                for(auto candidate=range.first;candidate!=range.second;++candidate){const int mc=e.sub(candidate->second).model;if(rank(mc,index,width))winner=mc;}
                if(winner<0)winner=e.sub(range.first->second).model;
                for(auto candidate=range.first;candidate!=range.second;++candidate){const int mc=e.sub(candidate->second).model;
                    e.model(mc).flags|=mc==winner?0x01000000u:0x02000000u;e.topology_changed=true;
                    if(mc!=winner){if(!result.redirected_models.contains(mc))charge(e.storage,64,512ull*1024*1024,"Boundary redirect tree storage limit");result.redirected_models[mc]=winner;}
                }
            }
        }
    }
    void repair(){
        for(int v:result.global_vertices){
            std::set<std::size_t> owners;
            for(int sc:e.vertex(v).subcurves){const int lc=e.model(e.sub(sc).model).line;if(lc>=0){const auto it=line_planes.find(lc);
                if(it==line_planes.end())throw std::invalid_argument("Boundary Line has no owner plane");owners.insert(it->second);}}
            if(owners.size()>1)throw std::invalid_argument("Boundary multi-plane pointer repair order is unsupported");
            for(auto owner:owners)e.repair_vertex(v,owner,line_planes);
        }
        e.slots[4].reset();
    }
    RepairState run(const std::vector<int>& copied,const std::vector<int>& affected,std::vector<int>& regions){
        Events a,b;
        result.events_found=discover(copied,affected,a,b);
        if(!result.events_found)return result;
        for(const auto& [key,event]:a){register_pair(event.vertex,event.model,event.other);append_vertex(result.global_vertices,event.vertex);}
        std::vector<int> residual_a,residual_b;
        for(int mc:copied)split_events(mc,a,residual_a);
        for(int mc:affected)split_events(mc,b,residual_b);
        Events work;
        const bool ga=expand(result.global_vertices,residual_a,work),gb=expand(result.global_vertices,residual_b,work);
        if(ga||gb)for(;;){
            e.budget.charge(128);
            for(const auto& [key,event]:work){
                if(std::ranges::find(residual_a,event.model)!=residual_a.end()||std::ranges::find(residual_b,event.model)!=residual_b.end())continue;
                append_vertex(residual_b,event.model);
                const auto m=e.model(event.model);const int side0=owner(m,0),side1=owner(m,1);
                if(std::ranges::find(regions,side0)==regions.end())append_vertex(regions,side0);
                //5D2F59 deliberately appends SIDE0 when the SIDE1 search misses.
                if(std::ranges::find(regions,side1)==regions.end())append_vertex(regions,side0);
            }
            std::vector<int> next_a,next_b;
            for(int mc:residual_a)split_events(mc,work,next_a);residual_a.clear();
            for(int mc:residual_b)split_events(mc,work,next_b);residual_b.clear();
            Vertices current;
            for(const auto& [key,event]:work){append_vertex(result.global_vertices,event.vertex);insert_vertex(visited,event.vertex);insert_vertex(current,event.vertex);}
            work.clear();const std::vector<int> fan(current.begin(),current.end());
            const bool na=expand(fan,next_a,work),nb=expand(fan,next_b,work);
            if(!na&&!nb)break;
            residual_a=std::move(next_a);residual_b=std::move(next_b);
        }
        redirects();repair();
        for(const auto& [key,vertices]:pairs){charge(e.storage,64+4*vertices.size(),512ull*1024*1024,"Boundary Region pair result storage limit");result.region_pairs.push_back({key.first,key.second,{vertices.begin(),vertices.end()}});}
        result.visited_vertices.assign(visited.begin(),visited.end());
        //std::map<int,int> exposes only the wire identities. Confirm every
        //comparison its downstream native pointer tree will require.
        if(!result.redirected_models.empty()){PointerLess less;const int first=result.redirected_models.begin()->first;for(const auto& [id,to]:result.redirected_models)(void)less(first,id);}
        e.budget.charge();return std::move(result);
    }
    RepairState pass(const std::vector<int>& source,const std::vector<int>& peer,bool shared){
        Events a,b;auto& destination=shared?a:b;
        result.events_found=discover(source,peer,a,destination);
        if(!result.events_found)return result;
        std::vector<int> discarded;
        for(int mc:source)split_events(mc,a,discarded);
        if(!shared)for(int mc:peer)split_events(mc,b,discarded);
        //5D3A93/5D3F21 collects the source event tree after splitting; the
        //peer tree is destroyed without contributing another Vertex array.
        for(const auto& [key,event]:a)append_vertex(result.global_vertices,event.vertex);
        redirects();repair();
        if(!result.redirected_models.empty()){PointerLess less;const int first=result.redirected_models.begin()->first;for(const auto& [id,to]:result.redirected_models)(void)less(first,id);}
        e.budget.charge();return std::move(result);
    }
};
}
std::vector<ModelLocationPair> collect_model_locations(Editor& e,int first,int second){
    const std::map<int,std::size_t> empty;Kernel kernel{e,empty,empty,{},{},{},{}};
    const auto& source=e.model(first).subcurves;const auto& peer=e.model(second).subcurves;
    charge(e.storage,(source.size()+peer.size())*sizeof(int)+peer.size()*sizeof(VectorBounds),512ull*1024*1024,"Vector LOCATION snapshot storage limit");
    const auto a=source,b=peer;
    std::vector<VectorBounds> boxes;for(int sc:b)boxes.push_back(Kernel::box(kernel.curve(sc)));std::vector<ModelLocationPair> result;
    const auto append=[&](ModelLocationPair location){e.budget.charge(128);charge(e.storage,sizeof(ModelLocationPair),512ull*1024*1024,"Vector LOCATION output storage limit");if(result.size()>=8192)throw std::length_error("Vector LOCATION occurrence limit");result.push_back(location);};
    for(std::size_t i=0;i<a.size();++i){const auto curve=kernel.curve(a[i]);const auto bounds=Kernel::box(curve);for(std::size_t j=first==second?i+1:0;j<b.size();++j){e.budget.charge(128);if(!overlap(bounds,boxes[j]))continue;const auto sca=e.sub(a[i]),scb=e.sub(b[j]);
            if((i+1==j&&first==second&&sca.vertex_b==scb.vertex_a)||(j+1==i&&first==second&&scb.vertex_b==sca.vertex_a))continue;
            const auto peer=kernel.curve(b[j]);const bool equal=bounds==boxes[j]&&Kernel::equal_geometry(curve,peer);
            for(const auto hit:kernel.intersections(curve,peer,equal,false)){append({a[i],b[j],hit.target_parameter,hit.probe_parameter});if(first==second)append({b[j],a[i],hit.probe_parameter,hit.target_parameter});}
        }}return result;
}
RepairState run(Editor& e,const std::vector<int>& copied,const std::vector<int>& affected,
    std::vector<int>& regions,const std::map<int,std::size_t>& lines,const std::map<int,std::size_t>& faces){
    if(!e.paint||&e.paint->base!=&e.a)throw std::invalid_argument("Boundary intersections require a unified Paint archive");
    e.budget.charge();return Kernel{e,lines,faces,{},{},{},{}}.run(copied,affected,regions);
}
RepairState intersect_pass(Editor& e,const std::vector<int>& source,const std::vector<int>& peer,
    bool shared,const std::map<int,std::size_t>& lines,const std::map<int,std::size_t>& faces){
    e.budget.charge();return Kernel{e,lines,faces,{},{},{},{}}.pass(source,peer,shared);
}
}
