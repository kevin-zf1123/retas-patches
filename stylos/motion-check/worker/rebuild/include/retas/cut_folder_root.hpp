#pragma once
#include "mn6_archive.hpp"
#include "model_thumbnail.hpp"
#include "cut_folder.hpp"
#include <filesystem>
#include <memory>
#include <optional>

namespace retas {
// The New CutFolder source owns these fields. RootInfo.field_34 stays24;
// kind belongs to creation, while field_38 and field_54 are copied verbatim.
struct CutFolderNewMetadata {
    Mn6BaseInfo paper;
    Bytes paper_name,text_04,text_10,text_1c,text_28,text_48;
    std::int32_t field_38{},field_54{1};
    CutFolderNewMetadata();
};
struct CutFolderLayout {
    std::int32_t layer_count{},omit_background{};
    // kind2 consumes indices0..<layer_count+1-(omit_background!=0).
    // Empty arrays mean all zero; otherwise each must cover that domain.
    std::vector<std::int32_t> key,inbetween;
};
struct CutFolderNewSettings {
    std::uint16_t kind{1};
    std::string name;
    std::int32_t name_kind{};
    CutFolderNewMetadata metadata;
    std::array<Bytes,9> category_names;
    CutFolderLayout layout;
    // Encoding of user-entered metadata, including category and model notes.
    // Category resource labels and model paths have separate ASCII profiles.
    NativePreferenceEncoding metadata_encoding{};
};
// Static native ASCII helpers. text_04 is the unformatted prefix; numeric
// parts in text_10/1c/28 use C-prefixes and zero-padding3/3/4. Decimal overflow
// rejects. Layer labels use
// the recovered two-byte algorithm, including703="[A" (not Excel AAA).
std::string cut_folder_name(std::int32_t kind,const CutFolderNewMetadata&);
std::string cut_folder_layer_name(std::int32_t index);
// Decode only the RootInfo fields used by New. This is field projection,
// without root admission, model binding or any image/filesystem access.
CutFolderNewMetadata project_cut_folder_new_metadata(const Mn6RootInfo&,
    std::stop_token stop={});
struct CutFolderModelLocation {
    std::string native_path;
    std::filesystem::path host_path;
};
struct CutFolderRootLoadSettings {
    std::array<Bytes,9> category_names;
    // Win32 ASCII spelling used by archive reference arithmetic. On portable
    // hosts this root maps to the metadata file's parent directory.
    std::string model_root;
    std::vector<CutFolderModelLocation> model_locations;
    // Explicit class1/flag0x10 registry extensions, without leading dots.
    std::vector<std::string> image_extensions;
    // Encoding of user-entered metadata, including category and model notes.
    // Category resource labels and model paths have separate ASCII profiles.
    NativePreferenceEncoding metadata_encoding{};
    bool read_only{}; // Background rendering must never publish project metadata.
};
struct CutFolderModelBinding {
    std::string native_path;
    std::filesystem::path host_path;
    bool image{},layered{},generated{},write_suppressed{};
    std::optional<ModelThumbnail> thumbnail;
};
struct CutFolderModelFailure {std::size_t source_index{};std::string error;};
struct CutFolderAdmission {
    std::uint16_t kind{};
    std::filesystem::path selected_path;
    std::string error;
};
struct CutFolderChildView {
    const std::filesystem::path& path;
    std::uint16_t flags;
    const CelFolder& owner;
};
// Last visible MN6 wins over last MNG. MN6 runs a complete temporary load;
// MNG-only admission returns2 without reading it. Load may write thumbnails.
CutFolderAdmission admit_cut_folder_root(const std::filesystem::path& directory,
    const CutFolderRootLoadSettings&,std::stop_token stop={});
CutFolderAdmission admit_cut_folder_root(const std::filesystem::path& directory,
    const CutFolderRootLoadSettings&,CelFolderBudget&,std::stop_token stop={});

class CutFolderRoot {
    struct Child {
        std::filesystem::path path;
        std::uint16_t flags;
        CelFolder owner;
    };
    std::filesystem::path root_,archive_path_,management_path_;
    Mn6Archive archive_;
    std::string model_root_;
    NativePreferenceEncoding metadata_encoding_{};
    std::vector<CutFolderModelBinding> models_;
    std::vector<CutFolderModelFailure> model_failures_;
    // Layout, Key Animation, Inbetween, BG. kind2 uses only Inbetween.
    std::array<std::vector<std::unique_ptr<Child>>,4> children_;
    std::vector<MngFolderFailure> child_failures_;
    bool children_active_{},read_only_{};
    std::uint64_t children_revision_{};
    bool dirty_{},closed_{},write_suppressed_{};
    CutFolderRoot()=default;
    void validate_candidate(const Mn6Archive&)const;
    void clear_children_selected(std::uint32_t mask,std::stop_token);
public:
    CutFolderRoot(const CutFolderRoot&)=delete;
    CutFolderRoot& operator=(const CutFolderRoot&)=delete;
    CutFolderRoot(CutFolderRoot&&)noexcept;
    CutFolderRoot& operator=(CutFolderRoot&&)noexcept;
    // Existing targets (including ordinary files) are a no-op: nullopt.
    // A new root publishes its directories, MN6 and kind2 companion MNG.
    // Failure attempts removal only of the root created by this call.
    static std::optional<CutFolderRoot> create(const std::filesystem::path& parent,
        const CutFolderNewSettings&,std::stop_token stop={});
    // Fresh owner; only .mng selects management loading. Other suffixes are
    // parsed as CT06. No CF06 expansion occurs during root load.
    static CutFolderRoot load(const std::filesystem::path& metadata,
        const CutFolderRootLoadSettings&,std::stop_token stop={});
    static CutFolderRoot load(const std::filesystem::path& metadata,
        const CutFolderRootLoadSettings&,CelFolderBudget&,std::stop_token stop={});
    const std::filesystem::path& directory()const{return root_;}
    const std::filesystem::path& archive_path()const{return archive_path_;}
    const std::filesystem::path& management_path()const{return management_path_;}
    const Mn6Archive& archive()const{return archive_;}
    NativePreferenceEncoding metadata_encoding()const{return metadata_encoding_;}
    std::string_view native_model_root()const{return model_root_;}
    const std::vector<CutFolderModelBinding>& model_bindings()const{return models_;}
    const std::vector<CutFolderModelFailure>& model_failures()const{return model_failures_;}
    // Low16 mask: zero selects all, otherwise bits1/2/4/8 select
    // Layout/BG/Key/Inbetween. kind2 ignores mask. Hits never reopen CF06.
    void expand_children(std::uint32_t mask=0,const MngCatalogOptions& options={},
        std::stop_token stop={});
    void expand_children(std::uint32_t mask,const MngCatalogOptions&,
        CelFolderBudget&,std::stop_token stop={});
    // Views borrow stable heap owners until pruning, clear, Close or destruction.
    std::size_t child_count(std::uint32_t mask=0)const;
    std::vector<CutFolderChildView> child_folders(std::uint32_t mask=0)const;
    const std::vector<MngFolderFailure>& child_failures()const{return child_failures_;}
    // Close each selected child before removing it. Failures retain that child
    // and later children for retry; completed removals remain completed.
    void clear_children(std::uint32_t mask=0,std::stop_token stop={});
    bool dirty()const{return dirty_;}
    bool write_suppressed()const{return write_suppressed_;}
    void set_metadata(const CutFolderNewMetadata&,std::stop_token stop={});
    void set_category_progress(std::size_t group,std::size_t row,Bytes name,
                               std::optional<std::uint32_t> date);
    void append_category_check(std::size_t group,std::size_t row,Mn6CategoryChild check);
    // New directions are inserted first. Editing retains the original ID and time.
    void set_direction(std::optional<std::size_t> row,Mn6Model model,
                       const CutFolderRootLoadSettings&,std::uint32_t now);
    // Explicit Save writes even when clean. Read-only primary MN6 clears
    // dirty and suppresses both writes. MN6 publishes before MNG: a later
    // MNG failure retains the new MN6 and the prior dirty state.
    void save(std::stop_token stop={});
    // Save root metadata only when dirty, then Close/remove cached children.
    // Retryable after failure, idempotent on success; destructor performs no I/O.
    void close(std::stop_token stop={});
};
// Explicit reference manager. Returned pointers borrow stable heap owners;
// callers may edit/expand them, but leave moving and closing to this manager.
// The manager and its destructor perform no implicit filesystem publication.
class CutFolderRootManager {
    struct Entry {std::unique_ptr<CutFolderRoot> root;std::uint32_t references;};
    std::vector<Entry> roots_;
public:
    CutFolderRootManager()=default;
    CutFolderRootManager(const CutFolderRootManager&)=delete;
    CutFolderRootManager& operator=(const CutFolderRootManager&)=delete;
    CutFolderRoot* acquire(const std::filesystem::path& metadata,
        const CutFolderRootLoadSettings&,std::stop_token stop={});
    CutFolderRoot* acquire(const std::filesystem::path& metadata,
        const CutFolderRootLoadSettings&,CelFolderBudget&,std::stop_token stop={});
    // A recognized release clears the caller's pointer. Last release closes
    // first; failure retains one reference and the pointer for retry.
    bool release(CutFolderRoot*&,std::stop_token stop={});
    std::uint32_t references(const CutFolderRoot*)const noexcept;
    std::size_t size()const noexcept{return roots_.size();}
    // Close all entries in acquisition order, irrespective of reference count.
    // Completed removals stay completed; unfinished owners remain on failure.
    void close(std::stop_token stop={});
};
}
