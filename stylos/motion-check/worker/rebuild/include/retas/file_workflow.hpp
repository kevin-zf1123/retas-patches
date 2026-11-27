#pragma once
#include "document.hpp"

namespace retas {
class ScoreDocument;
enum class CloseSaveResponse {Save,Discard,Cancel};
enum class FileDocumentKind {Cel,Score,Chart};
enum class CloseSaveDecision {Accept,Prompt,Save};
struct CloseSavePolicy {
    std::int32_t cel{},score{},chart{};
    CloseSaveDecision decide(FileDocumentKind kind,bool modified,bool last_view,bool eligible_path) const noexcept;
};
// Headless Score close adapter uses its ordinary SXF writer. An unnamed Save
// returns false so a UI owner can obtain a Save As path; write errors propagate.
bool prepare_score_close(ScoreDocument&,const CloseSavePolicy&,bool last_view,
    const std::function<CloseSaveResponse()>& prompt);
// Bind live identities to the IDs in the last opened/saved file. Canonical
// native saves write one-based ordinals; loaded files retain their actual IDs.
struct SavedPlaneBinding {std::uint64_t identity{};int file_id{};};
struct SavedLayerBinding {std::uint64_t identity{};int file_id{};std::vector<SavedPlaneBinding> planes;};
using SavedRasterBindings=std::vector<SavedLayerBinding>;
SavedRasterBindings saved_raster_bindings(const DocumentData&,bool canonical_ids);
bool can_revert_raster_selection(const Document&) noexcept;
// Safe rewrite: private candidate, native AreaMask/Copy composition, one history step. Missing
// IDs skip; matched incompatible families reject atomically. No match/overlap has no history; an admitted copy records history even for equal samples.
// Rectangle edges scale per plane DPI; mask transfer follows native integer
// expansion and lower-DPI no-write. Source/destination depths may differ.
bool revert_raster_selection(Document&,const DocumentData& disk,const SavedRasterBindings&,bool native_disk=true);
}
