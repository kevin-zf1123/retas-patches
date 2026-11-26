#pragma once
#include "retas/vector_face_render.hpp"
#include "retas/fill.hpp"
#include <map>

namespace retas::vector_probe_detail {
struct Budget {
    std::uint64_t& work;
    std::stop_token stop;
    void charge(std::uint64_t n=1) const;
};
struct Scope {
    bool configured{};
    VectorBounds bounds{};
    const BinaryMask* mask{}; // Immutable borrow throughout query/mutation.
    Rect mask_bounds{}; // Native extent/clip before raw coordinate mapping.
    double forward{1},inverse{1};
    double bitmap_dpi{}; // Actual carrier DPI; zero keeps explicit legacy raw-grid calls.
    std::array<std::int32_t,2> offset{};
    static Scope rectangle(VectorBounds);
    // Native5C76E0 binds the active-bit extent, distinct from allocated blocks.
    static Scope masked(const BinaryMask&,double raw_units_per_pixel,
        std::array<std::int32_t,2> raw_offset,const std::optional<Rect>&,Budget);
    bool point(VectorFaceVertex,Budget) const;
};
struct Curve {
    VectorFaceVertex start,control,end;
    bool linear{}; // MC bit0, not an inferred endpoint/control alias.
};
struct Crossing {VectorFaceVertex point;float parameter{};};
VectorPoint position(const Curve&,float,Budget);
VectorFaceVertex integer_position(const Curve&,float,Budget);
VectorFaceVertex node_position(const Curve&,float,Budget);
double four_chord_length(const Curve&,Budget);
double insertion_length(const Curve&,Budget);
std::vector<float> axis_roots(const Curve&,bool horizontal,int query,bool bias,Budget);
std::vector<Crossing> rectangle_intersections(const Curve&,VectorBounds,Budget);
bool curve_matches(const Curve&,const Scope&,bool all,Budget);
bool overlap(VectorBounds,VectorBounds);
struct Graph {
    std::map<int,const VectorVertex*> vertices;
    std::map<int,const VectorSubcurve*> subcurves;
    std::map<int,const VectorModelCurve*> models;
    explicit Graph(const VectorLineArchive&,Budget);
    Curve curve(const VectorModelCurve&,const VectorSubcurve&,Budget) const;
    bool model_matches(const VectorModelCurve&,const Scope&,bool all,Budget) const;
    bool line_matches(const VectorLine&,const Scope&,bool all,Budget) const;
};
}
