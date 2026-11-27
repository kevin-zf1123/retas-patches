#pragma once
#include "score.hpp"

namespace retas {
constexpr std::size_t score_csv_max_output_bytes=128U*1024U*1024U;
// Runtime-encoded resource strings, not Unicode text. These six fields map to
// PaintMan resources427D/427E/427F/42E0/42E1/42E2. Custom fields are bounded to
//65535 bytes and consumed through their first NUL, like native RCString printf.
struct ScoreCsvResources {
    Bytes primary_header,derived_header,voice_header;
    Bytes open_circle,filled_circle,cross;
};
// The shipped PaintMan/share.sys Chinese resource profile (GB18030 bytes).
ScoreCsvResources native_score_csv_resources();
// Pure synchronous export: no BOM, archive envelope, history or source edit.
// Both complete banks and all frames are emitted regardless of visibility or
// row field; labels do not expand holds. Names/cells retain native unescaped
// quotes, while Voice doubles quotes in the first six bytes only.
// ScoreLayer.name is stored RCString wire data. Consumed CR in names is rejected
// because native ReadString's CRLF projection depends on the runtime ACP/DBCS
// classifier. Fixed cell/Voice CR remains raw; bytes after NUL are ignored.
// The complete Score and projected output are admitted before output allocation.
Bytes encode_score_csv(const ScoreData&);
// Ordinary SaveAs projection for the pinned CP936 original: RCString layer
// names expand archive CR, while fixed labels and Voice remain raw bytes.
// Stateless; ScoreDocument owns successful-save baseline publication.
Bytes encode_score_csv_native(const ScoreData&);
Bytes encode_score_csv(const ScoreData&,const ScoreCsvResources&);
}
