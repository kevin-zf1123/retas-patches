#pragma once
#include "retas/document.hpp"
namespace retas::native_vector_detail {
//Document loading applies native constructor/setter and pool activation rules
//once. The raw typed archive codecs remain independent lossless transports.
void load_position(CommonElement&);
void load_owner(Layer&);
//Write-only masks, pointer indices and pool enumeration on a private archive.
//Live H/width parameters and all live runtime state remain unchanged.
Bytes canonical_owner(const Layer&);
//Call after confirming equal specific bytes; compare only persisted H lanes.
bool same_allocator_output(const Layer&,const Layer&) noexcept;
}
