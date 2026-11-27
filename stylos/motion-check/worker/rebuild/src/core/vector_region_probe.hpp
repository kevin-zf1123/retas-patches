#pragma once
#include "vector_probe.hpp"
#include "retas/vector_face_archive.hpp"

namespace retas::vector_probe_detail {
// Original-span Region queries use saved Loop/MC axis gates independently of
// material sampling, width nodes, geometric certification or cached pixels.
struct PaintGraph {
    Graph lines;
    std::map<int,const VectorArchiveLoop*> loops;
    std::map<int,const VectorArchiveRegion*> regions;
    explicit PaintGraph(const VectorPaintArchive&,Budget);
    std::vector<int> axis_values(const VectorArchiveLoop&,int,bool horizontal,Budget) const;
    std::vector<int> axis_values(const VectorArchiveRegion&,int,bool horizontal,Budget) const;
    int classify(const VectorArchiveLoop&,VectorFaceVertex,Budget) const;
    bool contains(const VectorArchiveRegion&,VectorFaceVertex,Budget) const;
    bool rectangle_matches(const VectorArchiveRegion&,const Scope&,bool all,Budget) const;
    //5C7F90 masked query uses a temporary1bit original-span stencil including
    // native present-zero cursor blocks. Actual bitmap DPI separates bbox and geometry scales;
    // raw_dpi is the Region topology owner's resolution, not Face material DPI.
    // An explicit geometry_scale is topology DPI / carrier DPI. Selection bbox
    // mapping remains scope.forward; zero derives geometry from actual bitmap DPI,
    // or retains the explicit raw-grid profile when bitmap_dpi is absent.
    BinaryMask stencil(const VectorArchiveRegion&,const Scope&,double raw_dpi,
        std::uint64_t& retained_storage,Budget,double geometry_scale=0) const;
    //5A2150/5DFC00: caller projects the saved bbox and supplies each native
    // constructor clip. Write directly into shared scratch without clearing.
    void stencil_into(const VectorArchiveRegion&,VectorBounds draw,int scale,
        std::array<int,2> raw_offset,double raw_dpi,BinaryMask&,
        std::uint64_t& retained_storage,Budget,const VectorRenderContext* transformed=nullptr) const;
    bool matches(const VectorArchiveRegion&,const Scope&,bool all,double raw_dpi,
        std::uint64_t& retained_storage,Budget,double geometry_scale=0) const;
};
}
