#pragma once
#include "vector_boundary_integrate.hpp"

namespace retas::vector_mask_detail {
struct TemporaryLineRelease {
    int line{-1},previous{-1},next{-1},members{},count{},cursor{},high{-1};
    bool live{};
    auto operator<=>(const TemporaryLineRelease&)const=default;
};
struct TemporaryLineCleanup {
    std::size_t plane{};
    std::vector<int> array,released_sources,reused_slots;
    std::vector<TemporaryLineRelease> releases;
    int head{-1},tail{-1},count{},cursor{},high{-1};
    bool native_archive_would_fail{};
};
struct RegionConversion {
    std::vector<int> regions,selected_regions,faces;
    VectorBounds context_dirty;
    //Exact pre-copy5C9570 state, including old pointer-array aliases and
    //negative cached counts. This ephemeral header is not a CEL owner list.
    std::optional<TemporaryLineCleanup> temporary_lines;
};
//5D5720/5D38E0/5C3DD0 followed by5C9570 and the5830F0 scratch
//Face packet. The Integrator/Editor belong to a private scratch candidate;
//their sparse-pool cursors remain live throughout all normalization phases.
//The fresh sampling LC records must form the native contiguous seed walk.
//The modern carrier stores live LC references; the raw temporary Line header
//above survives separately until the caller's after-copy Line+4C reset.
RegionConversion materialize_samples(vector_boundary_detail::Integrator&,
    std::vector<int>& temporary_lines,const std::map<int,std::size_t>& line_planes,
    std::size_t paint_plane,std::uint64_t color,bool create_faces=true);
//5D38E0/5C3DD0 lower entry for directed Line-to-Paint copying. Validates
//the ACTUAL cloned LC stride domain, not source identities. Returns the raw
//emitted Region array and retains LC occurrences, including released slots
//and replacements appended by5B59D0. No selector filtering, Face packet,
//5C9570 cleanup or exterior merge is applied by this lower entry.
RegionConversion materialize_retained_lines(vector_boundary_detail::Integrator&,
    std::vector<int>& lines,std::map<int,std::size_t>& line_planes);
}
