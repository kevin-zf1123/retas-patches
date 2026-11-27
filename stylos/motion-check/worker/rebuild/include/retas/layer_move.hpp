#pragma once
#include "document.hpp"
#include "viewport.hpp"
#include <span>

namespace retas {
enum class LayerMoveTarget { SelectedLayers, Grid, Guide };
struct LayerMoveSettings {
    LayerMoveTarget target{LayerMoveTarget::SelectedLayers};
    bool operator==(const LayerMoveSettings&) const = default;
};
// The live tool retains its raw point across key groups, views and tool
// switches. MainWindow shares this transient phase among its Canvas views.
struct LayerMovePhase {ViewPoint point;std::uint64_t revision{};};
// Selected includes hidden/inhibited history participants. Only the eligible
// subset moves; callers must snapshot before applying the first increment.
std::vector<std::size_t> selected_move_layers(const DocumentData& document);
bool layer_move_eligible(const Layer& layer);
// Atomic raster or decoded vector-layer increment. All candidate layers are prepared before
// publication; unsupported target families and invalid geometry throw.
bool move_raster_layers(DocumentData& document,std::span<const std::size_t> selected,int dx,int dy);
//Current-root movement additionally moves Document's attached Temp pair once,
//regardless of the carriers' own visible/locked/selected flags. Prepare all
//primary and attached targets before publishing any of them.
bool move_layer_targets(Document&,std::span<const std::size_t> selected,int dx,int dy);
// Native layer vslot +0x28 resize used by raster merge. Bounds are layer-local
// and already unioned with the old extent. Every raster child participates;
// left/top are tile padded and the last raster child supplies owner geometry.
// Stack transfer may admit decoded raster companions alongside a vector graph;
// ordinary movement retains its established strict admission by default.
bool expand_raster_layer(Layer& layer,Rect bounds,bool allow_mixed_vector=false);
}
