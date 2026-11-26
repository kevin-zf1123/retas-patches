#pragma once
#include "retas/vector_face_archive.hpp"

namespace retas::vector_material_detail {
// Diagonal fields retain the ImageResolution aggregate order; page transforms
// add the cross-axis terms and keep translation until final point truncation.
struct Matrix {double sx,sy,tx,ty,xy{},yx{};};
void transform(VectorArchiveGradation&,const Matrix&);
void transform(VectorArchiveTexture&,const Matrix&);
}
