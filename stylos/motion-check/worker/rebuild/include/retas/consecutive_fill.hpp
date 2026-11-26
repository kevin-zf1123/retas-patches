#pragma once
#include "fill.hpp"
#include <functional>
#include <span>

namespace retas {
// Native BFIL _FILLLIST is 24 bytes. tail is retained runtime data and is
// neither a row key nor a persisted RCRegKey field. Coordinates are already
// normalized document pixels, before subtracting the target layer origin.
struct ConsecutiveFillRow {
    std::uint64_t color{0xffffffffffffffffull};
    std::int32_t x{},y{},done{},tail{};
    bool operator==(const ConsecutiveFillRow&) const = default;
};
inline constexpr std::array<std::uint32_t,18> consecutive_fill_families{
    0x10000100,0x12000200,0x14002000,0x10000400,0x11000800,0x11001000,
    0x18004000,0x10008000,0x22100000,0x21200000,0x24400000,0x40002000,
    0x40000200,0x40000800,0x48040800,0x10040000,0x40001000,0x40020000};
struct ConsecutiveFillLayerRule {
    std::int32_t select{},check{};
    bool operator==(const ConsecutiveFillLayerRule&) const = default;
};
inline constexpr std::size_t consecutive_fill_max_rows=(100000-45)/4;
struct ConsecutiveFillItem {
    std::array<ConsecutiveFillLayerRule,18> layers{{{1,0},{},{},{},{1,1},{1,1},{},{},{},{1,1}}};
    std::int32_t selected{1},focused{},show_dialog{},own_setting{1},make_adjust{};
    std::int32_t add_columns{1},del_columns{1},show_pos{},current_row{};
    std::vector<ConsecutiveFillRow> rows=std::vector<ConsecutiveFillRow>(7);
    bool operator==(const ConsecutiveFillItem&) const = default;
};
// Headless counterpart of the live recorder: update one existing row only.
// Window visibility/current BATP dispatch and pointer/view conversion belong
// to the caller. Out-of-range current_row rejects without changing the item.
void record_consecutive_fill_point(ConsecutiveFillItem&,std::int32_t document_x,
    std::int32_t document_y,std::uint64_t current_color);
// Borrowed immutable native vector owner; retain it through apply/execute.
// Translation is already prepared in target-local raw16x units. Geometry
// copies use integer translation and do not resample for a different DPI.
using ConsecutiveFillVectorReference=FillVectorReference;
struct ConsecutiveFillReferences {
    // Prepared owner-local transforms replace the native window/light table.
    // LightColor tries entries in order and ignores zero-alpha samples.
    std::optional<FillReference> light_line;
    std::vector<FillReference> light_colors;
    std::vector<ConsecutiveFillVectorReference> vector_light_lines;
};
struct ConsecutiveFillRuntime {
    FillSettings bucket;
    // Read at execution, once per eligible target. Shift is read per Done row.
    // Providers must be read-only and retain borrowed reference owners until the entire
    // apply/execute call returns, including after the provider itself returns.
    // Document API reentry invalidates publication; concurrent access is not
    // supported. Empty callbacks use bucket, false, and absent references.
    std::function<FillSettings(std::size_t layer)> bucket_provider;
    std::function<bool(std::size_t layer,std::size_t row)> shift_provider;
    std::function<ConsecutiveFillReferences(std::size_t layer)> references_provider;
};
struct ConsecutiveFillDamage {std::size_t layer{},plane{};Rect local,document;};
struct ConsecutiveFillResult {
    bool handled{},completed{},performed{},cancelled{},stale{},history_recorded{},storage_changed{};
    std::size_t targets{},attempted_rows{},changed_pixels{};
    std::vector<ConsecutiveFillDamage> damage;
    std::string error;
};
// Visible layers with BOTH rule fields nonzero execute in document order;
// plane lookup is literal ANYBIT and takes the last matching plane, regardless
// of its visibility, marker, lock, or current index. BFIL never supplies the
// shared RegionSelection to Bucket. StopIfLeaked/FillSeparated are ignored.
// Seeds inside the translated document inner frame use that work rectangle;
// other seeds fall back to the full physical raster. An inner frame extending
// beyond the physical target is explicitly unsupported, never silently clipped.
// Bounded adapter: normal raster32/64, same DPI, supported native layer/plane
// families. Paint32/64 vector owners support native16x saved-gate Region
// queries, raw Face replacement and signed native gap prepasses; Include0/2 (or Shift), or
// Include1 with exclusively owned pure/vertex-line siblings. Prepared LightColor
// references use the first nonzero-alphaWORD sample in order; no successful
// sample retains the row Color64. A missing actual LightLine reference falls
// back normally; an existing Region Face chain suppresses its geometry prepass.
// Prepared vector LightLine sources support native Paint32/64 owners with
// Paint/pure-Line planes and integer raw translations. Each source selects
// its first Region hit; all enabled sources copy geometry with an empty Plane
// map, then the last admitted target Region hit wins. Source LC/materials are
// omitted; native MC/SC/Vertex, width/CP and missing-side topology are retained.
// VertexLine owners require a separate WorkPaint conversion and reject here.
// Raster LightLine geometry copying into an unpainted vector Region rejects.
// Positive gaps use the native original-vertex snapshot, nearest target/split,
// ordinary connector or special source replacement, intersection repair and
// Region lifecycle. First returned Region hit wins, then pending Regions run
// common-boundary materialization. Ordinary false joins retain earlier cuts.
// Negative effective gaps run the nonzero driver without connectors, then
// FIRST-hit requery and pending materialization; raw0 skips that driver.
// Nonzero WholeVector preserves support-Line topology;
// WholeVector0 clones visible21120001 Lines strictly overlapping seed bounds
// into consecutive two-Loop Region membership runs (including empty streams).
// Each transition retains endpoint CP profiles and incrementally updates MC
// stroke caches, preserving Scale records and inactive CP fields. It uses
// native lowest-free IDs, cached MC bounds and ordered plane links. Extended
// Line records and missing Loop lanes reject atomically.
// Include1 walks the
// Region's outer then saved secondary loops, recoloring linked0x21120001 Lines
// by exact previous seed Color64 or enabled native fixed/custom colors; it
// unions saved Line bounds; the recolor phase retains topology/flags/common colors.
// Missing linked plane ownership rejects. Hidden Lines skip partition but
// still recolor. New Faces use previous color0. Damage follows the native
// caller rectangle, excluding partition-only context damage; complete archive
// snapshots retain every structural change. Vector mode2 follows the native
// no-propagation path, with no protected-color barrier. Ordered Border queries
// consume the original SCs and saved Loop/MC axis gates independently of render
// certification. Empty/inverted outer gates select exterior membership; holes
// still exclude points. Outer memberCount0 suppresses the direct Face write and
// caller damage, independently of membership and materialization.
// Unoutlined nonmarked common boundaries dissolve only when complete ordered
// Face chains have equal plane owners, full flags and active raw payloads. The
// native BFS/angle-predecessor walk creates or retains a destination Region,
// preserves/moves secondary Loop identities, clones same-owner materials before
// releasing old Faces and Regions, and deletes dissolved MCs. The final degree2
// normalizer merges same-Line MCs at >=150 degrees, retaining SC/Vertex IDs,
// transferring CP ownership and mirroring raw width payloads on reversal. New
// sparse slots allocate before old release, with reconstructed live cursor/H.
// Materializer-only damage belongs to the commit context, not caller damage.
// Consumed cross-page pointer-order comparisons and unsupported
// source transforms reject atomically; native x87 runtime bit parity is not
// implied. Attached
// gradients/textures remove the first matching backlink and are freed only
// when the complete pointer-node list becomes empty; gradient deletion also
// frees its exclusive live stops. Untouched leaves remain raw. Invalid child
// ownership rejects before publication. No image sampling is needed to detach.
// Raw Color64 is never depth-quantized. A missing target Face uses the native
// lowest-free ID and reconstructed runtime H, including sparse/unordered raw
// pools, appending to Region/Plane lists and saving the Face pool ID-sorted.
// changed_pixels counts only raster writes, storage_changed includes Faces.
// This query profile caps wire at8MiB,4096 records/8192 references and1024
// planes, plus the shared work/storage bounds. Scalar/white-keyed, opaque and
// other mixed-DPI target workers reject. Eligible nonopaque vector owners without active Done rows or a
// matching plane complete without a worker, retaining per-target history.
// One native first-sibling profile is read, with its visibility gate; ordinary
// depth1 companions use fixed black/transmit samples, not an editable target
// or an inferred display LUT. Prepared Light surfaces replace window tables.
// Companion seed admission requires alpha0; its bulk pixels use the separate
// alpha0-or-white HARD gate before IncludeFill work/support. LightLine is
// installed only for target seed alphaWORD0 and requires raw32 equality with
// its cached reference seed (64-bit reference samples use their high bytes).
// Reuses the existing normal Bucket solver with native reported-rectangle
// physical writeback, including same-valued pixels and sparse tile holes.
// Native per-target success (even no rows/no plane/no pixel change)
// still owns one compound history entry when an eligible target was visited.
// completed is headless traversal completion, not raw framework1219/2 status.
// Atomic candidate publication is a modern safety policy (the native framework
// also has a no-history path that can retain mutations before cancellation).
// Modern source/retained/scratch limits:512MiB each, total work128Mi units,
// including ordered Light samples and conservative64-bit tile copy/check work;
// close-gap worst-case work is conservatively quadratic in physical pixels.
ConsecutiveFillResult apply_consecutive_fill(DocumentData&,const ConsecutiveFillItem&,
    const ConsecutiveFillRuntime&,std::stop_token stop={});
// One atomic PreserveCurrent history publication. Floating content rejects;
// failure, cancellation and stale provider reentry publish no candidate.
ConsecutiveFillResult execute_consecutive_fill(Document&,const ConsecutiveFillItem&,
    const ConsecutiveFillRuntime&,std::stop_token stop={});
}
