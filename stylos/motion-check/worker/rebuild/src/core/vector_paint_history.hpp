#pragma once
#include "retas/vector_extension_archive.hpp"
#include <tuple>
#include <memory>
#include <optional>
#include <stop_token>
namespace retas::vector_draw_history_detail {class Edit;class Manager;}

namespace retas::vector_paint_history_detail {
struct Budget {
    std::uint64_t bytes{},work{};
    std::stop_token stop;
    void allocate(std::uint64_t);
    void visit(std::uint64_t=1);
};
// Runtime caches can retain pointers to inactive slots and cannot roundtrip
// through the persistent archive validator when switching history modes.
template<class T>struct CachePoolMemory {std::vector<VectorPoolRecord<T>> retired;int high{-1},cursor{};};
using CacheMemory=std::tuple<CachePoolMemory<VectorVertex>,CachePoolMemory<VectorLineScale>,CachePoolMemory<VectorSubcurve>,CachePoolMemory<VectorModelCurve>,CachePoolMemory<VectorCrosspoint>,CachePoolMemory<VectorLine>,CachePoolMemory<VectorArchiveLoop>,CachePoolMemory<VectorArchiveRegion>,CachePoolMemory<VectorArchiveFace>,CachePoolMemory<Bytes>,CachePoolMemory<Bytes>,CachePoolMemory<Bytes>>;
struct Cache {Layer owner;VectorPaintArchive content;std::optional<VectorShapeArchive> shape;CacheMemory memory;};
struct Snapshot;
struct Prepared;
class Manager;
//450C30/450F00/44F3F0: immutable primary, lazy opposite, same-ID typed
//copy and630FC0 clear. Owns content only; common properties remain live.
class Edit {
    friend class Manager;
    std::shared_ptr<const Snapshot> primary_,secondary_;
    bool reverse_{};
    std::shared_ptr<const vector_draw_history_detail::Edit> draw_;
public:
    static std::shared_ptr<const Edit> bind(const Layer&,
        std::optional<VectorBounds> raw_damage,Budget&);
    Prepared replay(const Layer& receiver,Budget&)const;
    //450C30 also consumes the live source's temporary material markers while
    //synchronizing its baseline. Return that source independently of payload.
    Layer synchronize_source(const Layer&,Budget&)const;
    std::optional<VectorBounds> window()const;
};
struct Prepared {Layer layer;std::shared_ptr<const Edit> opposite;};
struct ManagerState;
struct Bound;
struct Mirrored;
//44ABA0 owns one current source baseline, independently of the history stack.
//All returned managers are immutable candidates until Document publication.
class Manager {
    std::uint64_t owner_{};
    std::uint32_t family_{};
    std::shared_ptr<const ManagerState> state_;
    std::shared_ptr<const vector_draw_history_detail::Manager> draw_;
public:
    static std::shared_ptr<const Manager> ensure(std::shared_ptr<const Manager>,
        const Layer&,Budget&,bool force=false,bool draw_mode=false,bool select_paint=false);
    Bound bind(const Layer& post_transfer,std::optional<VectorBounds> raw_damage,Budget&)const;
    Bound bind_draw(const Layer& before,const Layer& post,std::optional<std::size_t> plane,
        std::optional<VectorBounds> raw_damage,Budget&,const std::vector<std::size_t>& selected={})const;
    Mirrored replay(const Edit&,const Layer& receiver,Budget&)const;
    bool draw_baseline()const noexcept{return bool(draw_)&&!state_;}
    const VectorPaintArchive* paint_baseline()const noexcept;
    const Layer* baseline_owner()const noexcept;
};
struct Bound {Layer source;std::shared_ptr<const Edit> edit;std::shared_ptr<const Manager> manager;};
struct Mirrored {Layer layer;std::shared_ptr<const Edit> opposite;std::shared_ptr<const Manager> manager;};
struct DocumentBinding {
    std::shared_ptr<const Manager> manager;
    std::uint64_t owner{};std::uint32_t family{};std::size_t hint{};
    std::shared_ptr<const std::vector<std::uint64_t>> layout;
};
// VecDraw clears only Vert topology. Copy the six face-extension pools
// through their native leaf copiers without touching Border/child ownership.
VectorPaintArchive copy_draw_extensions(const Layer&,VectorPaintArchive destination,
    VectorPaintArchive source,Budget&,CacheMemory* memory=nullptr);
}
