#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "../src/frame_cache.h"
#include "../src/cache_space.h"
#include "retas/score_motion_check.hpp"
#include <zlib.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

void limit_renderer_memory(unsigned mib);
namespace fs=std::filesystem;
using Hash=std::array<BYTE,32>;

static Hash fingerprint(std::span<const std::uint8_t> bytes){
    Hash result{};
    if(bytes.size()>MAXDWORD||!sha(bytes.data(),DWORD(bytes.size()),result.data()))
        throw std::runtime_error("Cannot hash renderer input");
    return result;
}
static Hash fingerprint_file(const fs::path& file){
    Hash result{};
    if(!file_digest(file.c_str(),result.data()))throw std::runtime_error("Cannot fingerprint project file");
    return result;
}
static std::string write_frame(const fs::path& out,DWORD frame,const retas::Raster& image,
                        const std::vector<Hash>& inputs,const Hash& preferences){
    BYTE key[32];wchar_t name[72];
    if(inputs.size()>MaxFrameInputs||!worker_frame_key(frame,image.width,image.height,preferences.data(),
       reinterpret_cast<const BYTE(*)[32]>(inputs.data()),DWORD(inputs.size()),key))
        throw std::runtime_error("Cannot name frame");
    worker_frame_name(key,name);
    fs::path path=out/name;
    const auto bytes=size_t(image.width)*image.height*4;
    ULARGE_INTEGER free_space{};
    if(!GetDiskFreeSpaceExW(out.c_str(),&free_space,nullptr,nullptr)||free_space.QuadPart<128ULL*1024*1024+bytes)
        throw std::runtime_error("Insufficient frame cache space");
    std::vector<BYTE> pixels(bytes);
    for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x){
        auto p=image.pixel(x,y);auto* row=pixels.data()+(size_t(y)*image.width+x)*4;
        row[0]=BYTE(p.b>>8);row[1]=BYTE(p.g>>8);row[2]=BYTE(p.r>>8);row[3]=0;
    }
    Header header{FrameZlib,DWORD(image.width),DWORD(image.height),0,{}};
    auto digest=fingerprint(pixels);std::copy(digest.begin(),digest.end(),header.digest);
    auto capacity=compressBound(static_cast<uLong>(pixels.size()));std::vector<BYTE> packed(capacity);
    if(compress2(packed.data(),&capacity,pixels.data(),static_cast<uLong>(pixels.size()),1)!=Z_OK)
        throw std::runtime_error("Frame compression failed");
    header.size=capacity;
    if(!store_cache_file(out.c_str(),path.c_str(),&header,sizeof(header),packed.data(),DWORD(capacity)))
        throw std::runtime_error("Cannot publish frame within the disk budget");
    return path.filename().string();
}
int wmain(int argc,wchar_t** argv){
    if(argc!=4&&argc!=5){std::cerr<<"Usage: StylosRender64.exe scene.sxf cache-dir preference-dir [parent-pid]\n";return 2;}
    try{
        auto config=render_settings();
        limit_renderer_memory(config.memory_mib);
        if(argc==5){
            HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,std::stoul(argv[4]));
            if(!parent)return 2;
            std::thread([parent]{if(WaitForSingleObject(parent,INFINITE)==WAIT_OBJECT_0)ExitProcess(0);CloseHandle(parent);}).detach();
        }
        const fs::path scene=fs::absolute(argv[1]),out=argv[2],prefs=argv[3];
        std::vector<fs::path> files{scene};auto meta=scene;meta.replace_extension(L".mn6");
        if(fs::exists(meta))files.push_back(meta);
        for(auto leaf:{L"AppColor",L"AppVecSetting"}){const auto f=prefs/L"Data"/leaf/L"pref.pff";if(fs::exists(f))files.push_back(f);}
        std::vector<Hash> base;for(const auto& f:files)base.push_back(fingerprint_file(f));
        const auto settings_file=prefs/L"Data"/L"MotionCheckDlg"/L"pref.pff";
        retas::MotionCheckSettings settings;Hash settings_hash{};
        if(fs::exists(settings_file)){
            auto bytes=retas::read_file(settings_file);
            settings=retas::load_stylos_motion_preferences(retas::decode_preference_archive(bytes));
        }
        retas::CutFolderRootLoadSettings load;load.read_only=true;load.model_root="C:\\ScoreCut";
        load.metadata_encoding.code_page=GetACP();
        auto project=retas::ScoreWorkflow::open(scene,load);
        if(!project.cut_folder())throw std::runtime_error(project.association_error());
        retas::ScoreMotionCheckSession plan(project.document(),*project.cut_folder(),settings,{},
            retas::default_batch_image_codecs(),{},{},retas::ScoreMotionRenderer::Stylos260);
        const auto geometry=plan.unscaled_size();
        if(!worker_settings_key(settings.scale,settings.background,settings.show_score_layer,settings.darken,
                                settings.show_shadow_layer,settings.show_all,geometry[0],geometry[1],settings_hash.data()))
            throw std::runtime_error("Cannot fingerprint rendering settings");
        const auto count=project.document().data().general.frame_count;
        std::vector<DWORD> representatives;representatives.reserve(count);
        for(int i=0;i<count;++i)representatives.push_back(DWORD(plan.representative(i)));
        project.close();
        for(size_t i=0;i<files.size();++i)if(fingerprint_file(files[i])!=base[i])throw std::runtime_error("Project changed while opening");
        fs::create_directories(out);
        wchar_t map_name[72];
        if(!worker_map_name(base.front().data(),settings_hash.data(),map_name))throw std::runtime_error("Cannot name exposure map");
        const auto map=out/map_name;const DWORD map_size=DWORD(representatives.size()*sizeof(DWORD));Hash check{};
        if(!sha(representatives.data(),map_size,check.data())||
           !store_cache_file(out.c_str(),map.c_str(),check.data(),DWORD(check.size()),representatives.data(),map_size))
            throw std::runtime_error("Cannot publish exposure map");
        const auto frames=plan.unique_frames();std::atomic_size_t next{};std::atomic_bool failed{};
        std::mutex log;std::vector<std::jthread> pool;
        const unsigned cpus=render_cpus();
        const unsigned requested=config.threads?config.threads:std::min(4u,cpus);
        const auto threads=std::min<size_t>({frames.size(),requested,std::max(1u,unsigned(config.memory_mib)/128u)});
        std::cerr<<"Renderer limits: threads="<<threads<<" memoryMiB="<<config.memory_mib<<" diskMiB="<<config.disk_mib<<"\n";
        std::vector<size_t> retry;retry.reserve(frames.size());
        const auto render=[&](size_t n){
            auto inputs=base;
            auto pixels=plan.render_frame(frames[n],[&](const fs::path&,std::span<const std::uint8_t> bytes){
                if(inputs.size()>=MaxFrameInputs)throw std::length_error("Too many frame inputs");
                inputs.push_back(fingerprint(bytes));
            });
            const auto name=write_frame(out,DWORD(frames[n]),pixels,inputs,settings_hash);
            std::lock_guard guard(log);std::cout<<"Frame "<<frames[n]<<" "<<name<<"\n";
        };
        for(size_t i=0;i<threads;++i)pool.emplace_back([&]{
            while(!failed){
                const auto n=next.fetch_add(1);if(n>=frames.size())break;
                try{render(n);}
                catch(const std::bad_alloc&){std::lock_guard guard(log);retry.push_back(n);}
                catch(const std::exception& e){failed=true;std::lock_guard guard(log);std::cerr<<"Frame "<<frames[n]<<": "<<e.what()<<"\n";}
            }
        });
        pool.clear();if(failed)return 1;
        for(const auto n:retry)render(n);
        std::cout<<"Rendered "<<frames.size()<<" distinct exposures using "<<threads<<" threads\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    return 0;
}
