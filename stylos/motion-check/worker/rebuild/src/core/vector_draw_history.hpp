#pragma once
#include "vector_paint_history.hpp"
namespace retas::vector_draw_history_detail {
using Budget=vector_paint_history_detail::Budget;
struct Snapshot;struct ManagerState;struct Prepared;struct Bound;struct Mirrored;
class Manager;
class Edit {
    friend class Manager;
    std::shared_ptr<const Snapshot> primary_,secondary_;bool reverse_{};
public:
    Prepared replay(const Layer&,Budget&)const;
};
struct Prepared{Layer layer;std::shared_ptr<const Edit> opposite;};
class Manager {
    std::shared_ptr<const ManagerState> state_;
public:
    static std::shared_ptr<const Manager> ensure(std::shared_ptr<const Manager>,const Layer&,Budget&,bool force=false);
    Bound bind(const Layer& post,std::optional<std::size_t> plane,std::optional<VectorBounds> raw_damage,Budget&,const std::vector<std::size_t>& selected={})const;
    Mirrored replay(const Edit&,const Layer&,Budget&)const;
    const Layer* baseline_owner()const noexcept;
    vector_paint_history_detail::Cache cache(Budget&)const;
    static std::shared_ptr<const Manager> from_cache(vector_paint_history_detail::Cache,Budget&);
};
struct Bound{std::shared_ptr<const Edit> edit;std::shared_ptr<const Manager> manager;};
struct Mirrored{Layer layer;std::shared_ptr<const Edit> opposite;std::shared_ptr<const Manager> manager;};
}
