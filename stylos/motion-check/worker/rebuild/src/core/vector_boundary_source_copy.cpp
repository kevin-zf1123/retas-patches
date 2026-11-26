#include "vector_boundary_source_copy.hpp"
#include "vector_fill_boundary.hpp"
#include "retas/vector_color_metadata.hpp"
#include <set>
#include <zlib.h>

namespace retas::vector_boundary_detail {
namespace {
using vector_graph_detail::Editor;
using vector_probe_detail::Budget;
using vector_material_detail::PaintLists;
using vector_material_detail::charge;
bool paint_plane(const Plane& p){return !p.opaque&&(p.family==0x22100001||p.family==0x22100002);}
bool line_plane(const Plane& p){return !p.opaque&&(p.family==0x21110001||p.family==0x21120001);}
template<class T> const T& get(const std::vector<VectorPoolRecord<T>>& pool,int id,Budget b){
    b.charge(pool.size()+1);const auto i=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);
    if(i==pool.end())throw std::invalid_argument("Vector source copy has a dead source identity");return i->value;
}
struct GeometryCopier {
    Editor& e;const VectorLineArchive& graph;std::optional<vector_copy_detail::Matrix> matrix;LineSourceCopyResult result;
    GeometryCopier(Editor& edit,const VectorLineArchive& input,const std::array<std::int32_t,2>& shift,bool directed_leaf=false,std::optional<vector_copy_detail::Matrix> affine={}):e(edit),graph(input),matrix(vector_copy_detail::transform(shift,affine)){if(!directed_leaf && &graph==&e.a)throw std::invalid_argument("Vector source copy requires a distinct immutable graph");if(!e.owner.border_owner&&(!directed_leaf||!e.owner.vertex_plane))throw std::invalid_argument("Vector source copy requires a typed destination topology owner");}
    int component(int value,int shift){
        const auto n=std::int64_t(value)+shift;
        if(std::abs(std::int64_t(value))>134217727||std::abs(n)>134217727)throw std::invalid_argument("Vector source copy point exceeds bounded raw domain");
        return int(n);
    }
    std::array<int,2> point(int x,int y){
        component(x,0);component(y,0);if(!matrix)return {x,y};
        const auto p=vector_copy_detail::point(x,y,*matrix);return {vector_copy_detail::integer(p[0]),vector_copy_detail::integer(p[1])};
    }
    std::uint32_t width(std::uint32_t bits){return vector_copy_detail::width(bits,matrix);}
    int vertex(int source_id,bool mapped=true){
        if(mapped)if(const auto found=result.vertices.find(source_id);found!=result.vertices.end())return found->second;
        const auto& old=get(graph.vertices,source_id,e.budget);const auto p=point(old.x,old.y);
        const int id=e.create(e.a.vertices,0,VectorVertex{p[0],p[1],{}});e.owner.vertices.push_back(id);e.reference();result.vertices.emplace(source_id,id);result.vertex_clones.emplace(source_id,id);return id;
    }
    int crosspoint(int source_id,bool reversed=false){
        const auto& old=get(graph.crosspoints,source_id,e.budget);VectorCrosspoint fresh;
        fresh.words[0]=old.words[0];fresh.words[1]=old.words[1];
        const auto copy_point=[&](int at){const auto p=point(std::bit_cast<int>(old.words[at]),std::bit_cast<int>(old.words[at+1]));fresh.words[at]=std::bit_cast<std::uint32_t>(p[0]);fresh.words[at+1]=std::bit_cast<std::uint32_t>(p[1]);};
        copy_point(4);copy_point(12);
        //5D0400 transforms the middle point even when neither setter consumes
        // it.5977F0/597860 store it only for active kinds1/2/4.
        const auto middle=point(std::bit_cast<int>(old.words[8]),std::bit_cast<int>(old.words[9]));
        for(int lane=0;lane<2;++lane){const auto kind=old.words[lane];if(kind==1)fresh.words[2+lane]=reversed?std::bit_cast<std::uint32_t>(float(numeric_detail::binary64(1-double(std::bit_cast<float>(old.words[2+lane]))))):old.words[2+lane];if(kind==2)copy_point(lane?10:6);if(kind==1||kind==2||kind==4){fresh.words[8]=std::bit_cast<std::uint32_t>(middle[0]);fresh.words[9]=std::bit_cast<std::uint32_t>(middle[1]);}}
        const int id=e.create(e.a.crosspoints,4,fresh);result.crosspoints.emplace(source_id,id);return id;
    }
    int model(int source_id,bool directed=false,bool forward=true,int* carry=nullptr){
        if(!directed)if(const auto found=result.models.find(source_id);found!=result.models.end())return found->second;
        std::optional<VectorModelCurve> captured_model;const auto* old_pointer=&get(graph.models,source_id,e.budget);
        if(&graph==&e.a){captured_model=*old_pointer;old_pointer=&*captured_model;}const auto& old=*old_pointer;
        if(old.subcurves.empty())throw std::invalid_argument("Vector source copy requires nonempty MC chains");
        VectorModelCurve fresh;fresh.flags=directed&&!forward?(old.flags&~0x3cu)|((old.flags&0x14u)<<1)|((old.flags&0x28u)>>1):old.flags;fresh.line=-1;fresh.targets={-1,-1};fresh.crosspoints={-1,-1};
        const int id=e.create(e.a.models,3,fresh);e.owner.models.push_back(id);e.reference();
        const auto& head=get(graph.subcurves,forward?old.subcurves.front():old.subcurves.back(),e.budget);
        int previous_source=forward?head.vertex_a:head.vertex_b;
        int previous=directed&&carry&&*carry>=0?*carry:vertex(previous_source,!directed);
        if(directed&&carry&&*carry>=0){result.vertices.emplace(previous_source,previous);result.vertex_clones.emplace(previous_source,previous);}
        for(std::size_t step=0;step<old.subcurves.size();++step){
            const int source_sc=old.subcurves[forward?step:old.subcurves.size()-1-step];
            std::optional<VectorSubcurve> captured_sub;const auto* sub_pointer=&get(graph.subcurves,source_sc,e.budget);
            if(&graph==&e.a){captured_sub=*sub_pointer;sub_pointer=&*captured_sub;}const auto& sc=*sub_pointer;
            if(sc.model!=source_id||(forward?sc.vertex_a:sc.vertex_b)!=previous_source||(!directed&&result.subcurves.contains(source_sc)))throw std::invalid_argument("Vector source copy MC/SC chain is not reciprocal and continuous");
            const int source_end=forward?sc.vertex_b:sc.vertex_a;const int end=vertex(source_end,!directed);std::array<int,2> control;
            if(old.flags&1){const auto& a=e.vertex(previous);const auto& b=e.vertex(end);const auto half=[](int x,int y){const auto n=std::int64_t(x)+y;return int(n>=0?n/2:-((-n+1)/2));};control={half(a.x,b.x),half(a.y,b.y)};}
            else control=point(sc.control_x,sc.control_y);
            const int copied=e.create(e.a.subcurves,2,VectorSubcurve{previous,end,control[0],control[1],id,{}});
            e.vertex(previous).subcurves.push_back(copied);e.vertex(end).subcurves.push_back(copied);e.model(id).subcurves.push_back(copied);e.reference(3);
            if(!(old.flags&2)&&(directed||old.line>=0)){
                for(auto node:sc.scales){const auto& scale=get(graph.scales,node,e.budget);const int knot=e.create(e.a.scales,1,scale);e.sub(copied).scales.push_back(knot);result.scales.emplace(node,knot);e.reference();}
                //5A7F00(reverse0) invokes5A5AC0(0): exchange payloads across
                // the existing node identities; never reverse the linked IDs.
                if(directed&&!forward){e.budget.charge(32*sc.scales.size()+1);charge(e.storage,32*sc.scales.size()+128,512ull*1024*1024,"Directed width reverse storage limit");std::vector<VectorLineScale> payloads;for(auto node:e.sub(copied).scales)payloads.push_back(e.value(e.a.scales,node));const auto ids=e.sub(copied).scales;
                    for(std::size_t i=0;i<ids.size();++i){auto value=payloads[ids.size()-1-i];value.parameter_bits=std::bit_cast<std::uint32_t>(float(numeric_detail::binary64(1-double(std::bit_cast<float>(value.parameter_bits)))));e.value(e.a.scales,ids[i])=value;}}
            }
            result.subcurves.emplace(source_sc,copied);result.subcurve_clones.emplace(source_sc,copied);previous=end;previous_source=source_end;
        }
        for(int side=0;side<2;++side){const int from=forward?side:1-side;if(old.crosspoints[from]>=0){const int cp=crosspoint(old.crosspoints[from],directed&&!forward);e.model(id).crosspoints[side]=cp;e.reference();}}
        if(!directed)vector_geometry_detail::rebuild_models(e.a,{id},e.budget.work,e.budget.stop);
        if(carry)*carry=previous;
        result.models.emplace(source_id,id);result.model_clones.emplace(source_id,id);result.copied_models.push_back(id);return id;
    }
};
struct Copier:GeometryCopier {
    Integrator& context;VectorPaintArchive& destination;
    const VectorPaintArchive& source;const Layer& owner;const SourceCopySettings& settings;
    std::map<int,std::size_t> face_planes;
    std::map<std::size_t,std::vector<int>> source_lines;
    std::map<std::size_t,vector_graph_detail::OwnerLists> target_lines;
    std::optional<std::size_t> paint_override;
    std::set<int> selected;
    Copier(Integrator& c,const VectorPaintArchive& s,const Layer& l,const SourceCopySettings& options):GeometryCopier(c.geometry(),s.base,options.translation,false,options.matrix),context(c),destination(c.paint()),source(s),owner(l),settings(options){
        if(&source==&destination)throw std::invalid_argument("Vector source copy requires a distinct immutable source");
        if(!e.owner.border_owner)throw std::invalid_argument("Vector source copy requires the destination Border editor");
        if(settings.filtered&&!settings.regions)throw std::invalid_argument("Filtered source copy requires a Region array");
    }
    void leaf(const Bytes& raw){e.budget.charge(4*std::uint64_t(raw.size())+32);charge(e.storage,2*std::uint64_t(raw.size())+128,512ull*1024*1024,"Vector source copy material storage exceeds512MiB");charge(e.wire,raw.size(),8ull*1024*1024,"Vector source copy wire exceeds8MiB");}
    Bytes write_image(const Raster& fresh){
        //5AB120 calls shared RCVOffscreen::Write directly. Raster-plane save
        // Optimize is a different caller and would drop live low-depth samples.
        Writer outer,header;int left{},top{},right{},bottom{};
        if(!fresh.tiles.empty()){left=fresh.width;top=fresh.height;for(const auto& [position,backing]:fresh.tiles){(void)backing;left=std::min(left,position.x*256);top=std::min(top,position.y*256);right=std::max(right,std::min(fresh.width,(position.x+1)*256));bottom=std::max(bottom,std::min(fresh.height,(position.y+1)*256));}}
        const auto count=std::uint32_t(((right-left+255)/256)*((bottom-top+255)/256));header.u16(0x100);header.u32(fresh.width);header.u32(fresh.height);header.u32(fresh.depth);header.f64(fresh.resolution);header.u32(count);header.u8(fresh.color_order);outer.section(header.data);
        if(count){Writer extent,blocks;for(int n:{left,top,right,bottom})extent.i32(n);extent.u8(1);outer.section(extent.data);
            for(int y=top;y<bottom;y+=256){for(int x=left;x<right;x+=256){e.budget.charge();const auto found=fresh.tiles.find({x/256,y/256});blocks.u8(found!=fresh.tiles.end());if(found==fresh.tiles.end())continue;const auto& pixels=*found->second;uLongf size=compressBound(static_cast<uLong>(pixels.size()));Bytes compressed(size);
                if(compress2(compressed.data(),&size,pixels.data(),static_cast<uLong>(pixels.size()),Z_DEFAULT_COMPRESSION)!=Z_OK)throw std::runtime_error("Vector source texture compression failed");compressed.resize(size);blocks.u32(std::uint32_t(pixels.size()));blocks.u32(std::uint32_t(compressed.size()));blocks.raw(compressed);
            }}
            outer.section(blocks.data);
        }Writer framed;framed.section(outer.data);return framed.data;
    }
    Bytes image(const Bytes& raw){
        Reader prefix(raw);auto outer=prefix.section();auto header=outer.section();const auto version=header.u16();const auto width=header.u32(),height=header.u32(),depth=header.u32();const double dpi=header.f64();const auto count=header.u32();const auto order=header.u8();
        if((version>>8)>1||width==0||height==0||width>100000||height>100000||std::uint64_t(width)*height>268435456||!std::isfinite(dpi)||dpi<=0||count>100000)
            throw std::invalid_argument("Vector source texture image header exceeds supported domain");
        if((depth!=1&&depth!=2&&depth!=8&&depth!=16&&depth!=24&&depth!=32&&depth!=64)||order!=(depth==24?4:depth==32||depth==64?3:0))
            throw std::invalid_argument("Vector source texture image depth/color order is unsupported");
        const auto tile_bytes=std::uint64_t(256*256)*depth/8;
        // Charge decompression, independent copied backing and canonical
        // compression scratch BEFORE the first decode allocation.
        charge(e.storage,2*tile_bytes*count+4*tile_bytes+raw.size(),512ull*1024*1024,"Vector source texture clone storage exceeds512MiB");e.budget.charge(tile_bytes*count+4*raw.size());
        Reader reader(raw);const auto old=decode_offscreen(reader);if(reader.remaining())throw std::invalid_argument("Vector source texture has trailing outer image data");Raster fresh(int(width),int(height),int(depth),dpi);
        for(const auto& [position,backing]:old.tiles){e.budget.charge(tile_bytes);
            if(position.x*256>=int(width)||position.y*256>=int(height))continue;
            // The full-size equal-depth Copy fast path calls CopyAll. Its
            // REP MOVS copies entire live block backing, including padding.
            // Image archive suffixes are different objects and stay fresh.
            fresh.tiles.emplace(position,std::make_shared<Bytes>(*backing));
        }
        auto encoded=write_image(fresh);charge(e.storage,encoded.size(),512ull*1024*1024,"Vector source texture encoded clone storage exceeds512MiB");return encoded;
    }
    void owners(){
        vector_fill_detail::validate_paint_owners(source,owner,e.budget);
        std::size_t paint_count{};
        std::set<std::size_t> possible_override;
        for(std::size_t i=0;i<owner.planes.size();++i){
            e.budget.charge();const auto& plane=owner.planes[i];
            if(paint_plane(plane)){
                ++paint_count;const PaintLists lists(plane);
                for(auto id:lists.ids[0])face_planes.emplace(id,i);
            }else if(line_plane(plane))source_lines.emplace(i,vector_line_plane_references(plane));
            else if(!plane.opaque&&(plane.family==0x21210001||plane.family==0x21220001)&&!settings.planes.contains(i)){
                //Border copy never consumes an unmapped Vert child's own
                //topology. Mapped Vert copying has its separate Line route.
            }
            else if((plane.family&0xff000000u)==0x21000000u||(plane.family&0xff000000u)==0x22000000u)
                throw std::invalid_argument("Vector source copy unsupported companion Plane");
        }
        for(const auto& [from,to]:settings.planes){
            e.budget.charge();if(from>=owner.planes.size()||to>=e.layer.planes.size())throw std::invalid_argument("Vector source copy Plane map index is outside its owner");
            const auto& target=e.layer.planes[to];
            if(paint_plane(target))possible_override.insert(to);
            if(line_plane(owner.planes[from])){
                if(!line_plane(target))throw std::invalid_argument("Vector source Line maps to a non-Line Plane");
                target_lines.try_emplace(to,target,&destination);
            }else if(!paint_plane(owner.planes[from])||!paint_plane(target))throw std::invalid_argument("Vector source Paint maps to a non-Paint Plane");
        }
        //5D0C90 scans a pointer-keyed map. A unique destination is independent
        // of that unrecovered address order; never substitute Plane index order.
        if(paint_count==1){if(possible_override.size()>1)throw std::invalid_argument("Vector source Paint override requires native Plane pointer order");if(!possible_override.empty())paint_override=*possible_override.begin();}
        if(settings.regions)for(auto id:*settings.regions){e.budget.charge();(void)get(source.regions,id,e.budget);selected.insert(id);}
        if(settings.selected)for(auto id:*settings.selected){e.budget.charge();(void)get(source.regions,id,e.budget);}
    }
    int region_of_side(const VectorModelCurve& mc,int lane){const int id=mc.targets[lane];if(id<0)return -1;if(mc.target_kinds[lane]){(void)get(source.regions,id,e.budget);return id;}return get(source.loops,id,e.budget).region;}
    bool marked(const VectorModelCurve& mc){if(mc.flags&0x10000u)return true;for(int lane=0;lane<2;++lane){const int region=region_of_side(mc,lane);if(region<0)continue;for(auto face:get(source.regions,region,e.budget).faces)if(get(source.faces,face,e.budget).flags&0x10000u)return true;}return false;}
    void models(){
        if(!settings.regions||settings.filtered){for(auto id:source.border.models){e.budget.charge();const auto& mc=get(source.base.models,id,e.budget);if(!settings.filtered||marked(mc))model(id);}}
        else for(auto id:*settings.regions){const auto& r=get(source.regions,id,e.budget);const auto walk=[&](const VectorArchiveLoop& loop){for(auto member:loop.members)model(member.model);};walk(r.outer);for(auto hole:r.holes)walk(get(source.loops,hole,e.budget));}
    }
    void lines(){
        for(const auto& [source_plane,ids]:source_lines){const auto mapping=settings.planes.find(source_plane);if(mapping==settings.planes.end())continue;
            for(auto source_id:ids){
                const auto& old=get(source.base.lines,source_id,e.budget);if(old.members.empty())throw std::invalid_argument("Vector source copy requires nonempty mapped Lines");
                const auto& first=get(source.base.models,old.members.front().model,e.budget);
                if(settings.filtered){if(!(first.flags&0x10000u))continue;}
                else if(settings.regions&&!selected.contains(region_of_side(first,0))&&!selected.contains(region_of_side(first,1)))continue;
                VectorLine fresh;fresh.half_width_bits=width(old.half_width_bits);fresh.color=old.color;
                const int id=e.create(e.a.lines,5,fresh);target_lines.at(mapping->second).lines.push_back(id);e.reference();
                for(auto member:old.members){const auto found=result.models.find(member.model);if(found==result.models.end())throw std::invalid_argument("Mapped source Line references an uncopied MC");e.line(id).members.push_back({found->second,member.selector});e.model(found->second).line=id;e.reference();}
                e.cache(id,true);vector_graph_detail::unite(result.line_dirty,e.line(id).archived_bounds);result.lines.emplace(source_id,id);result.line_clones.emplace(source_id,id);result.copied_lines.push_back(id);result.line_planes.emplace(id,mapping->second);
            }
        }
    }
    int material(int source_id,unsigned kind,std::size_t plane){
        auto& map=kind==2?result.gradations:result.textures;
        if(const auto i=map.find(source_id);i!=map.end())return i->second;
        const auto& raw=get(kind==2?source.gradations:source.textures,source_id,e.budget);leaf(raw);int id;
        if(kind==2){auto old=decode_vector_gradation(raw);if(matrix)vector_material_detail::transform(old,*matrix);VectorArchiveGradation fresh;fresh.flags=old.flags;fresh.start_x=old.start_x;fresh.start_y=old.start_y;fresh.end_x=old.end_x;fresh.end_y=old.end_y;fresh.minor_ratio_bits=old.minor_ratio_bits;
            id=context.create_material(10,plane,encode_vector_gradation(fresh));
            for(auto stop:old.stops){const auto& node_raw=get(source.gradnodes,stop,e.budget);leaf(node_raw);const auto node=decode_vector_gradnode(node_raw);const int cloned=context.create_material(9,plane,encode_vector_gradnode({node.color,node.position_bits,{}}));fresh.stops.push_back(cloned);result.gradnodes.emplace(stop,cloned);e.reference();}
            e.value(destination.gradations,id)=encode_vector_gradation(fresh);
        }else{auto old=decode_vector_texture(raw);if(matrix)vector_material_detail::transform(old,*matrix);VectorArchiveTexture fresh;fresh.flags=old.flags;fresh.offscreen=image(old.offscreen);fresh.origin_x=old.origin_x;fresh.origin_y=old.origin_y;fresh.scale_x_bits=old.scale_x_bits;fresh.scale_y_bits=old.scale_y_bits;fresh.rotation_bits=old.rotation_bits;fresh.shear_x_bits=old.shear_x_bits;
            //5AA980 passes null RCString to5AAF70. The offscreen is copied
            // without resampling; backing pixel ownership is independent here.
            id=context.create_material(11,plane,encode_vector_texture(fresh));}
        map.emplace(source_id,id);return id;
    }
    void faces(const VectorArchiveRegion& old,int region){
        for(auto source_id:old.faces){const auto& face=get(source.faces,source_id,e.budget);const auto owner_plane=face_planes.find(source_id);if(owner_plane==face_planes.end())throw std::invalid_argument("Vector source Face has no child Plane owner");
            std::optional<std::size_t> target=paint_override;if(!target){const auto mapping=settings.planes.find(owner_plane->second);if(mapping==settings.planes.end())continue;target=mapping->second;}
            VectorArchiveFace fresh;fresh.region=region;fresh.flags=face.flags;fresh.material=-1;const unsigned kind=face.flags&15;
            if(kind==1)fresh.color=face.color;else if((kind==2||kind==4)&&face.material>=0)fresh.material=material(face.material,kind,*target);
            const int cloned=context.create_face(*target,region,fresh);result.face_clones.push_back({source_id,cloned});
        }
    }
    void loop(const VectorArchiveLoop& old,bool embedded,int id){std::vector<VectorLineMember> members;for(auto member:old.members){e.budget.charge();const auto found=result.models.find(member.model);if(found!=result.models.end())members.push_back({found->second,member.selector});}context.populate_loop(embedded,id,members);}
    SourceCopyResult run(){
        owners();models();lines();const auto& regions=settings.regions?*settings.regions:source.border.regions;
        std::set<int> selection;if(settings.selected)selection.insert(settings.selected->begin(),settings.selected->end());
        for(auto source_id:regions){e.budget.charge();const auto& old=get(source.regions,source_id,e.budget);const int id=context.create_region();result.new_regions.push_back(id);result.region_clones.push_back({source_id,id});if(settings.regions||selection.contains(source_id))result.selected_regions.push_back(id);faces(old,id);loop(old.outer,true,id);for(auto hole:old.holes){const int fresh=context.create_hole(id);result.loops.emplace(hole,fresh);loop(get(source.loops,hole,e.budget),false,fresh);}}
        for(auto& [plane,list]:target_lines){
            // Pure Line publishing must not replace the single Editor's live
            // Border lists with a snapshot taken before geometry allocation.
            list.vertices=e.owner.vertices;list.models=e.owner.models;list.publish(e.layer.planes[plane],&destination);
        }
        context.publish();e.budget.charge();return std::move(result);
    }
};
}
int copy_directed_model(Editor& e,const VectorLineArchive& source,int model,bool forward,int& carry,std::array<std::int32_t,2> translation,std::optional<vector_copy_detail::Matrix> matrix){
    const auto& old=get(source.models,model,e.budget);
    // Same-pool native copies hold stable object addresses. Our vector pools
    // capture their variable chains before allocation, billed before copies.
    std::uint64_t snapshot=sizeof(VectorModelCurve)+old.subcurves.size()*sizeof(int);
    for(int sc:old.subcurves){const auto& part=get(source.subcurves,sc,e.budget);snapshot+=sizeof(VectorSubcurve)+part.scales.size()*sizeof(int);}
    charge(e.storage,snapshot,512ull*1024*1024,"Directed MC source snapshot exceeds512MiB");e.budget.charge(snapshot);
    if(carry>=0)(void)e.vertex(carry);
    GeometryCopier copier(e,source,translation,true,matrix);return copier.model(model,true,forward,&carry);
}
LineSourceCopyResult copy_line_occurrences(Editor& e,const VectorLineArchive& source,const Layer& owner,const std::vector<int>& input,const std::map<std::size_t,std::size_t>& mappings,std::array<std::int32_t,2> translation,std::optional<vector_copy_detail::Matrix> matrix){
    GeometryCopier copier(e,source,translation,false,matrix);
    if(owner.opaque||(owner.family!=0x22100000&&owner.family!=0x24400000&&owner.family!=0x20000200&&owner.family!=0x21200000&&owner.family!=0x20800000))throw std::invalid_argument("Directed Line copy requires a typed native source owner");
    if(owner.planes.size()>1024||input.size()>8192)throw std::length_error("Directed Line copy child/occurrence limit");
    std::map<int,std::size_t> source_planes;
    std::map<std::size_t,vector_graph_detail::OwnerLists> targets;
    for(std::size_t index=0;index<owner.planes.size();++index){const auto& plane=owner.planes[index];e.budget.charge(4*plane.specific.size()+32);if(plane.opaque)continue;
        const bool vertex=plane.family==0x21210001||plane.family==0x21220001;
        if(!line_plane(plane)&&!vertex)continue;
        const auto ids=vector_line_plane_references(plane);
        for(int id:ids){(void)get(source.lines,id,e.budget);if(!source_planes.emplace(id,index).second)throw std::invalid_argument("Directed Line source has competing child memberships");}
    }
    //5D4F10's source LC array already represents selection/occurrence order.
    // Its child map must contain each referenced native source owner; this
    // bounded adapter rejects a null/wrong dynamic-cast target before copying.
    for(int id:input){e.budget.charge();const auto found=source_planes.find(id);if(found==source_planes.end())throw std::invalid_argument("Directed Line source occurrence has no typed child owner");const auto mapped=mappings.find(found->second);
        if(mapped==mappings.end()||mapped->second>=e.layer.planes.size()||!line_plane(e.layer.planes[mapped->second]))throw std::invalid_argument("Directed Line source maps to no actual pure-Line destination child");
        targets.try_emplace(mapped->second,e.layer.planes[mapped->second],e.paint);
        const auto& lc=get(source.lines,id,e.budget);for(const auto member:lc.members){const auto& mc=get(source.models,member.model,e.budget);if(mc.line!=id)throw std::invalid_argument("Directed Line source member parent is not reciprocal");}
    }
    for(int source_id:input){const auto& old=get(source.lines,source_id,e.budget);const auto plane=mappings.at(source_planes.at(source_id));VectorLine fresh;fresh.half_width_bits=copier.width(old.half_width_bits);fresh.color=old.color;
        const int id=e.create(e.a.lines,5,fresh);targets.at(plane).lines.push_back(id);e.reference();int carry=-1;
        for(const auto member:old.members){const int model=copier.model(member.model,true,member.selector!=0,&carry);e.line(id).members.push_back({model,1});e.model(model).line=id;e.reference();}
        e.cache(id,true);vector_graph_detail::unite(copier.result.line_dirty,e.line(id).archived_bounds);copier.result.lines.emplace(source_id,id);copier.result.line_clones.emplace(source_id,id);copier.result.copied_lines.push_back(id);copier.result.line_planes.emplace(id,plane);
    }
    for(auto& [plane,list]:targets){list.vertices=e.owner.vertices;list.models=e.owner.models;list.publish(e.layer.planes[plane],e.paint);}
    e.owner.publish(*e.paint);e.budget.charge();return std::move(copier.result);
}
SourceCopyResult copy_source(Integrator& context,const VectorPaintArchive& source,const Layer& owner,const SourceCopySettings& settings){return Copier(context,source,owner,settings).run();}
int copy_shape_face(Integrator& context,const VectorPaintArchive& source,int face,std::size_t plane,int region,std::optional<vector_copy_detail::Matrix> matrix){
    //This leaf needs no source PlaneMap: Shape passes the actual destination
    //child directly to its Face constructor and material copy.
    Layer unused;SourceCopySettings settings;settings.matrix=matrix;Copier copier(context,source,unused,settings);
    const auto old=get(source.faces,face,context.geometry().budget);VectorArchiveFace fresh;fresh.region=region;fresh.flags=old.flags;fresh.material=-1;
    const unsigned kind=old.flags&15;if(kind==1)fresh.color=old.color;else if((kind==2||kind==4)&&old.material>=0)fresh.material=copier.material(old.material,kind,plane);
    return context.create_face(plane,region,fresh);
}
std::vector<int> source_pointer_ordered_models(const SourceCopyResult& result){
    std::vector<int> output;if(result.models.empty())return output;const int page=result.models.begin()->first>>12;
    for(auto [source,target]:result.models){if(source<0||source>>12!=page)throw std::invalid_argument("Vector source MC map requires recovered cross-page native pointer order");output.push_back(target);}return output;
}
VectorFaceVertex boundary_query_center(VectorBounds b){
    //rcwCmn SDK export915: ADD32,CDQ,SUB,SAR1. Signed division by2 has
    // exactly the same truncation, including the wrapped INT_MIN sum.
    const auto half=[](int a,int z){return std::bit_cast<int>(std::uint32_t(a)+std::uint32_t(z))/2;};return {half(b.left,b.right),half(b.top,b.bottom)};
}
BoundaryCandidates query_candidates(Integrator& context,VectorBounds bounds){
    auto& e=context.geometry();auto& a=context.paint();BoundaryCandidates result;
    const auto append=[&](std::vector<int>& output,int id){e.budget.charge(32);if(output.size()>=8192)throw std::length_error("Vector boundary candidate array exceeds8192");charge(e.storage,32,512ull*1024*1024,"Vector boundary query storage exceeds512MiB");output.push_back(id);};
    const auto positive=[](VectorBounds b){return b.left<b.right&&b.top<b.bottom;};
    const auto overlap=[](VectorBounds a,VectorBounds b){return a.left<b.right&&a.right>b.left&&a.top<b.bottom&&a.bottom>b.top;};
    const auto loop=[&](const VectorArchiveLoop& l){if(!overlap(l.archived_bounds,bounds))return false;bool hit{};for(auto member:l.members){e.budget.charge();const auto& m=e.model(member.model);if(!overlap({m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]},bounds))continue;hit=true;if(member.selector!=0)append(result.models,member.model);}return hit;};
    for(int id:a.border.regions){e.budget.charge();const auto& r=get(a.regions,id,e.budget);const auto outer=r.outer.archived_bounds;const bool bounded=positive(outer);if(bounded&&!overlap(outer,bounds))continue;
        bool hit=bounded&&loop(r.outer);for(int hole:r.holes)hit=loop(get(a.loops,hole,e.budget))||hit;
        if(hit){append(result.regions,id);continue;}
        if(bounded&&(outer.left>bounds.left||outer.top>bounds.top||outer.right<bounds.right||outer.bottom<bounds.bottom))continue;
        if(vector_fill_detail::region_contains(a,id,boundary_query_center(bounds),e.budget))append(result.regions,id);
    }
    return result;
}
}
