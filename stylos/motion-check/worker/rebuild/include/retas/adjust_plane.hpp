#pragma once
#include "filters.hpp"

namespace retas {
// Native CAdjustPlane helpers. Parameterless helpers retain their exact archive
// discriminator; kind/settings describe the eight helpers with settings dialogs.
struct AdjustParameters {
    FilterKind kind{FilterKind::Contrast};
    FilterSettings settings{ContrastSettings{}};
    Bytes payload_suffix,wrapper_suffix;
    std::array<Bytes,4> channel_suffix;
    std::uint32_t parameterless_kind{};
};
// The native dispatcher executes independently of its menu query. AcceptedSettings
// supplies a completed helper dialog packet; it performs no UI interaction.
enum class AdjustFilterExecution {Direct,AcceptedSettings};
AdjustParameters decode_adjust_parameters(std::span<const std::uint8_t>);
Bytes encode_adjust_parameters(const AdjustParameters&);
bool editable_adjust_plane(const Plane&) noexcept;
}
