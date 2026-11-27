#pragma once
#include "retas/vector_gradation.hpp"
#include <utility>
namespace retas::vector_render_detail {
// Preparation-only certificate: validate/cache numeric fields and stop order
// without allocating an immutable chain. Rendering uses a frozen sampler only
// after every plan, actual query domain and joint budget has passed.
class GradationAdmission {
    VectorGradationSampler prepared_;
public:
    explicit GradationAdmission(std::shared_ptr<const VectorGradationMaterial> source):prepared_(std::move(source),false){}
    void rectangle(int left,int top,int right,int bottom)const{prepared_.admit_rectangle(left,top,right,bottom);}
};
}
