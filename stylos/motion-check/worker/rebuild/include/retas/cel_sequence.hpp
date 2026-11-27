#pragma once
#include "batch_image_codec.hpp"
#include "file_workflow.hpp"

namespace retas {
class CelSequenceSourceChanged : public std::logic_error {public:using std::logic_error::logic_error;};
enum class CelNavigation {Unchanged,Moved,Cancelled,Unavailable};
enum class CelBatchPreparation {Ready,Cancelled,Failed};
struct CelSequenceProviders {
    // Nullopt is an ordinary Open failure; exceptions remain exceptions.
    std::function<std::optional<BatchDecodedImage>(const std::filesystem::path&,std::stop_token)> load;
    // Called only for a modified live document. Absence never means Discard.
    std::function<CloseSaveResponse(const Document&)> settle;
    // Publish first, then return the exact saved bytes. Nullopt means Cancel.
    std::function<std::optional<Bytes>(const std::filesystem::path&,const DocumentData&,const BatchImageContext&,std::stop_token)> save;
};
// One live CEL window expressed without widgets. Navigation replaces its
// document only after dirty settlement and successful loading. Paths and
// provider handles are immutable for the lifetime of the sequence.
class CelSequence {
    std::vector<std::filesystem::path> paths_;
    std::size_t index_{};
    std::unique_ptr<Document> current_;
    BatchImageContext context_;
    CelSequenceProviders providers_;
    bool entered_{};
    bool save_impl(std::stop_token);
public:
    class CurrentLease {
        friend class CelSequence;
        CelSequence* sequence_;
        explicit CurrentLease(CelSequence&);
    public:
        CurrentLease(const CurrentLease&)=delete;
        CurrentLease& operator=(const CurrentLease&)=delete;
        CurrentLease(CurrentLease&&)noexcept;
        ~CurrentLease();
        Document& document()const noexcept;
    };
    // Reject navigation, reload and Save callbacks while a caller borrows the
    // live document. Release this lease before invoking save_current().
    CurrentLease borrow_current();
    CelSequence(std::vector<std::filesystem::path>,std::size_t current,
        BatchDecodedImage,CelSequenceProviders);
    const std::vector<std::filesystem::path>& paths()const noexcept{return paths_;}
    std::size_t index()const noexcept{return index_;}
    Document& document()noexcept{return *current_;}
    const Document& document()const noexcept{return *current_;}
    const BatchImageContext& context()const noexcept{return context_;}
    std::optional<BatchDecodedImage> load_frame(std::size_t,std::stop_token={})const;
    CelNavigation navigate(std::size_t,std::stop_token={},bool inherit_plane_state=false);
    bool save_current(std::stop_token={});
    // Source2's first forced close check: Save keeps the live document;
    // Discard reloads the current file; an already clean document stays live.
    CelBatchPreparation prepare_batch(std::stop_token={});
    bool reload_current(std::stop_token={});
};
struct CelPreviewResult {std::size_t navigated{};bool cancelled{},unavailable{};};
// CEL_PREVIEW20012: immediate next/wrap navigation, no timer and no cache.
// The pump is called after each successful navigation; false requests Exit.
// A bounded step count is required by headless callers.
CelPreviewResult run_cel_preview(CelSequence&,std::size_t maximum_steps,
    const std::function<bool(const CelSequence&)>& pump={},std::stop_token={});
}
