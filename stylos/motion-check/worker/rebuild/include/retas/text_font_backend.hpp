#pragma once
#include <cstddef>
#include <memory>
#include <stop_token>

namespace retas {
struct DocumentData;
// Build capability only. The Qt offscreen platform and fonts must also be
// installed. No Qt types, widgets or persistent font data enter this API.
bool text_font_backend_available() noexcept;

// Construct, use and destroy on the application main thread. Reuses an existing
// QGuiApplication, or owns an offscreen application until the last backend dies.
// An existing QCoreApplication is insufficient and is rejected.
// Standalone OS-main-thread bootstrap is provided on macOS/Linux; other hosts
// require a previously initialized Qt main thread. Keep borrowed GUI apps alive.
class TextFontBackend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::size_t prepare_impl(DocumentData&,std::stop_token,bool image) const;
public:
    TextFontBackend();
    ~TextFontBackend();
    TextFontBackend(const TextFontBackend&)=delete;
    TextFontBackend& operator=(const TextFontBackend&)=delete;
    // Prepare every nonopaque Text child, including hidden ones. All cache
    // pointers publish together; failure/cancellation preserves the input.
    // The caller keeps the document stable/exclusive throughout preparation;
    // callbacks must not mutate it or destroy the application through reentry.
    // Only ordinary Text/Indication with children at document DPI and0..100%
    // owner density are admitted. Owner DPI must be finite/positive but does
    // not affect Text geometry. Native Text ignores child density/mode/fixed
    // color and owner mode at its glyph terminal; Preview retains stack gates.
    std::size_t prepare(DocumentData&,std::stop_token={}) const;
    // Ordinary image/CEL-thumbnail placement at root scale1 also admits a
    // finite positive child DPI different from the document. Such caches
    // bind raw owner XY and must be prepared again after an owner move.
    // PreviewScene applies its separate zero-placement bitmap adapter.
    // MotionCheck regenerates private positive-placement masks through its
    // explicit provider. LAYP uses ordinary colored Text; print regenerates
    // its own masks through the independent print-image consumer.
    std::size_t prepare_image(DocumentData&,std::stop_token={}) const;
};
}
