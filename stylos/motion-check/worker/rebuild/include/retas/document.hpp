#pragma once
#include "raster.hpp"
#include "selection.hpp"
#include "floating_transform.hpp"
#include "frame_move.hpp"
#include "vanishing_point_move.hpp"
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <map>
#include <variant>

namespace retas {
namespace adjust_render_detail { struct TableCache; }
namespace text_font_detail { struct ExportMasks; }
namespace vector_paint_history_detail { class Edit; struct DocumentBinding; }
namespace paper_setting_detail { struct Context; }
struct FloatingSelection;
struct SelectionCopyPacket;
struct SelectionCopyOptions;
struct ConvertPasteOptions;
struct FloatingTarget;
struct ImageSizeRequest;
struct ImageResolutionRequest;
struct TrimPacket;
struct DocumentGeometryOutcome;
struct PaperSettingRequest;
struct PaperSettingBackup;
struct PaperSettingOutcome;
struct MovePlaneOutcome;
struct AdjustParameters;
enum class AdjustFilterExecution;
struct ElementConvertOptions;
struct FillResult;
struct FilterToolSettings;
struct DustSettings;
struct HsvSettings;
struct ColorBalanceSettings;
struct UnsharpMaskSettings;
struct DustFilterResult;
class DustFilterPreview;
class FilterPreview;
struct GaussianBlurSettings;
struct ContrastSettings;
struct LevelSettings;
struct ToneCurveSettings;
enum class FilterKind;
enum class LayerPlaneOperation {ConvertLayer,ConvertPlane,MergeLayers,MergePlanes};
struct Length {
    double value{};
    std::int32_t unit{0};
    double reference{144};
    bool operator==(const Length&) const = default;
};
// Standard GridLayer is a document-owned session object (secondary +0x2a8),
// not a member of the CEL root layer collection. +0x60/+0x78 are origins;
// +0x100 is the major spacing and +0x118 its subdivision count.
struct GridState {
    double dpi{144};
    std::array<int,2> origin{};
    std::array<Length,2> position{{{0,2,144},{0,2,144}}};
    Length spacing{10,2,144};
    int subdivisions{4};
    bool enabled{}; // Document-shared support gate +0x28; visibility is per view.
    bool operator==(const GridState&) const = default;
};
GridState make_grid(double dpi);
struct GridSettings {
    std::array<Length,2> position;
    Length spacing;
    int subdivisions{};
    bool operator==(const GridSettings&) const = default;
};
GridSettings grid_settings(const GridState& grid);
double grid_spacing_cm(const GridState& grid);
// Native packet application preserves typed units. Applying origins also
// recomputes the cached pixel point, including when cancelling the dialog.
void set_grid_settings(GridState& grid,const GridSettings& settings,bool apply_origins);
void move_grid(GridState& grid,int dx,int dy);
// Replay the native cached-point delta; it normalizes both origins to mm,
// even for a zero delta, instead of replacing unrelated Grid fields.
void move_grid_to(GridState& grid,std::array<int,2> origin);
struct GridLayout {double spacing_mm{};int subdivisions{};};
struct GridLine {int coordinate{};bool major{};bool operator==(const GridLine&) const = default;};
GridLayout grid_layout(const GridState& grid,double zoom);
std::vector<GridLine> grid_lines(const GridState& grid,int axis,int extent,double zoom);
// Standard GuideLayer is also a secondary, non-CEL document-session object.
// Native LYMV treats record +18 as X only for 1 and Y for all other values.
struct GuideRecord {
    Length position;
    std::int32_t orientation{},marker{};
    bool operator==(const GuideRecord&) const = default;
};
struct GuideState {
    double dpi{144};
    std::array<int,2> origin{};
    std::array<Length,2> position{{{0,2,144},{0,2,144}}};
    std::vector<GuideRecord> records;
    bool enabled{true};
    bool operator==(const GuideState&) const = default;
};
struct GuideSnapshot {
    std::size_t index{};
    GuideRecord record;
    bool operator==(const GuideSnapshot&) const = default;
};
GuideState make_guide(double dpi);
int guide_coordinate(const GuideState& guide,const GuideRecord& record);
std::vector<GuideSnapshot> selected_guides(const GuideState& guide);
void move_guide(GuideState& guide,int dx,int dy);
void move_guide_to(GuideState& guide,std::array<int,2> origin);
// Capture before live modification/removal. Markers and typed Length fields
// are retained verbatim; history only changes the explicitly captured slots.
enum class GuideEditMode {Created=1,Removed=2,Modified=3};
// Process-local identity follows an element through snapshots and reordering.
// It never enters a native archive. Insertion assigns a fresh identity.
inline std::uint64_t next_element_identity() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1,std::memory_order_relaxed);
}
struct CommonElement {
    int width{},height{},depth{32};
    double resolution{144};
    Bytes name;
    std::array<std::int32_t,5> flags{0,1,0,0,0};
    std::uint64_t color{};
    int x{},y{};
    std::array<Length,2> position{};
    // The native +0x94 mode also selects white-keyed raster behavior (value 5).
    int opacity{100},opacity_mode{1},field_14{-1};
    Bytes suffix;
    std::uint64_t runtime_identity{next_element_identity()};
    //Native CElement+1C, separate from busy/visible/selected flags. Fresh
    //elements start enabled; PaintMan CEL load propagates its owner-family gate.
    bool runtime_operable{true};
};
struct TextRasterCache {
    Bytes specific;int source_width{},source_height{};double source_dpi{};Raster raster;
    // Plane-local origin of derived backing. Native text geometry may extend
    // left/up of zero; carrier dimensions do not clip the glyph bitmap.
    std::array<int,2> origin{};
    bool static_geometry{}; // bounded native-geometry Qt producer
    std::array<int,4> coordinate_bounds{}; // minXY/maxXY for checked owner translation
    // Independent glyph/leader coverage, plane-local unless image_geometry.
    // The merged raster above
    // remains an opaque-density adapter; partial density must consume both masks.
    std::shared_ptr<const text_font_detail::ExportMasks> masks;
    // Mixed-child-DPI image geometry uses absolute origins and is tied to
    // the raw owner origin. Ordinary view uses a separate zero-placement blit.
    bool image_geometry{};
    std::array<int,2> source_owner_origin{};
};
struct Plane {
    bool opaque{};
    std::uint32_t family{0x10000001};
    CommonElement common;
    Bytes specific,suffix,opaque_record;
    std::optional<Raster> raster;
    std::shared_ptr<const TextRasterCache> runtime_text_cache;
    // Attached helper tables are session state, independent of archived parameters.
    std::shared_ptr<const adjust_render_detail::TableCache> runtime_adjust_cache;
    bool pixels_changed{};
    bool raster_modified{}; // native +90 dirty latch, including Adjust; Bind/Undo/Redo latch, Save clears
    bool visibility_modified{}; // runtime owner dirty state; not serialized
    bool frame_modified{}; // native Frame owner dirty; Bind/Undo/Redo latch, Save clears
    bool vanishing_modified{}; // native Vanishing owner dirty; not serialized
};
// Returns only a derived cache that still matches the native text payload and geometry.
const Raster* text_raster_cache(const Plane&) noexcept;
struct Layer;
// Also checks the producer's owner/DPI/density admission after property edits.
const Raster* text_raster_cache(const Layer&,const Plane&,double dpi) noexcept;
// Inspect only the bounded Adjust wrapper discriminator; filter parameters and
// derived LUTs are not decoded or changed. Nullopt means absent/malformed wrapper.
std::optional<std::uint32_t> archived_adjust_filter_kind(const Plane& plane) noexcept;
struct Layer {
    std::uint32_t runtime_new_plane_type{}; // Native owner +FC; session state, never archived.
    bool opaque{};
    std::uint32_t family{0x10000100};
    CommonElement common;
    Bytes specific,suffix,plane_suffix,opaque_record;
    int current_plane{};
    std::vector<Plane> planes;
    bool visibility_modified{}; // follows this live/detached layer; not serialized
    // Native allocator H can exceed maxlive after a release. Keep this live
    // execution state through snapshots and successive vector edits, while
    // freshly decoded files reconstruct H from occupied IDs. Never serialized.
    // Indices0..5 are base pools;6..11 are face-extension pools.
    std::array<std::optional<std::int32_t>,12> runtime_vector_high_water{};
    //630270 only resizes geometry when the native view cache exists. Loaded
    //and ordinary factory-created vector layers construct that cache.
    //Non-solid Face Color64 survives native material replacement but is not
    //serialized. Immutable backing lets modal snapshots retain that state.
    std::shared_ptr<const std::map<std::int32_t,std::uint64_t>> runtime_vector_face_colors;
    bool runtime_vector_cache_present{true}; // Runtime only; never serialized.
    // Last published shadow-cache block extent, in owner pixels. Rendering
    // publishes this separately from graph/history; a fresh cache is empty.
    Rect runtime_vector_cache_extent{};
};
namespace vector_runtime_detail {
inline bool face_colors_equal(const Layer& a,const Layer& b) noexcept {
    if(a.runtime_vector_face_colors==b.runtime_vector_face_colors)return true;
    const bool ae=!a.runtime_vector_face_colors||a.runtime_vector_face_colors->empty();
    const bool be=!b.runtime_vector_face_colors||b.runtime_vector_face_colors->empty();
    return ae||be?ae==be:*a.runtime_vector_face_colors==*b.runtime_vector_face_colors;
}
inline std::uint64_t face_colors_storage(const Layer& layer){
    if(!layer.runtime_vector_face_colors)return 0;
    if(layer.runtime_vector_face_colors->size()>100000)throw std::length_error("Runtime Face color count exceeds100000");
    return 64+96ull*layer.runtime_vector_face_colors->size();
}
}
// Native Document+28C/+290 carriers, distinct from floating selection. They
// participate in current-layer LYMV history but never enter the CEL layer list.
struct AttachedTemporaryLayers {
    std::optional<Layer> vector,raster;
};
struct BaseInfo {
    std::array<Length,16> lengths{};
    int frame_mode{};
    double scale{100},dpi{144},dialog_dpi{144};
    int flags{};
    Bytes suffix;
    int width() const;
    int height() const;
};
struct PlaneProperties {Bytes name;std::uint64_t color{};bool operator==(const PlaneProperties&) const = default;};
struct LayerProperties {Bytes name;int opacity{100},opacity_mode{1};bool operator==(const LayerProperties&) const = default;};
enum class GridAnchor {TopLeft,TopRight,BottomLeft,BottomRight,Center};
std::array<Length,2> grid_anchor_position(const BaseInfo& page,GridAnchor anchor);
std::optional<GridAnchor> matching_grid_anchor(const GridState& grid,const BaseInfo& page);
struct DocumentData {
    std::uint16_t version{0x100};
    bool dga{};
    BaseInfo base;
    std::vector<Layer> layers;
    int current_layer{};
    Bytes preview,layer_suffix,trailing;
    std::shared_ptr<const Bytes> original;
    bool unrestored_external_content{};
    // Paired only by decoders/writers; snapshots themselves leave this null.
    std::shared_ptr<const DocumentData> original_content;
    bool has_unrendered_content() const;
    Plane* active_plane();
    const Plane* active_plane() const;
    Pixel composite_pixel(int x,int y) const;
    Raster composite() const;
    // Existing rebuild compositor with an explicit output precision. 32 keeps
    // display's per-plane quantization; 64 retains 16-bit channel intermediates.
    Raster composite(int target_depth) const;
    Raster composite(int target_depth,std::stop_token stop) const;
    Raster composite(int target_depth,std::stop_token stop,bool white_background) const;
};
enum class SavePolicy { LegacyCanonical, ExactRaw };
// PreserveCurrent is for operations retaining the same layer/plane objects
// in the same order; retain their current selection and visibility on replay.
// Structural history must restore its captured selection.
enum class HistorySelection { RestoreSnapshot, PreserveCurrent };
DocumentData make_document(int width,int height,int depth=32,double dpi=144);
DocumentData decode_cel(std::span<const std::uint8_t> bytes);
Bytes encode_cel(const DocumentData& document,SavePolicy policy=SavePolicy::LegacyCanonical,bool unchanged=false,std::optional<int> compression_level={},bool v5_compatible=false);
Bytes read_file(const std::filesystem::path& path);
void write_file_atomic(const std::filesystem::path& path,std::span<const std::uint8_t> bytes);
struct CommandOutcome {
    bool handled{},applied{};
    std::string error;
};
struct BatchContentTarget {
    std::size_t layer{};
    bool vector_content{};
    std::vector<std::size_t> raster_planes;
    std::optional<std::size_t> vector_plane;
    std::optional<std::array<std::int32_t,4>> vector_damage;
    bool vector_draw_history{};
    std::vector<Rect> raster_regions; // empty means whole planes; otherwise aligned
    std::vector<std::size_t> vector_draw_planes; // explicit native Vert history collector
    bool vector_paint_history{}; // explicit native partial Paint history for Select208
    bool skip_empty_raster_regions{}; // preserve an empty native RasterPlane item without a PlaneState
    bool native_owner_entry{}; // Clear owner-order cache touch, without a history item
    bool vector_storage_only{}; // generic vector writeback outside a native history collector
};
class Document {
    friend class FloatingTransformSession;
    friend class FrameMoveSession;
    friend class VanishingPointMoveSession;
    std::shared_ptr<VanishingPointMoveSession> vanishing_move_;
    bool vanishing_move_preparing_{};
    void require_current_vanishing_point_move() const;
    std::shared_ptr<FrameMoveSession> frame_move_;
    bool frame_move_preparing_{};
    void require_current_frame_move() const;
    std::shared_ptr<FloatingTransformSession> floating_transform_;
    bool floating_transform_preparing_{};
    std::uint64_t text_preview_owner_{};
    void require_no_floating_transform() const;
    void insert_layer_checked(std::size_t,Layer,const std::string&,const std::function<void()>& before_publish);
    void prepare_selection_command(const std::string&,std::stop_token,const std::function<void(Document&,std::uint64_t&,std::uint64_t&)>&,bool publish=true,std::array<std::uint64_t,2> prior_costs={});
    void require_current_floating_transform() const;
    struct GridMove {std::array<int,2> origin;};
    struct GuideMove {std::array<int,2> origin;};
    struct GuideEdit {
        GuideEditMode mode;
        bool reverse{},captured{};
        std::vector<std::size_t> indices;
        std::vector<GuideRecord> primary,secondary;
    };
    struct RegionSelectionEdit {
        RegionSelection primary;
        std::optional<RegionSelection> secondary;
        bool reverse{};
    };
    struct RasterFloatingScope {std::uint64_t owner{};std::vector<std::uint64_t> planes;bool geometry{};};
    struct FloatingSelectionEdit {
        enum class Mode {Capture,Offset,Finalize};
        Mode mode{Mode::Capture};
        std::optional<RegionSelectionEdit> selection;
        std::shared_ptr<const FloatingSelection> opposite;
        std::shared_ptr<const raster_history_detail::FloatingBinding> binding;
        std::array<int,2> primary_offset{};
        std::optional<std::array<int,2>> secondary_offset;
        bool reverse{},source_history{};
        bool exchange_data{true},foreign_payload{};
        //Color commands scope local raster materialization to its actual
        //offscreen/Size owners; Floa metadata never owns unrelated marks.
        std::shared_ptr<const std::vector<RasterFloatingScope>> raster_scope;
    };
    struct OrderEdit {
        std::optional<std::size_t> layer; // absent: document layers; present: this layer planes
        std::vector<std::size_t> order; // destination slot -> current slot
    };
    struct CompoundEdit;
    struct DeletePlanesEdit {std::size_t layer{};std::vector<std::size_t> indices;std::vector<Plane> detached;};
    // A vector owns zero or one detached Plane without moving its Raster when
    // the history record moves (MSVC map move construction can allocate).
    struct AddPlaneEdit {std::size_t layer{},index{};std::vector<Plane> detached;};
    struct AddLayerEdit {std::size_t index{};std::optional<Layer> detached;};
    struct DeleteLayersEdit {std::vector<std::size_t> indices;std::vector<Layer> detached;};
    struct PlanePropertyEdit {std::size_t layer{},plane{};bool color{};std::variant<Bytes,std::uint64_t> saved;};
    struct LayerPropertyEdit {
        enum class Field {Name,Opacity,OpacityMode};
        std::size_t layer{};Field field{};std::variant<Bytes,int> saved;
    };
    struct FrameEdit {FrameTarget target;std::uint32_t family{};FrameParameters primary;std::optional<FrameParameters> secondary;bool reverse{};};
    struct VanishingEdit {VanishingPointTarget target;std::shared_ptr<const VanishingPointParameters> primary,secondary;bool reverse{};};
    struct TextEdit {std::uint64_t layer{},plane{};Bytes specific,name;};
    struct StackEdit {};
    struct PageTransformEdit {std::int32_t kind{};};
    struct PlaneSettingsEdit {std::uint64_t layer{},plane{};Bytes specific;bool native_binding{};};
    struct AttachedLayerMoveEdit {std::shared_ptr<const AttachedTemporaryLayers> opposite;bool refresh_indications{},restore_attached{true};};
    struct CaptureSourceEdit {
        std::uint64_t owner{};std::uint32_t family{};std::size_t hint{};
        std::shared_ptr<const std::vector<std::uint64_t>> layout;
        std::shared_ptr<const vector_paint_history_detail::Edit> paint;
        // Existing non-Paint capture storage stays scoped to its actual
        //owner/children while Paint and normal32/64 raster use typed items.
        std::shared_ptr<const Layer> storage;
        std::vector<std::size_t> planes;
        bool whole_owner{};
    };
    using RuntimeHistory=std::variant<std::monostate,StackEdit,PageTransformEdit,PlaneSettingsEdit,TextEdit,FrameEdit,VanishingEdit,GridMove,GuideMove,GuideEdit,RegionSelectionEdit,FloatingSelectionEdit,OrderEdit,AddLayerEdit,LayerPropertyEdit,DeleteLayersEdit,AddPlaneEdit,DeletePlanesEdit,std::shared_ptr<const CompoundEdit>,PlanePropertyEdit,AttachedLayerMoveEdit,std::shared_ptr<const raster_history_detail::Edit>,std::shared_ptr<const CaptureSourceEdit>>;
    struct Revision {
        DocumentData data; std::string label; std::uint64_t id; HistorySelection selection;
        RuntimeHistory runtime;
        bool frame_content{}; // snapshot transaction owns specialized parameter fields it changed
    };
    struct CompoundEdit {std::vector<Revision> past,future;};
    std::vector<Revision> undo_,redo_;
    std::size_t history_limit_{100};
    bool collecting_history_{};
    void trim_history();
    bool undo_next_{true};
    std::uint64_t revision_{},saved_{},next_{1};
    std::uint64_t state_generation_{};
    raster_history_detail::Budget* selection_command_budget_{};
    bool building_clipboard_group_{};
    std::shared_ptr<const AttachedTemporaryLayers> attached_temporary_layers_;
    std::uint64_t attached_temporary_generation_{};
    std::uint64_t grid_generation_{};
    std::uint64_t guide_generation_{};
    std::uint64_t region_selection_generation_{};
    std::uint64_t floating_selection_generation_{};
    std::shared_ptr<const FloatingSelection> floating_selection_;
    // Native clear/history-apply leaves live receiver +0x08 untouched.
    bool floating_same_document_source_{};
    // Concrete floating/raster history dirtiness; visibility is owned by the
    // affected Layer/Plane so detached objects do not dirty the live document.
    bool runtime_dirty_{};
    // Shared native document +0x20: Add/Delete replay pops/pushes this counter,
    // while Order, Guide and Property replay leave it untouched. Save clears it.
    std::uint64_t document_modify_count_{};
    std::shared_ptr<const DocumentData> raw_baseline_;
    std::shared_ptr<const vector_paint_history_detail::DocumentBinding> paint_history_manager_;
    void replay(std::vector<Revision>& source,std::vector<Revision>& destination,raster_history_detail::Budget* budget=nullptr);
    std::shared_ptr<const CaptureSourceEdit> replay_capture_source(const CaptureSourceEdit&,raster_history_detail::Budget&);
    void replay_vanishing(std::vector<Revision>& source,std::vector<Revision>& destination);
    void replay_frame(std::vector<Revision>& source,std::vector<Revision>& destination);
    void order_elements(std::optional<std::size_t> layer,const std::vector<std::size_t>& order,const std::string& label);
    bool apply_vector_indications(bool refresh,double actual_carrier_dpi,std::stop_token);
    void finalize_floating(std::optional<RegionSelection> selection,const std::string& label,bool prepared=false);
    void finalize_foreign_floating(std::optional<RegionSelection> selection,const std::string& label);
    void finalize_clipboard_floating(std::optional<RegionSelection> selection,const std::string& label);
    void publish_floating_materialization(DocumentData data,bool content_history,std::optional<RegionSelection> selection,const std::string& label,bool refresh_selection=true);
    bool begin_floating_capture(const std::string&,const std::function<void(const DocumentData&,std::shared_ptr<const FloatingSelection>)>&,bool command,bool current_only=false);
    DocumentGeometryOutcome apply_document_geometry(Rect,const std::array<Length,2>&,bool,std::stop_token,const ImageResolutionRequest* = nullptr,bool fit_camera=false,paper_setting_detail::Context* paper=nullptr);
public:
    // UI previews borrow the document graph exclusively until rollback. A
    // competing history or save operation must first settle the owning Canvas.
    void begin_text_preview(std::uint64_t owner);
    void end_text_preview(std::uint64_t owner) noexcept {if(text_preview_owner_==owner)text_preview_owner_=0;}
    bool has_text_preview() const noexcept {return text_preview_owner_!=0;}
    const std::shared_ptr<const AttachedTemporaryLayers>& attached_temporary_layers()const noexcept{return attached_temporary_layers_;}
    std::uint64_t attached_temporary_generation()const noexcept{return attached_temporary_generation_;}
    void publish_attached_temporary_layers(std::shared_ptr<const AttachedTemporaryLayers> prepared)noexcept{attached_temporary_layers_=std::move(prepared);++attached_temporary_generation_;}
    void commit_layer_move(DocumentData&& before,std::shared_ptr<const AttachedTemporaryLayers> attached_before,const std::string& label,bool restore_attached=true);
    DocumentData data;
    GridState grid;
    GuideState guide;
    RegionSelection region_selection;
    // Application VecselectType preference, independent of CEL/history.
    std::optional<std::uint32_t> application_vector_selection_flags;
    //Native Doc+284 is independent of per-owner/plane/primitive markers.
    bool vector_indication_active{};
    std::filesystem::path path;
    explicit Document(DocumentData initial) : data(std::move(initial)),grid(make_grid(data.base.dpi)),guide(make_guide(data.base.dpi)) {raw_baseline_=data.original_content;}
    bool modified() const noexcept;
    bool can_save_exact() const noexcept;
    std::uint64_t state_generation() const {return state_generation_;}
    std::uint64_t grid_generation() const {return grid_generation_;}
    std::uint64_t guide_generation() const {return guide_generation_;}
    std::uint64_t region_selection_generation() const {return region_selection_generation_;}
    std::uint64_t floating_selection_generation() const {return floating_selection_generation_;}
    bool has_floating_transform() const {return bool(floating_transform_)||floating_transform_preparing_;}
    std::shared_ptr<const FloatingTransformSession> floating_transform() const {return floating_transform_;}
    FloatingTransformBegin begin_floating_transform(FloatingTransformOperation operation=FloatingTransformOperation::Scale,
        std::uint64_t origin=0,std::stop_token stop={});
    //Known public commands are handled even when Begin has no usable payload.
    //Fresh Reverse accepts immediately; Scale/Rotate leave an active session.
    FloatingTransformCommandResult execute_floating_transform_command(
        FloatingTransformCommand,std::uint64_t origin=0,std::stop_token stop={});
    void switch_floating_transform_operation(FloatingTransformOperation);
    void preview_floating_transform(const FloatingTransformPacket&,std::stop_token stop={});
    void position_floating_transform(int dx,int dy,std::optional<FloatingTransformPoint> pivot={});
    void release_floating_transform_scale(const FloatingTransformPacket&,std::uint32_t reversals=0,std::stop_token stop={});
    // Degrees are absolute relative to the raster at the beginning of an
    // adjacent Rotate run; equal-flag records coalesce to its latest angle.
    // Geometry packets must describe that same absolute result.
    void release_floating_transform_rotate(const FloatingTransformPacket&,std::int32_t degrees,std::stop_token stop={});
    void reverse_floating_transform(bool vertical,std::stop_token stop={});
    FloatingTransformOutcome accept_floating_transform(const std::string& label="变换浮动图像",std::stop_token stop={});
    void cancel_floating_transform();
    bool has_frame_move() const {return bool(frame_move_)||frame_move_preparing_;}
    std::shared_ptr<const FrameMoveSession> frame_move() const {return frame_move_;}
    void begin_frame_move(FrameTarget,std::stop_token stop={});
    void begin_frame_drag(const FrameDragPacket&,std::stop_token stop={});
    bool update_frame_drag(FramePoint view,FramePoint world,bool shift=false,std::stop_token stop={});
    void release_frame_drag(std::stop_token stop={});
    void apply_frame_settings(const FrameSettingPacket&,std::stop_token stop={});
    void apply_frame_preset(FramePreset,std::stop_token stop={});
    void accept_frame_move(const std::string& label="移动画框",std::stop_token stop={});
    void cancel_frame_move(std::stop_token stop={});
    bool has_vanishing_point_move() const {return bool(vanishing_move_)||vanishing_move_preparing_;}
    std::shared_ptr<const VanishingPointMoveSession> vanishing_point_move() const {return vanishing_move_;}
    void begin_vanishing_point_move(VanishingPointTarget,std::stop_token stop={});
    // Identity-view convenience projects centers to document pixels. Explicit
    // projected centers must correspond in order to every current point.
    bool begin_vanishing_point_drag(VanishingPointSample,bool alt=false,std::stop_token stop={});
    bool begin_vanishing_point_drag_projected(VanishingPointSample,std::span<const VanishingPointPixel> centers,bool alt=false,std::stop_token stop={});
    bool update_vanishing_point_drag(VanishingPointSample,std::stop_token stop={});
    void release_vanishing_point_drag(std::stop_token stop={});
    // Settings require a released gesture. Native runtime header integers are
    // copied verbatim; only Begin applies the archive angle-step fallback.
    void apply_vanishing_point_parameters(const VanishingPointParameters&,std::stop_token stop={});
    void accept_vanishing_point_move(const std::string& label="移动灭点",std::stop_token stop={});
    void cancel_vanishing_point_move(std::stop_token stop={});
    bool has_floating_selection() const {return bool(floating_selection_);}
    // Geometry stages floating finalization and every participant privately.
    // Unlike the native failure path, a rejected operation leaves its floating
    // prelude uncommitted. Success clears both history directions; an accepted
    // equal-size ImageSize may publish only ordinary floating history.
    DocumentGeometryOutcome change_image_size(const ImageSizeRequest&,bool confirmed=true,std::stop_token stop={});
    DocumentGeometryOutcome fit_camera_frame(bool confirmed=true,std::stop_token stop={});
    PaperSettingOutcome change_paper_setting(const PaperSettingRequest&,bool confirmed=true,std::stop_token stop={});
    PaperSettingOutcome change_paper_setting(const PaperSettingRequest&,const PaperSettingBackup&,bool confirmed=true,std::stop_token stop={});
    // This is the native worker packet domain, independent of dialog control
    // limits. Every confirmed result rewrites page/dialog metadata and clears
    // history and helpers. Failed/cancelled preparation is fully atomic.
    DocumentGeometryOutcome change_image_resolution(const ImageResolutionRequest&,bool confirmed=true,std::stop_token stop={});
    DocumentGeometryOutcome change_batch_blank_size(const std::array<Length,4>& margins,std::stop_token stop={});
    // Native Batch28H/29V/30Left/31Right. Undo/Redo execute the inverse worker,
    // retaining native quantization and later nonhistorical common properties.
    void transform_page(std::int32_t kind,std::stop_token stop={},const std::string& label="Transform page");
    DocumentGeometryOutcome trim_document(const TrimPacket&,bool confirmed=true,std::stop_token stop={});
    DocumentGeometryOutcome trim_document(bool confirmed=true,std::stop_token stop={});
    const std::shared_ptr<const FloatingSelection>& floating_selection() const {return floating_selection_;}
    void set_history_limit(std::size_t);
    std::size_t history_limit() const noexcept {return history_limit_;}
    bool can_undo() const { return !has_floating_transform()&&!has_frame_move()&&!has_vanishing_point_move()&&!undo_.empty(); }
    bool can_redo() const { return !has_floating_transform()&&!has_frame_move()&&!has_vanishing_point_move()&&!redo_.empty(); }
    // Content snapshots own Frame/Vanishing parameters only when those fields
    // changed in this transaction. Pixel-only replay retains later parameters;
    // parameter-containing generic transactions retain snapshot restoration.
    // Called after a live edit. Allocation failure preserves the caller's
    // snapshot so its transaction can restore the edited data.
    void commit(DocumentData&& before,const std::string& label,HistorySelection selection=HistorySelection::RestoreSnapshot);
    void commit(const DocumentData& before,const std::string& label,HistorySelection selection=HistorySelection::RestoreSnapshot);
    // Publish validated, privately prepared storage when the native operation
    // has no history item. Requires settled ownership and the captured content
    // generation; object membership/families must remain unchanged. Retains
    // current selection, owner dirty flags, raw-save baseline and both history
    // directions. Success consumes prepared without allocating and advances
    // only state_generation. Rejection leaves prepared and live data untouched.
    void publish_prepared_without_history(DocumentData&& prepared,std::uint64_t expected_state_generation,
        std::stop_token stop={});
    // Publish same-membership Batch content with one selective compound item.
    // Common properties, current selection and unrelated owners remain live.
    void publish_vector_tool_content(DocumentData,const std::string&,std::uint64_t expected_generation,const std::function<bool()>& current,std::optional<std::array<std::int32_t,4>> raw_damage={},std::stop_token stop={},bool native_paint_history=false);
    void publish_vector_selection_content(DocumentData,bool history,bool storage_changed,const BatchContentTarget&,const std::string&,std::uint64_t expected_generation,std::stop_token stop={},const std::function<bool()>& current={});
    void publish_clear_menu_content(DocumentData,std::span<const BatchContentTarget>,std::uint32_t flags,
        std::uint64_t expected_generation,const std::function<bool()>& current,std::stop_token stop={},std::array<std::uint64_t,2> prior_costs={});
    void publish_batch_content(DocumentData&& prepared,std::span<const BatchContentTarget>,
        bool history,const std::string& label,std::stop_token stop={});
    // Settled active Dust: bind every consumed plane's original work ROI.
    // Positive native damage creates one typed ROI item; backing/prelude-only
    // publication keeps both history directions and latches plane dirtiness.
    // All snapshots, execution and history capacity prepare before publication.
    FillResult apply_dust_tool(const FilterToolSettings&,bool use_millimeters,
        const std::optional<Rect>& bounds={},const BinaryMask* mask=nullptr,
        std::stop_token stop={},const std::optional<RasterSelection>& selection={},
        const std::string& label="Dust");
    // Ordinary Dust without a cached preview. Reads the live selection carrier,
    // binds all children before the marked-target driver, and records typed ROI
    // history only for Applied. Normal Skipped/WorkerFailed retains prebinding
    // dirtiness and completed writes; cancellation and rejection are atomic.
    DustFilterResult apply_dust_filter(const DustSettings&,std::stop_token stop={},
        const std::string& label="Dust");
    // Capture immutable artwork/selection for private Dust computation. The
    // document must outlive its capture; run is owned by one worker at a time.
    DustFilterPreview capture_dust_filter_preview(std::stop_token stop={}) const;
    // Consume only a ready capture from this unchanged document. Cached mode
    // binds each raster child's own ROI and accepts normal driver1/2 as1219;
    // uncached mode retains the ordinary driver's binding and status policy.
    DustFilterResult commit_dust_filter_preview(DustFilterPreview&,std::stop_token stop={},
        const std::string& label="Dust");
    // Shared ordinary filter preparation and typed history.
    FilterPreview capture_filter_preview(FilterKind,std::stop_token stop={}) const;
    FilterPreview capture_batch_filter_preview(std::size_t owner,FilterKind,std::stop_token stop={},std::array<std::uint64_t,2> consumed={}) const;
private:
    FilterPreview capture_filter_preview_impl(FilterKind,std::stop_token,std::optional<std::size_t>,std::array<std::uint64_t,2> consumed={}) const;
public:
    DustFilterResult commit_filter_preview(FilterPreview&,std::stop_token stop={},
        const std::string& label="Filter");
    DustFilterResult apply_gaussian_filter(const GaussianBlurSettings&,std::stop_token stop={},
        const std::string& label="Gaussian blur");
    DustFilterResult apply_contrast_filter(const ContrastSettings&,std::stop_token stop={},
        const std::string& label="Contrast");
    DustFilterResult apply_level_filter(const LevelSettings&,std::stop_token stop={},
        const std::string& label="Level");
    DustFilterResult apply_tone_curve_filter(const ToneCurveSettings&,std::stop_token stop={},
        const std::string& label="Tone curve");
    DustFilterResult apply_auto_contrast_filter(std::stop_token stop={},
        const std::string& label="Auto contrast");
    DustFilterResult apply_hsv_filter(const HsvSettings&,std::stop_token stop={},
        const std::string& label="HSV");
    DustFilterResult apply_color_balance_filter(const ColorBalanceSettings&,std::stop_token stop={},
        const std::string& label="Color balance");
    DustFilterResult apply_unsharp_filter(const UnsharpMaskSettings&,std::stop_token stop={},
        const std::string& label="Unsharp mask");
    DustFilterResult apply_fixed_convolution_filter(FilterKind,std::stop_token stop={},
        const std::string& label="Fixed filter");
    // Paired content/selection-storage history for a prepared raster edit.
    // Requires nonempty compatible layer membership. Call after editing data;
    // the caller owns raster rollback if allocation fails. Selection publishes
    // only after the history slot is allocated; neither rollback snapshot is
    // consumed on failure. Immutable masks must already be privately frozen.
    // Replay preserves exact selection state (including4), bits and block
    // presence, plus ordinary PreserveCurrent content/property semantics.
    void commit_raster_selection(DocumentData&& before,const RegionSelection& before_selection,
        RegionSelection after_selection,const std::string& label);
    void set_text_plane(std::size_t layer,std::size_t plane,Bytes specific,Bytes name,Raster derived,const std::string& label);
    bool edit_layer_plane(LayerPlaneOperation,const ElementConvertOptions&,const std::string& label,bool batch=false);
    bool reset_plane_alpha_editing();
    bool set_adjust_parameters(std::size_t layer,std::size_t plane,const AdjustParameters&,const std::string& label);
    DustFilterResult execute_adjust_filter(const AdjustParameters&,AdjustFilterExecution,
        std::stop_token stop={},const std::string& label="Adjust filter");
    bool accept_adjust_settings(std::size_t layer,std::size_t plane,const AdjustParameters&,
        const std::string& label="Adjust settings");
    // Returns the helper BOOL. A normal false preserves all three current tables.
    bool bind_adjust_plane(std::size_t layer,std::size_t plane,std::stop_token stop={});
    void transact(const std::string& label,const std::function<void(DocumentData&)>& operation,HistorySelection selection=HistorySelection::RestoreSnapshot);
    // Requires settled artwork. The operation edits only its DocumentData,
    // without calling Document/history methods. Prepare history allocations
    // before running it. A requested stop restores content and retains history;
    // successful publication has no allocation after the final stop check.
    void transact(const std::string& label,const std::function<void(DocumentData&)>& operation,
        HistorySelection selection,std::stop_token stop);
    // Native deletion settles floating content, then toggles the selected objects.
    bool delete_layers(const std::vector<std::size_t>& indices,const std::string& label);
    bool delete_selected_layers(const std::string& label);
    bool delete_invisible_layers(const std::string& label);
    PlaneProperties plane_properties(std::size_t layer,std::size_t plane) const;
    bool can_edit_current_plane_properties() const noexcept;
    // Native Property reuses one item: changed color supersedes changed name.
    bool set_plane_properties(std::size_t layer,std::size_t plane,PlaneProperties properties,const std::string& label);
    LayerProperties layer_properties(std::size_t layer) const;
    // Native dialog writes every changed field but reuses one history item:
    // only the last changed field (mode > opacity > name) is replayed.
    bool set_layer_properties(std::size_t layer,LayerProperties properties,const std::string& label);
    // Insert one already-prepared layer without settling floating pixels.
    // History moves the same layer between document and detached ownership.
    void insert_layer(std::size_t index,Layer layer,const std::string& label);
    // SELECT_SELECT_TO_LAYER: append a canonical raster selection carrier as
    // one AddLayer item, preserving live region selection and floating pixels.
    // Materialized masks copy complete logical backing and block ownership;
    // lazy FullPlane/Rectangle rebuild their native fresh zero-padding surface.
    bool selection_to_layer(const std::string& label,std::stop_token stop={});
    // SELECT_LAYER_TO_SELECT: bind the current canonical raster selection
    // layer before floating settlement, then copy child0's selected-bit extent
    // into the live selection carrier with one grouped selection history item.
    bool combine_selection_layer(bool subtract,const std::string& label,std::stop_token stop={});
    bool layer_to_selection(const std::string& label,std::stop_token stop={});
    void insert_plane(std::size_t layer,std::size_t index,Plane plane,const std::string& label);
    bool can_delete_selected_planes() const noexcept;
    bool delete_selected_planes(const std::string& label);
    bool can_copy_current_plane() const noexcept;
    bool copy_current_plane(const std::string& label);
    // Complete destination-to-source permutation. Reorders live objects without
    // settling floating content; replay preserves current object properties.
    // Live ordering dirties the document; native order replay does not.
    void order_layers(const std::vector<std::size_t>& order,const std::string& label);
    void order_planes(std::size_t layer,const std::vector<std::size_t>& order,const std::string& label);
    // Move all nonzero selection markers as a stable group into a gap in the
    // original collection (0 before first, size after last).
    void move_selected_layers(std::size_t gap,const std::string& label);
    void move_selected_planes(std::size_t layer,std::size_t gap,const std::string& label);
    // Grid movement has history and truncates redo, but does not change the
    // ordinary document's saved-content token or serialize Grid into CEL.
    void commit_grid_move(std::array<int,2> before,const std::string& label);
    // Accept a live settings preview. Only a stored typed-origin change adds
    // history; spacing/subdivision changes keep the existing redo branch.
    void commit_grid_settings(const GridState& before,const std::string& label);
    void commit_guide_move(std::array<int,2> before,const std::string& label);
    // The creation item captures its opposite-direction record lazily at the
    // first Undo. Individual create/edit/delete commits change ordinary dirty.
    void commit_guide_created(std::size_t index,const std::string& label);
    void commit_guide_edit(const std::vector<GuideSnapshot>& before,GuideEditMode mode,const std::string& label);
    // Raw runtime replacement for a caller that manages its own transaction.
    // Unlike select_region(), this does not append history or alter content dirty.
    void set_region_selection(RegionSelection selection);
    // Every admitted native selection command records a selection-only item,
    // even for equal/no-op results. Deselect admission is checked by its caller.
    // Without floating content, publish native indications with the selection;
    // unpaired selection history recomputes them from current application flags.
    void select_region(RegionSelection selection,const std::string& label,bool refresh=true);
    // Generate a new selection against settled artwork, then publish its
    // selection and optional floating materialization as one history entry.
    // Candidate/allocation failure leaves this document unchanged.
    void edit_region_selection(const std::string& label,const std::function<RegionSelection(const DocumentData&,const RegionSelection&)>& operation,const bool* refresh=nullptr);
    // Public SELECTINVERT admission rejects Cleared without history. An admitted
    // call settles floating content and records the resulting selection in one
    // undo step; selection-only edits preserve ordinary content dirty state.
    bool invert_region_selection(const std::string& label);
    // States 0/1 reject without history; every admitted morphology command,
    // including radius zero, publishes through the shared selection edit path.
    bool morph_region_selection(SelectionMorphology,int radius_px,const std::string& label,std::stop_token stop={});
    // Public four-command query is wider than the branch-specific producers.
    // An admitted execution records Select(+optional floating) history even
    // for an unhandled plane high nibble or below32 raster ColorSpace.
    bool can_select_color(SelectionColorCommand) const noexcept;
    bool select_color(SelectionColorCommand,Pixel reference,const std::string& label,
        bool additive_space=false,bool minimum_width=false,std::stop_token stop={});
    // Current-raster COLOR/NOTCOLOR/COLORADD/COLORSPACE. Every admitted call,
    // including empty/below32 ColorSpace results, records selection history.
    bool select_raster_color(SelectionColorCommand,Pixel reference,const std::string& label,
        bool additive_space=false,std::stop_token stop={});
    // Explicit vector bitmap selection branch. Settles floating content and
    // records selection history through the same atomic command boundary.
    bool select_vector_color_bitmap(SelectionColorCommand,Pixel reference,const std::string& label,
        bool additive_space=false,bool minimum_width=false,std::stop_token stop={});
    //Native indication workers. No content or selection history is appended;
    //redraw reports old Doc+284 OR new hits, independent of resulting active.
    bool clear_vector_indications(std::stop_token stop={});
    bool refresh_vector_indications(double actual_carrier_dpi,std::stop_token stop={});
    // Native EDIT_COPY query for default LAYP=0. Execution currently admits
    // canonical General/Select/RasterIndicator/PaintMono raster holders. Unsupported
    // selected payloads reject atomically; no OS clipboard or history is used.
    bool can_copy_selection() const noexcept;
    bool can_copy_selection(const SelectionCopyOptions&) const noexcept;
    bool can_cut_clipboard(const SelectionCopyOptions&) const noexcept;
    // Null means query rejection. An admitted empty holder is non-null.
    std::shared_ptr<const SelectionCopyPacket> copy_selection(std::stop_token stop={});
    std::shared_ptr<const SelectionCopyPacket> copy_selection(const SelectionCopyOptions&,std::stop_token stop);
    std::shared_ptr<const SelectionCopyPacket> cut_clipboard(const SelectionCopyOptions&,const std::string& label,std::stop_token stop={});
    // Shared MoveImage=1 begin: selection + source cut + floating creation
    // form one undo step. Existing content instead records offset-only history.
    // False means no eligible source produced a floater; selection and any
    // admitted source content history are still recorded. Ordinary edits settle before modifying object topology.
    bool begin_floating_selection_move(const std::string& label);
    using FloatingPreviewValidator=std::function<void(const DocumentData&,std::shared_ptr<const FloatingSelection>)>;
    bool paste_clipboard(const SelectionCopyPacket&,bool same_document,const std::string& label,
        PasteSelection selection=PasteSelection::SourceFrame,const FloatingPreviewValidator& validate={},std::stop_token stop={});
    bool convert_paste(const SelectionCopyPacket&,const ConvertPasteOptions&,const std::string& label,
        PasteSelection selection=PasteSelection::SourceFrame,const FloatingPreviewValidator& validate={},std::stop_token stop={});
    // Supported normal cross-document paste. Preview and grouped history are
    // prepared before publication; typed layer creation waits for finalization.
    bool paste_foreign_floating(const FloatingSelection& exported,const std::string& label,const FloatingPreviewValidator& validate={},const FloatingTarget* target=nullptr);
    bool paste_targeted_floating(const FloatingSelection&,FloatingTarget,const std::string& label,const FloatingPreviewValidator& validate={});
    // Content selection uses the actual helper mask. Empty content still
    // records the selection edit and optional prior-floating settlement, but
    // creates no new floater and does not invoke its preview validator.
    bool paste_foreign_floating(const FloatingSelection&,const std::string& label,PasteSelection,
        const FloatingPreviewValidator& validate={},const FloatingTarget* target=nullptr);
    bool paste_targeted_floating(const FloatingSelection&,FloatingTarget,const std::string& label,PasteSelection,
        const FloatingPreviewValidator& validate={});
    // Validate a staged preview before changing pixels/history. Validators
    // are read-only; reentrant Document API mutations invalidate publication.
    bool begin_floating_selection_move(const std::string& label,const FloatingPreviewValidator&);
    // Update both shared selection geometry and the live payload delta. The
    // caller owns capture/release state and selection-state normalization.
    void move_floating_selection(int dx,int dy);
    void move_floating_selection(int dx,int dy,const FloatingPreviewValidator&);
    // Ordinary materialization records mode1 history without changing selection.
    // It remains available after the capture item leaves the bounded history.
    void finalize_floating_selection(const std::string& label="确定浮动图像");
    // Clipboard CUT removes a live payload without painting it back. A mode-1
    // history item restores the payload on undo; export is owned by the caller.
    void discard_floating_selection(const std::string& label="剪切浮动图像");
    // No-live CUT groups prepared source removal and selection clearing into
    // one undo step. A missing payload still clears an active selection.
    bool cut_selection(std::optional<DocumentData> remainder,const std::string& label="剪切");
    // Merge newer selection and visibility into a staged copy of this same
    // document. The caller guarantees unchanged object membership/order.
    void adopt_current_selection(const Document& current);
    // Layer/plane row-eye changes are serialized and dirty, but do not own
    // history, truncate redo or settle floating content. Target activation
    // can reveal a hidden node through the same setter without marking dirty.
    bool set_visibility(std::size_t layer,std::optional<std::size_t> plane,bool visible,bool mark_modified=true);
    // Executes a cached native GrayView token without history or dirty state.
    //0 is a disabled no-op;101/102/201/202 are hexadecimal native tokens.
    bool execute_gray_view(std::uint32_t token);
    MovePlaneOutcome move_plane(std::stop_token stop={});
    //6AF3E0 writes the raw flag and latches dirty even when it was equal.
    void set_batch_layer_visibility(std::size_t layer,std::int32_t value);
    void adopt_current_runtime(const Document& current);
    // For immediate, noexcept publication of a prepared document after I/O:
    // move the latest runtime into it without allocating a post-write copy.
    void swap_runtime(Document& other);
    bool undo_next() const noexcept {return undo_next_;}
    void undo_or_redo();
    void undo();
    void redo();
    void save(const std::filesystem::path& destination,SavePolicy policy=SavePolicy::LegacyCanonical,std::optional<int> compression_level={},bool v5_compatible=false);
    // Called only after a writer has atomically committed. Prepare the optional
    // COW content baseline before I/O; omission disables ExactRaw.
    void mark_saved(std::filesystem::path destination,std::shared_ptr<const Bytes> original,std::shared_ptr<const DocumentData> baseline={});
};
}
