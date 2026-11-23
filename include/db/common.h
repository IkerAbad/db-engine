#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace db {

using PageId = std::uint32_t;
using Lsn = std::uint64_t;
using TxnId = std::uint64_t;

inline constexpr std::size_t kPageSize = 4096;
inline constexpr PageId kInvalidPage = 0xFFFFFFFFu;
inline constexpr TxnId kNoTxn = 0;

// Page 0 body: magic + next page id. Table meta pages: magic + table id +
// current root page. Meta pages never move, so a root split only has to update
// the table meta page.
inline constexpr std::uint32_t kDbPageMagic = 0x44423031u;     // "DB01"
inline constexpr std::uint32_t kTableMetaMagic = 0x4D455441u;  // "META"
inline constexpr std::size_t kMetaBodyOffset = 64;

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

[[noreturn]] inline void fail(const std::string& message) {
    throw Error(message);
}

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t seed = 0);

inline std::uint32_t crc32(const void* data, std::size_t size) {
    return crc32(std::span<const std::uint8_t>(
        static_cast<const std::uint8_t*>(data), size));
}

// Little-endian encoding. The on-disk format is endian-stable.
inline std::uint16_t load_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}

inline std::uint32_t load_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t load_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

inline std::int64_t load_i64(const std::uint8_t* p) {
    return static_cast<std::int64_t>(load_u64(p));
}

inline void store_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

inline void store_u32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

inline void store_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

inline void store_i64(std::uint8_t* p, std::int64_t v) {
    store_u64(p, static_cast<std::uint64_t>(v));
}

inline std::uint64_t double_to_bits(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

inline double bits_to_double(std::uint64_t bits) {
    double v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

inline void append_u32(std::span<std::uint8_t>& out, std::uint32_t v) {
    store_u32(out.data(), v);
    out = out.subspan(4);
}

}  // namespace db
