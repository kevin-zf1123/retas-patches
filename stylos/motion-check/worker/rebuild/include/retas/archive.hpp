#pragma once
#include "byte_storage.hpp"
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace retas {
using Bytes = std::vector<std::uint8_t,ByteAllocator<std::uint8_t>>;
class FormatError : public std::runtime_error {
public:
    std::size_t offset;
    FormatError(std::size_t where, const std::string& what)
        : std::runtime_error(what + " (offset " + std::to_string(where) + ")"), offset(where) {}
};
enum class Endian { Big, Little };
class Reader {
    std::span<const std::uint8_t> data_;
    std::size_t position_{}, origin_{};
    Endian endian_;
public:
    explicit Reader(std::span<const std::uint8_t> data, Endian e = Endian::Big, std::size_t origin = 0)
        : data_(data), origin_(origin), endian_(e) {}
    std::size_t remaining() const { return data_.size() - position_; }
    std::size_t offset() const { return origin_ + position_; }
    std::span<const std::uint8_t> take(std::size_t count) {
        if (count > remaining()) throw FormatError(offset(), "Truncated archive");
        auto result = data_.subspan(position_, count); position_ += count; return result;
    }
    Bytes bytes(std::size_t count) { auto s = take(count); return {s.begin(), s.end()}; }
    Bytes rest() { return bytes(remaining()); }
    std::uint64_t integer(unsigned count) {
        auto s = take(count); std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i)
            value = (value << 8) | s[endian_ == Endian::Big ? i : count - 1 - i];
        return value;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(integer(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(integer(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(integer(4)); }
    std::int32_t i32() { return std::bit_cast<std::int32_t>(u32()); }
    std::uint64_t u64() { return integer(8); }
    double f64() { return std::bit_cast<double>(u64()); }
    float f32() { return std::bit_cast<float>(u32()); }
    Bytes string() { return bytes(u16()); }
    Reader section() {
        const auto count = u32(); const auto start = offset();
        return Reader(take(count), endian_, start);
    }
    void require_count(std::int32_t count, std::size_t minimum, std::size_t cap = 100000) const {
        if (count < 0 || static_cast<std::size_t>(count) > cap ||
            (minimum && static_cast<std::size_t>(count) > remaining() / minimum))
            throw FormatError(offset(), "Invalid collection count");
    }
};
class Writer {
    Endian endian_;
public:
    Bytes data;
    explicit Writer(Endian e = Endian::Big) : endian_(e) {}
    void raw(std::span<const std::uint8_t> value) { data.insert(data.end(), value.begin(), value.end()); }
    void integer(std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            const unsigned shift = endian_ == Endian::Big ? count - 1 - i : i;
            data.push_back(static_cast<std::uint8_t>(value >> (shift * 8)));
        }
    }
    void u8(std::uint8_t value) { integer(value, 1); }
    void u16(std::uint16_t value) { integer(value, 2); }
    void u32(std::uint32_t value) { integer(value, 4); }
    void i32(std::int32_t value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void u64(std::uint64_t value) { integer(value, 8); }
    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }
    void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void string(std::span<const std::uint8_t> value) {
        if (value.size() > 65535) throw std::length_error("RCString exceeds 65535 bytes");
        u16(static_cast<std::uint16_t>(value.size())); raw(value);
    }
    void string(std::string_view value) {
        string(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    }
    void section(std::span<const std::uint8_t> value) {
        if (value.size() > UINT32_MAX) throw std::length_error("RCArchive section exceeds 32-bit size");
        u32(static_cast<std::uint32_t>(value.size())); raw(value);
    }
};
inline Bytes ascii(std::string_view value) { return {value.begin(), value.end()}; }
}
