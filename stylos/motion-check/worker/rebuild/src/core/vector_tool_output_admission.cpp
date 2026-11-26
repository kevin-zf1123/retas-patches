#include "vector_tool_output_admission.hpp"
#include "vector_stored_render_costs.hpp"
#include "vector_probe.hpp"
#include "vector_material_edit.hpp"
#include "../io/vector_line_projection.hpp"
#include "../io/vector_face_projection.hpp"
#include "retas/vector_color_metadata.hpp"
#include <bit>
#include <cmath>

namespace retas::vector_tool_output_detail {
namespace {
using vector_material_detail::charge;
using vector_probe_detail::Budget;
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Vector tool output admission cancelled");}
}
void admit_metadata(const Layer& l){admit_metadata(l,false);}
void admit_metadata(const Layer& l,bool allow_paint_mode4){
    admit_metadata(l,allow_paint_mode4?PaintProfile::Fill:PaintProfile::Normal);
}
void admit_metadata(const Layer& l,PaintProfile profile){
    const bool allow_paint_mode4=profile!=PaintProfile::Normal;
    // Modern Document consumer profile, not552640's native host gate.
    const bool brush_owner=profile==PaintProfile::Brush&&(l.family==0x21200000||l.family==0x20800000);
    const bool owner_ok=brush_owner?(l.common.opacity>=0&&l.common.opacity<=100&&l.common.opacity_mode>=1&&l.common.opacity_mode<=5)
        :(l.common.opacity==100&&(l.common.opacity_mode==1||(allow_paint_mode4&&(l.family==0x21200000||l.family==0x20800000)&&l.common.opacity_mode==4)));
    if(!owner_ok)throw std::invalid_argument("Vector tool owner opacity is outside the current Document consumer profile");
    if(l.family!=0x21200000&&l.family!=0x20800000){if(allow_paint_mode4)throw std::invalid_argument("Fill mode4 admission requires a Paint/Select owner");return;}
    const auto zero=[](const CommonElement& c){for(const auto& p:c.position){if(p.value==0&&p.unit==-1&&p.reference==0)continue;if(p.value!=0||!std::isfinite(p.reference)||p.reference<=0)return false;switch(p.unit){case 0:case 1:case 2:case 3:case 5:case 6:break;default:return false;}}return true;};
    if(!zero(l.common))throw std::invalid_argument("Vector tool Paint owner position is outside the current Document consumer profile");
    for(const auto& p:l.planes){
        const bool brush_paint=profile==PaintProfile::Brush&&(p.family==0x22100001||p.family==0x22100002);
        const bool admitted=brush_paint?(p.common.opacity>=0&&p.common.opacity<=100&&p.common.opacity_mode>=1&&p.common.opacity_mode<=5)
            :(p.common.opacity==100&&(p.common.opacity_mode==1||(allow_paint_mode4&&p.family==0x22100001&&p.common.opacity_mode==4)));
        if(p.common.x||p.common.y||!zero(p.common)||!admitted)throw std::invalid_argument("Vector tool Paint child metadata is outside the current Document consumer profile");
    }
}
void admit_output(const Layer& layer,VectorPaintArchive* private_encoded_candidate,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){admit_output(layer,private_encoded_candidate,storage,work,stop,false);}
void admit_output(const Layer& layer,VectorPaintArchive* private_encoded_candidate,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop,bool allow_paint_mode4){
    admit_output(layer,private_encoded_candidate,storage,work,stop,allow_paint_mode4?PaintProfile::Fill:PaintProfile::Normal);
}
void admit_output(const Layer& layer,VectorPaintArchive* private_encoded_candidate,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop,PaintProfile profile){
    poll(stop);
    // Enforce the caller's profile before any owning projection/recipes.
    // The false wrapper preserves every other producer's mode1 boundary.
    admit_metadata(layer,profile);
    const bool paint_owner=layer.family==0x21200000||layer.family==0x20800000;
    if(paint_owner!=bool(private_encoded_candidate))throw std::invalid_argument("Vector tool output admission requires the matching private Paint candidate");
    charge(storage,0,512ull*1024*1024,"Vector tool prior staging storage exceeds limits");
    charge(work,0,128ull*1024*1024,"Vector tool prior staging work exceeds limits");
    // Match Document's persistent normal RGBA consumer, including its actual
    // fixed-point outline/profile gates. No bitmap or full pixel render is run.
    const auto& c=layer.common;
    if((c.depth!=32&&c.depth!=64)||c.width<=0||c.height<=0||c.width>100000||c.height>100000)throw std::invalid_argument("Vector tool output requires bounded32/64 owner dimensions");
    const VectorRenderContext context{c.width,c.height,c.depth,c.resolution,16,{},std::nullopt,2};
    vector_render_detail::StoredLineCacheCosts costs;
    if(!private_encoded_candidate){const auto projected=vector_render_detail::decode_stored_line_planes(layer);costs=vector_render_detail::stored_line_planes_costs(projected,context,{storage,work},stop);}
    else{
        // Document selects its consumer by actual native LC occurrences.
        // Empty Line lists use the independent Face renderer, whose material
        // query domain (notably the gradient context) differs from mixed Paint.
        bool mixed=false;
        for(const auto& p:layer.planes)if(p.family==0x21110001||p.family==0x21120001||p.family==0x21210001||p.family==0x21220001)
            mixed=mixed||!vector_line_plane_references(p).empty();
        if(!mixed)vector_archive_detail::validate_stored_line_attachments(private_encoded_candidate->base);
        const auto w=std::uint64_t(c.width),h=std::uint64_t(c.height);
        const auto logical=((w+255)/256)*((h+255)/256)*65536*std::uint64_t(c.depth/8)+((w+31)/32)*h*4+((w+255)/256)*((h+255)/256);
        if(logical>512ull*1024*1024-storage)throw std::length_error("Vector tool Paint projection storage exceeds limits");
        std::map<int,std::shared_ptr<const VectorShapeTextureRecipe>> recipes;
        auto faces=mixed?vector_render_detail::project_stored_paint_faces(*private_encoded_candidate,layer,recipes,logical+storage)
            :vector_archive_detail::project_native_paint_faces(*private_encoded_candidate,layer,logical+storage);
        std::vector<vector_render_detail::StoredLinePlane> lines;
        if(mixed)lines=vector_render_detail::project_stored_paint_lines(layer,private_encoded_candidate->base);
        const Budget budget{work,stop};std::array<std::uint64_t,8> totals{};
        const auto count=[&](std::size_t field,std::uint64_t n){charge(totals[field],n,100000,"Vector tool Paint expanded graph limit");};
        std::map<int,int> materials;
        if(mixed){budget.charge(private_encoded_candidate->faces.size()*(1+std::bit_width(private_encoded_candidate->faces.size())));for(const auto& f:private_encoded_candidate->faces)materials.emplace(f.index,f.value.material);}
        std::vector<vector_render_detail::StoredPaintPlane> views(mixed?layer.planes.size():0);
        std::vector<std::vector<const VectorFaceGeometry*>> face_references(mixed?0:layer.planes.size());
        for(std::size_t i=0;i<layer.planes.size();++i){budget.charge();
            if(mixed){auto& view=views[i];view.lines=std::move(lines.at(i));view.visible=layer.planes[i].common.flags[1]!=0;}
            for(const auto& f:faces.at(i).faces){const auto& g=f.geometry;std::uint64_t spans{},members{},contours{};
                const auto stops=g.gradation?g.gradation->stops.size():0;count(7,stops);budget.charge(stops);
                for(const auto& m:g.models)spans+=m.spans.size();for(const auto& l:g.loops)members+=l.members.size();
                const auto contour=[&](std::uint32_t id){for(const auto& m:g.loops.at(id).members)contours+=g.models.at(m.model).spans.size();};contour(g.outer_loop);for(auto id:g.holes)contour(id);
                count(0,1);count(1,g.vertices.size());count(2,g.models.size());count(3,g.loops.size());count(4,g.holes.size());count(5,spans);count(6,members);budget.charge(contours*contours+g.vertices.size()+g.models.size()+g.loops.size()+spans+members);
                if(mixed){auto& view=views[i];view.faces.push_back(&g);view.texture_recipes.push_back((g.material_flags&15)==4?recipes.at(materials.at(f.face_index)):nullptr);}
                else face_references[i].push_back(&g);
            }
        }
        // The owning Face carrier and recipe map live through the complete
        // prepare call. Texture leaves moved into recipes affect only this
        // private decoded candidate; its encoded persistent bytes stay intact.
        if(mixed)costs=vector_render_detail::stored_paint_planes_costs(views,context,{storage,work},stop);
        else{
            std::vector<VectorFacePlaneView> face_views;face_views.reserve(face_references.size());
            for(const auto& refs:face_references)face_views.push_back({refs});
            costs=vector_render_detail::stored_face_planes_costs(face_views,context,{storage,work},stop);
        }
    }
    storage=costs.storage;work=costs.work;poll(stop);
}
}
