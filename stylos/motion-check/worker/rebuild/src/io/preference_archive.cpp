#include "retas/preference_archive.hpp"
#include <algorithm>
#include <set>

namespace retas {
namespace {
std::size_t add_record_size(std::size_t total, const PrefRecord& record) {
    if (record.label.size() > 65535) throw std::length_error("Preference label exceeds u16 byte count");
    if (record.payload.size() > 32767) throw std::length_error("Preference payload exceeds positive i16 byte count");
    const auto size = record.label.size() + record.payload.size() + 4;
    if (size > preference_archive_max_bytes - total)
        throw std::length_error("Preference archive exceeds modern byte limit");
    return total + size;
}
std::size_t validate_records(std::span<const PrefRecord> records) {
    if (records.size() > preference_archive_max_records)
        throw std::length_error("Preference archive exceeds modern record limit");
    std::size_t total = 0;
    for (const auto& record : records) total = add_record_size(total, record);
    return total;
}
void require_width(std::span<const std::uint8_t> payload, std::size_t width, std::size_t origin) {
    if (payload.size() != width) throw FormatError(origin, "Preference payload width mismatch");
}
struct LabelLess {
    bool operator()(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) const {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    }
};
}
PrefArchive decode_preference_archive(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > preference_archive_max_bytes)
        throw FormatError(0, "Preference archive exceeds modern byte limit");
    Reader input(bytes);
    PrefArchive records;
    while (input.remaining()) {
        if (records.size() == preference_archive_max_records)
            throw FormatError(input.offset(), "Preference archive exceeds modern record limit");
        auto label = input.string();
        const auto size_offset = input.offset();
        const auto size = input.u16();
        if (size > 32767) throw FormatError(size_offset, "Negative preference payload size");
        auto payload = input.bytes(size);
        records.push_back({std::move(label), std::move(payload)});
    }
    return records;
}
Bytes encode_preference_archive(std::span<const PrefRecord> records) {
    const auto size = validate_records(records);
    Writer output;
    output.data.reserve(size);
    for (const auto& record : records) {
        output.string(record.label);
        output.u16(static_cast<std::uint16_t>(record.payload.size()));
        output.raw(record.payload);
    }
    return std::move(output.data);
}
PrefLookupResult lookup_preference_record(std::span<const PrefRecord> records,
    std::span<const std::uint8_t> label, std::size_t expected_width) {
    for (const auto& record : records) {
        if (std::equal(record.label.begin(), record.label.end(), label.begin(), label.end()))
            return {record.payload.size() == expected_width ? PrefLookupStatus::Found : PrefLookupStatus::WidthMismatch,
                record.payload};
    }
    return {};
}
Bytes pref_i32_payload(std::int32_t value) {
    Writer output;
    output.i32(value);
    return std::move(output.data);
}
Bytes pref_u64_payload(std::uint64_t value) {
    Writer output;
    output.u64(value);
    return std::move(output.data);
}
Bytes pref_f64_payload(double value) {
    Writer output;
    output.f64(value);
    return std::move(output.data);
}
std::int32_t read_pref_i32(std::span<const std::uint8_t> payload, std::size_t origin) {
    require_width(payload, 4, origin);
    return Reader(payload, Endian::Big, origin).i32();
}
std::uint64_t read_pref_u64(std::span<const std::uint8_t> payload, std::size_t origin) {
    require_width(payload, 8, origin);
    return Reader(payload, Endian::Big, origin).u64();
}
double read_pref_f64(std::span<const std::uint8_t> payload, std::size_t origin) {
    require_width(payload, 8, origin);
    return Reader(payload, Endian::Big, origin).f64();
}
PrefArchive merge_preference_archives(std::span<const PrefRecord> pending,
    std::span<const PrefRecord> old_records, PrefMergeMode mode) {
    if (mode != PrefMergeMode::NativeClose && mode != PrefMergeMode::PreserveUnshadowed)
        throw std::invalid_argument("Invalid preference merge mode");
    validate_records(pending);
    validate_records(old_records);
    std::set<std::span<const std::uint8_t>, LabelLess> pending_labels;
    std::vector<const PrefRecord*> selected;
    selected.reserve(pending.size() + old_records.size());
    std::size_t total = 0;
    auto select = [&](const PrefRecord& record) {
        if (selected.size() == preference_archive_max_records)
            throw std::length_error("Merged preference archive exceeds modern record limit");
        total = add_record_size(total, record);
        selected.push_back(&record);
    };
    for (const auto& record : pending) {
        pending_labels.insert(record.label);
        select(record);
    }
    for (const auto& record : old_records) {
        if (mode == PrefMergeMode::NativeClose && record.payload.empty()) break;
        if (!pending_labels.contains(record.label)) select(record);
    }
    PrefArchive result;
    result.reserve(selected.size());
    for (const auto* record : selected) result.push_back(*record);
    return result;
}
}
