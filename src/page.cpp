#include "db/page.h"

#include <algorithm>
#include <cstring>

namespace db {
namespace {

constexpr std::size_t kSlotOffset = 20;
constexpr std::size_t kFreeEndOffset = 22;
constexpr std::size_t kNextOffset = 24;
constexpr std::size_t kPrevOffset = 28;
constexpr std::size_t kLeftChildOffset = 32;
constexpr std::size_t kCellBytesOffset = 36;
constexpr std::size_t kSlotDir = Page::kHeaderSize;

}  // namespace

Page::Page() = default;

void Page::clear() {
    data_.fill(0);
}

void Page::init(PageId id, PageType type, std::uint16_t level) {
    data_.fill(0);
    store_u32(data_.data(), id);
    data_[4] = static_cast<std::uint8_t>(type);
    data_[5] = 0;
    store_u16(data_.data() + 6, level);
    store_u64(data_.data() + 8, 0);
    store_u16(data_.data() + kSlotOffset, 0);
    store_u16(data_.data() + kFreeEndOffset, static_cast<std::uint16_t>(kPageSize));
    store_u32(data_.data() + kNextOffset, kInvalidPage);
    store_u32(data_.data() + kPrevOffset, kInvalidPage);
    store_u32(data_.data() + kLeftChildOffset, kInvalidPage);
    store_u32(data_.data() + kCellBytesOffset, 0);
    update_checksum();
}

PageId Page::id() const {
    return load_u32(data_.data());
}

PageType Page::type() const {
    return static_cast<PageType>(data_[4]);
}

std::uint16_t Page::level() const {
    return load_u16(data_.data() + 6);
}

void Page::set_level(std::uint16_t level) {
    store_u16(data_.data() + 6, level);
}

Lsn Page::lsn() const {
    return load_u64(data_.data() + 8);
}

void Page::set_lsn(Lsn lsn) {
    store_u64(data_.data() + 8, lsn);
}

PageId Page::next() const {
    return load_u32(data_.data() + kNextOffset);
}

void Page::set_next(PageId id) {
    store_u32(data_.data() + kNextOffset, id);
}

PageId Page::prev() const {
    return load_u32(data_.data() + kPrevOffset);
}

void Page::set_prev(PageId id) {
    store_u32(data_.data() + kPrevOffset, id);
}

PageId Page::left_child() const {
    return load_u32(data_.data() + kLeftChildOffset);
}

void Page::set_left_child(PageId id) {
    store_u32(data_.data() + kLeftChildOffset, id);
}

std::uint16_t Page::slot_count() const {
    const std::uint16_t count = load_u16(data_.data() + kSlotOffset);
    if (count > kMaxSlots) {
        fail("page has an invalid slot count");
    }
    return count;
}

std::uint16_t Page::slot_offset(int slot) const {
    return load_u16(data_.data() + kSlotDir + static_cast<std::size_t>(slot) * 4);
}

std::uint16_t Page::slot_length(int slot) const {
    return load_u16(data_.data() + kSlotDir + static_cast<std::size_t>(slot) * 4 + 2);
}

void Page::set_slot(int slot, std::uint16_t offset, std::uint16_t length) {
    store_u16(data_.data() + kSlotDir + static_cast<std::size_t>(slot) * 4, offset);
    store_u16(data_.data() + kSlotDir + static_cast<std::size_t>(slot) * 4 + 2, length);
}

std::span<const std::uint8_t> Page::cell_at(int slot) const {
    if (slot < 0 || slot >= slot_count()) {
        fail("cell slot out of range");
    }
    const std::uint16_t offset = slot_offset(slot);
    const std::uint16_t length = slot_length(slot);
    if (offset > kPageSize || length > kPageSize - offset) {
        fail("cell points outside the page");
    }
    return std::span<const std::uint8_t>(data_.data() + offset, length);
}

std::int64_t Page::key_at(int slot) const {
    if (slot < 0 || slot >= slot_count()) {
        fail("key slot out of range");
    }
    const auto cell = cell_at(slot);
    if (cell.size() < 8) {
        fail("cell is smaller than a key: slot " + std::to_string(slot) +
             " length " + std::to_string(cell.size()) + " count " +
             std::to_string(slot_count()) + " free_end " +
             std::to_string(load_u16(data_.data() + kFreeEndOffset)) +
             " cell_bytes " +
             std::to_string(load_u32(data_.data() + kCellBytesOffset)));
    }
    return load_i64(cell.data());
}

int Page::lower_bound(std::int64_t key) const {
    int lo = 0;
    int hi = slot_count();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (key_at(mid) < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

int Page::find_key(std::int64_t key) const {
    const int pos = lower_bound(key);
    if (pos < slot_count() && key_at(pos) == key) {
        return pos;
    }
    return -1;
}

std::size_t Page::used_bytes() const {
    return kHeaderSize + static_cast<std::size_t>(slot_count()) * 4 +
           load_u32(data_.data() + kCellBytesOffset);
}

std::size_t Page::free_bytes() const {
    return kPageSize - used_bytes();
}

bool Page::would_fit(std::size_t cell_size) const {
    return free_bytes() >= cell_size + 4;
}

void Page::compact() {
    const int n = slot_count();
    std::array<std::uint8_t, kPageSize> tmp{};
    std::array<std::uint16_t, kMaxSlots> offsets{};

    std::size_t pos = kPageSize;
    for (int i = 0; i < n; ++i) {
        const std::size_t len = slot_length(i);
        pos -= len;
        std::memcpy(tmp.data() + pos, data_.data() + slot_offset(i), len);
        offsets[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(pos);
    }
    if (pos < kPageSize) {
        std::memcpy(data_.data() + pos, tmp.data() + pos, kPageSize - pos);
    }
    for (int i = 0; i < n; ++i) {
        set_slot(i, offsets[static_cast<std::size_t>(i)], slot_length(i));
    }
    store_u16(data_.data() + kFreeEndOffset, static_cast<std::uint16_t>(pos));
}

bool Page::insert_cell(std::int64_t key, std::span<const std::uint8_t> cell) {
    if (cell.size() < 8 || cell.size() > kPageSize) {
        fail("invalid cell size");
    }
    if (load_i64(cell.data()) != key) {
        fail("cell key does not match slot key");
    }
    const int pos = lower_bound(key);
    if (pos < slot_count() && key_at(pos) == key) {
        return false;
    }
    const std::size_t needed = cell.size();
    if (!would_fit(needed)) {
        return false;
    }

    const std::size_t slot_end = kSlotDir + static_cast<std::size_t>(slot_count()) * 4;
    const std::size_t required = needed + 4;
    std::uint16_t free_end = load_u16(data_.data() + kFreeEndOffset);
    if (static_cast<std::size_t>(free_end) < slot_end + required) {
        compact();
        free_end = load_u16(data_.data() + kFreeEndOffset);
        if (static_cast<std::size_t>(free_end) < slot_end + required) {
            return false;
        }
    }

    const auto new_offset = static_cast<std::uint16_t>(free_end - needed);
    std::memcpy(data_.data() + new_offset, cell.data(), needed);
    store_u16(data_.data() + kFreeEndOffset, new_offset);

    const int n = slot_count();
    const std::size_t move_from = kSlotDir + static_cast<std::size_t>(pos) * 4;
    const std::size_t move_size = static_cast<std::size_t>(n - pos) * 4;
    if (move_size > 0) {
        std::memmove(data_.data() + move_from + 4, data_.data() + move_from, move_size);
    }
    set_slot(pos, new_offset, static_cast<std::uint16_t>(needed));
    store_u16(data_.data() + kSlotOffset, static_cast<std::uint16_t>(n + 1));
    store_u32(data_.data() + kCellBytesOffset,
              load_u32(data_.data() + kCellBytesOffset) + static_cast<std::uint32_t>(needed));
    return true;
}

bool Page::remove_at(int slot) {
    const int n = slot_count();
    if (slot < 0 || slot >= n) {
        return false;
    }
    const std::size_t len = slot_length(slot);
    const std::size_t move_from = kSlotDir + static_cast<std::size_t>(slot) * 4;
    const std::size_t move_size = static_cast<std::size_t>(n - slot - 1) * 4;
    if (move_size > 0) {
        std::memmove(data_.data() + move_from, data_.data() + move_from + 4, move_size);
    }
    store_u16(data_.data() + kSlotOffset, static_cast<std::uint16_t>(n - 1));
    store_u32(data_.data() + kCellBytesOffset,
              load_u32(data_.data() + kCellBytesOffset) - static_cast<std::uint32_t>(len));
    return true;
}

bool Page::remove_key(std::int64_t key) {
    const int pos = find_key(key);
    if (pos < 0) {
        return false;
    }
    return remove_at(pos);
}

void Page::set_key_at(int slot, std::int64_t key) {
    if (slot < 0 || slot >= slot_count()) {
        fail("slot out of range");
    }
    if (slot_length(slot) < 8) {
        fail("cell is too small to hold a key");
    }
    store_i64(data_.data() + slot_offset(slot), key);
}

void Page::update_checksum() {
    store_u32(data_.data() + kChecksumOffset, 0);
    const std::uint32_t sum = crc32(data_.data(), data_.size());
    store_u32(data_.data() + kChecksumOffset, sum);
}

bool Page::verify_checksum() const {
    const std::uint32_t stored = load_u32(data_.data() + kChecksumOffset);
    if (stored == 0) {
        return false;
    }
    std::array<std::uint8_t, kPageSize> copy = data_;
    store_u32(copy.data() + kChecksumOffset, 0);
    return crc32(copy.data(), copy.size()) == stored;
}

std::uint32_t Page::meta_u32(std::size_t offset) const {
    if (offset + 4 > kPageSize) {
        fail("meta read out of range");
    }
    return load_u32(data_.data() + offset);
}

void Page::set_meta_u32(std::size_t offset, std::uint32_t value) {
    if (offset + 4 > kPageSize) {
        fail("meta write out of range");
    }
    store_u32(data_.data() + offset, value);
}

std::uint64_t Page::meta_u64(std::size_t offset) const {
    if (offset + 8 > kPageSize) {
        fail("meta read out of range");
    }
    return load_u64(data_.data() + offset);
}

void Page::set_meta_u64(std::size_t offset, std::uint64_t value) {
    if (offset + 8 > kPageSize) {
        fail("meta write out of range");
    }
    store_u64(data_.data() + offset, value);
}

}  // namespace db
