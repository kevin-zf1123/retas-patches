#pragma once
#include "floating.hpp"

namespace retas {
// Native COPY holder, independent of a live floating selection. Rectangles
// and owner XY are raw capture geometry; no floating delta/target is exported.
struct SelectionCopyPacket {
    using Source=FloatingSourceIdentity;
    std::shared_ptr<const std::vector<Layer>> layers;
    std::shared_ptr<const std::vector<Source>> sources;
    Rect tight_bounds,capture_frame;
    // Relative to the producing Document only; this is not a destination ID.
    bool source_is_current_document{},current_plane_only{};
};
class TextFontBackend;
struct SelectionCopyOptions {
    // Native PrefInfo.CopyPaintAndLine: every nonzero value enables the fold.
    std::int32_t copy_paint_and_line{1};
    bool current_plane_only{};
    const TextFontBackend* text_fonts{};
};
struct ConvertPasteCandidates {std::vector<std::uint32_t> layers,planes;};
ConvertPasteCandidates convert_paste_candidates(const DocumentData&,const SelectionCopyPacket&,bool current_plane_only=false);
struct ConvertPasteOptions {bool new_layer{true};bool current_plane_only{};std::uint32_t family{0x10000100};Bytes name;};
struct PlainPasteImport {
    std::shared_ptr<const FloatingSelection> floating;
    RegionSelection selection;
};
// Cross-document General/PaintMono payloads: canonical seven-depth rasters,
// admitted Pure/Vert, Paint/Select, Ruler and Shape owners. Receiver context
// recalculates the kind; supported source material modes are retained.
// DPI retags use the first source owner without resampling raw pixels/pools.
// Does not resolve exported source identities; normalized consumer admission
// precedes publication, including calls without a preview callback.
// Content selection is derived from the prepared native helper. An empty
// content mask clears selection and returns no floating continuation.
PlainPasteImport prepare_plain_paste(const DocumentData& target,
    const FloatingSelection& exported,bool shrink_selection=false);
// Explicit foreign import of a COPY holder. It does not infer native local
// writeback from the producer-relative provenance bit.
PlainPasteImport prepare_plain_paste(const DocumentData&,const SelectionCopyPacket&,
    bool shrink_selection=false);
PlainPasteImport prepare_plain_paste(const DocumentData&,const SelectionCopyPacket&,
    bool shrink_selection,const RegionSelection& carrier);
// Retain an existing selection carrier's size and vector flags; origin and
// indication are reset by import. The three-argument form uses page storage.
PlainPasteImport prepare_plain_paste(const DocumentData&,const FloatingSelection&,
    bool shrink_selection,const RegionSelection& carrier);
PlainPasteImport prepare_targeted_paste(const DocumentData&,const FloatingSelection&,FloatingTarget);
PlainPasteImport prepare_targeted_paste(const DocumentData&,const FloatingSelection&,FloatingTarget,
    bool shrink_selection,const RegionSelection& carrier={});
// Validate an imported foreign payload independently of the current target
// layer/plane. Preview must not manufacture a local source map to admit it.
void validate_foreign_floating(const DocumentData& target,const FloatingSelection&);
// Prepare whole ordinary layers in payload order, translated by live delta.
// The Document owner appends them at its layer-root end during finalization
// and owns AddLayer + floating history. Neither argument is mutated here.
std::vector<Layer> prepare_pasted_layers(const DocumentData& target,const FloatingSelection&);
}
