#pragma once
#include <optional>

namespace retas {
struct ViewPoint {
    int x{},y{};
    bool operator==(const ViewPoint&) const = default;
};
struct ContinuousViewPoint {
    double x{},y{};
    bool operator==(const ContinuousViewPoint&) const = default;
};
// The original shared tool mapper truncates raw client coordinates before
// LYMV's separate integer document mapping. Invalid input is rejected.
ViewPoint truncate_view_point(double x,double y);

struct FixedViewMatrix {
    int xx{4096},xy{},yx{},yy{4096},tx{},ty{};
    bool operator==(const FixedViewMatrix&) const = default;
};
struct NativePointMap {
    int zoom_units{4096};
    ViewPoint scroll,center;
    FixedViewMatrix matrix;
    // sub_454C30 uses wrapping int32 multiply/add, arithmetic >>12, then
    // rtMulDiv. Do not replace the fixed matrix with a floating inverse.
    ViewPoint map(ViewPoint client) const;
    // sub_454DC0 retains floating fractions throughout the fixed-matrix
    // calculation. Guide creation rounds only after the complete projection.
    ContinuousViewPoint map_continuous(ContinuousViewPoint client) const;
    ViewPoint guide_point(ViewPoint raw_client,ViewPoint drawable_origin) const;
};

struct NativeViewOrientation {
    int degrees{};bool mirror_x{},mirror_y{},special_center{};
    bool operator==(const NativeViewOrientation&) const = default;
};
// Ordinary PaintMan viewport, with its quantized rotation/flip matrix and
// centering mode. Coordinates are relative to the drawable client rectangle.
// This models native geometry; it does not change Canvas's anchored zoom or
// fractional reference-view synchronization policy.
class NativeViewport {
public:
    struct Axis {
        int page{},viewport{},scaled_page{},center{},scroll{},max_scroll{},focus{};
        bool operator==(const Axis&) const = default;
    };
private:
    Axis x_,y_;
    int zoom_;
    NativeViewOrientation orientation_;
    FixedViewMatrix forward_,inverse_;
    void rebuild();
    void clamp_scroll();
    void refresh_focus();
public:
    // zoom_units is an already normalized positive [+290] integer state;
    // user-request clamping belongs to the caller's zoom policy.
    NativeViewport(ViewPoint page,ViewPoint viewport,int zoom_units=4096,NativeViewOrientation orientation={});
    const Axis& horizontal() const {return x_;}
    const Axis& vertical() const {return y_;}
    int zoom_units() const {return zoom_;}
    ViewPoint origin() const {return {x_.center-x_.scroll,y_.center-y_.scroll};}
    ViewPoint scroll() const {return {x_.scroll,y_.scroll};}
    ViewPoint focus() const {return {x_.focus,y_.focus};}
    // Set geometry from the saved integer focus; clamping then refreshes it.
    void resize(ViewPoint viewport);
    void set_orientation(NativeViewOrientation);
    const FixedViewMatrix& forward_matrix() const {return forward_;}
    ContinuousViewPoint project_document(ContinuousViewPoint) const;
    void scroll_to(ViewPoint position);
    void scroll_by(ViewPoint delta);
    void drag_from(ViewPoint initial_scroll,ViewPoint start,ViewPoint current);
    // Native point zoom recenters the document anchor at the viewport center.
    // With no anchor it uses the saved focus. User-anchored Canvas zoom is a
    // separate policy and must not silently call this method.
    // The optional client anchor is first mapped to a document point, then
    // passed to the native document-anchor recenter operation.
    void set_zoom(int zoom_units,std::optional<ViewPoint> client_anchor={});
    ViewPoint document_point(ViewPoint client) const;
    ViewPoint client_point(ViewPoint document) const;
    // sub_454640: snap's integer forward projection rounds the scale first,
    // then applies the wrapping fixed matrix and the integer view origin.
    ViewPoint snap_client_point(ViewPoint document) const;
    // sub_454C30: integer add/subtract, followed by rtMulDiv. This is distinct
    // from the floating document-point mapper used to refresh focus.
    ViewPoint layer_move_point(ViewPoint client) const;
    NativePointMap layer_move_mapper() const {return {zoom_,scroll(),{x_.center,y_.center},inverse_};}
    bool operator==(const NativeViewport&) const = default;
};
}
