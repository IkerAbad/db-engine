#include "db/common.h"

#include <array>

namespace db {
namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kCrcTable = make_table();

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t seed) {
    std::uint32_t c = seed ^ 0xFFFFFFFFu;
    for (std::uint8_t byte : data) {
        c = kCrcTable[(c ^ byte) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

}  // namespace db
