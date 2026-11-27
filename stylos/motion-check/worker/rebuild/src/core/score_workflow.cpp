#include "retas/score_workflow.hpp"
#include "retas/batch_image_codec.hpp"
#include "../io/native_ascii_string.hpp"

namespace retas {
ScoreNumber score_cut_number(std::span<const std::uint8_t> input){
    input=native_ascii_detail::c_prefix(input);if(input.size()>65535)throw std::length_error("Score Cut number text exceeds65535 bytes");
    ScoreNumber out{};std::uint32_t value{};std::size_t i{};
    for(;i<input.size()&&input[i]>='0'&&input[i]<='9';++i){
        const auto digit=unsigned(input[i]-'0');if(value>(2147483647u-digit)/10)throw std::out_of_range("Score Cut number exceeds int32");value=value*10+digit;
    }
    if(!i||!value||value>9999||input.size()-i>3)return {};
    const auto suffix=i;
    for(;i<input.size();++i){auto c=input[i];if(c>='a'&&c<='z')c-=32;if(c<'A'||c>'Z')return {};out[4+i-suffix]=c;}
    for(int b=0;b<4;++b)out[std::size_t(b)]=std::uint8_t(value>>(24-b*8));return out;
}
CutFolderScoreCreation create_score_cut_folder(const std::filesystem::path& parent,
    const CutFolderNewSettings& supplied,const CutFolderScoreOptions& options,
    CutFolderRootLoadSettings load,std::stop_token stop){
    // Freeze caller-owned strings/options before the first publication.
    const auto settings=supplied;const auto accepted=options;
    auto root=CutFolderRoot::create(parent,settings,stop);if(!root)return {};
    CutFolderScoreCreation result;result.created=true;result.directory=root->directory();result.metadata_path=root->archive_path();
    root->close(stop);
    if(!accepted.create_score)return result;
    result.score_path=result.directory/(result.directory.filename().string()+".sxf");
    try {
        ScoreGeneral general;general.frame_count=accepted.frame_count;general.layer_count=settings.layout.layer_count;general.fps=24;
        general.frame_display=8;general.guide_line=6;general.page=144;general.settings.resize(6);general.visibility.resize(7);
        general.scene=score_cut_number(settings.metadata.text_1c);general.cut=score_cut_number(settings.metadata.text_28);
        ScoreWorkflow workflow(ScoreDocument::create(std::move(general),accepted.add_background),std::move(load));
        workflow.save(result.score_path,stop);result.score_created=true;workflow.close(stop);
    }catch(const std::bad_alloc&){throw;}
     catch(const std::length_error&){throw;}
     catch(const std::exception& e){if(stop.stop_requested())throw;result.score_error=e.what();}
    return result;
}
struct ScoreWorkflow::State {
    ScoreDocument document;
    CutFolderRootLoadSettings settings;
    CutFolderRootManager manager;
    CutFolderRoot* root{};
    std::string error;
    bool default_model_root{};
    bool closed{};
    State(ScoreDocument d,CutFolderRootLoadSettings s)
        :document(std::move(d)),settings(std::move(s)) {
        default_model_root=settings.model_root.empty();
        if(settings.image_extensions.empty())
            for(const auto& codec:default_batch_image_codecs().formats)
                if(codec.decode)for(const auto& extension:codec.extensions)
                    settings.image_extensions.push_back(extension);
    }
};
ScoreWorkflow::ScoreWorkflow(ScoreDocument d,CutFolderRootLoadSettings s)
    :state_(std::make_unique<State>(std::move(d),std::move(s))) {}
ScoreWorkflow::~ScoreWorkflow()=default;
ScoreWorkflow::ScoreWorkflow(ScoreWorkflow&&) noexcept=default;
ScoreWorkflow ScoreWorkflow::open(const std::filesystem::path& path,
    CutFolderRootLoadSettings settings,std::stop_token stop,ScoreSoundPathEncoding encoding) {
    if(stop.stop_requested())throw std::runtime_error("Score open cancelled");
    ScoreWorkflow workflow(ScoreDocument::open(path,encoding),std::move(settings));
    workflow.refresh_cut_folder(stop);
    return workflow;
}
ScoreDocument& ScoreWorkflow::document(){if(!state_)throw std::logic_error("Moved Score workflow");return state_->document;}
const ScoreDocument& ScoreWorkflow::document()const{if(!state_)throw std::logic_error("Moved Score workflow");return state_->document;}
const CutFolderRoot* ScoreWorkflow::cut_folder()const{return state_?state_->root:nullptr;}
const std::string& ScoreWorkflow::association_error()const{if(!state_)throw std::logic_error("Moved Score workflow");return state_->error;}
void ScoreWorkflow::refresh_cut_folder(std::stop_token stop) {
    if(!state_||state_->closed)throw std::logic_error("Closed Score workflow");
    auto& s=*state_;
    if(s.root)s.manager.release(s.root,stop);
    s.error.clear();
    if(stop.stop_requested())throw std::runtime_error("Score association cancelled");
    const auto& path=s.document.path();
    if(path.empty())return;
    try {
        if(!std::filesystem::exists(path))return;
        auto settings=s.settings;
        if(s.default_model_root){
#ifdef _WIN32
            settings.model_root=path.parent_path().string();
#else
            // Virtual native spelling maps to this metadata parent through
            // the existing portable model-path adapter.
            settings.model_root="C:\\ScoreCut";
#endif
        }
        CelFolderBudget budget;
        const auto admitted=admit_cut_folder_root(path.parent_path(),settings,budget,stop);
        if(admitted.kind!=1&&admitted.kind!=2){s.error=admitted.error;return;}
        s.root=s.manager.acquire(admitted.selected_path,settings,budget,stop);
        s.root->expand_children(0,{settings.image_extensions,{},settings.metadata_encoding},budget,stop);
    } catch(const std::bad_alloc&) {throw;}
      catch(const std::length_error&) {throw;}
      catch(const std::exception& e) {
        // A failed last release remains owned and retryable. Do not conceal a
        // publication failure behind the best-effort admission diagnostic.
        if(s.root)s.manager.release(s.root);
        if(stop.stop_requested())throw;
        s.error=e.what();
    }
}
void ScoreWorkflow::save(const std::filesystem::path& path,std::stop_token stop) {
    if(!state_||state_->closed)throw std::logic_error("Closed Score workflow");
    const auto previous=state_->document.path();
    state_->document.save(path,[stop]{return stop.stop_requested();});
    if(previous!=state_->document.path())refresh_cut_folder(stop);
}
void ScoreWorkflow::save_as(const std::filesystem::path& path,std::stop_token stop) {
    if(!state_||state_->closed)throw std::logic_error("Closed Score workflow");
    state_->document.save(path,[stop]{return stop.stop_requested();});
    refresh_cut_folder(stop);
}
void ScoreWorkflow::save_interchange(const std::filesystem::path& path,std::stop_token stop) {
    if(!state_||state_->closed)throw std::logic_error("Closed Score workflow");
    state_->document.save_interchange(path,[stop]{return stop.stop_requested();});
    refresh_cut_folder(stop);
}
void ScoreWorkflow::close(std::stop_token stop) {
    if(!state_||state_->closed)return;
    if(state_->root)state_->manager.release(state_->root,stop);
    state_->closed=true;
}
}
