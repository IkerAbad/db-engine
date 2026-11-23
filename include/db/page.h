#pragma once

#include "db/common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace db {

enum class PageType : std::uint8_t {
    Free = 0,
    Meta = 1,
    Leaf = 2,
    Internal = 3,
};

// Slotted page. Cells are variable length and live at the end of the page,
// slots grow from the front and stay sorted by the int64 key at the start of
// every cell. Page 0 and per-table meta pages use PageType::Meta.
class Page {
public:
    static constexpr std::size_t kHeaderSize = 64;
    static constexpr std::size_t kChecksumOffset = 16;
    static constexpr std::size_t kMaxSlots = 1024;

    Page();
    Page(const Page&) = default;
    Page& operator=(const Page&) = default;

    void clear();
    void init(PageId id, PageType type, std::uint16_t level = 0);

    PageId id() const;
    PageType type() const;
    std::uint16_t level() const;
    void set_level(std::uint16_t level);
    Lsn lsn() const;
    void set_lsn(Lsn lsn);

    PageId next() const;
    void set_next(PageId id);
    PageId prev() const;
    void set_prev(PageId id);
    PageId left_child() const;
    void set_left_child(PageId id);

    std::uint16_t slot_count() const;
    std::int64_t key_at(int slot) const;
    std::span<const std::uint8_t> cell_at(int slot) const;

    // First slot whose key is >= key, or slot_count().
    int lower_bound(std::int64_t key) const;
    // Exact slot, or -1.
    int find_key(std::int64_t key) const;

    bool insert_cell(std::int64_t key, std::span<const std::uint8_t> cell);
    bool remove_at(int slot);
    bool remove_key(std::int64_t key);
    void set_key_at(int slot, std::int64_t key);

    std::size_t used_bytes() const;
    std::size_t free_bytes() const;
    bool would_fit(std::size_t cell_size) const;

    void update_checksum();
    bool verify_checksum() const;

    const std::uint8_t* raw() const { return data_.data(); }
    std::uint8_t* raw() { return data_.data(); }

    std::uint32_t meta_u32(std::size_t offset) const;
    void set_meta_u32(std::size_t offset, std::uint32_t value);
    std::uint64_t meta_u64(std::size_t offset) const;
    void set_meta_u64(std::size_t offset, std::uint64_t value);

private:
    std::array<std::uint8_t, kPageSize> data_{};

    void compact();
    void set_slot(int slot, std::uint16_t offset, std::uint16_t length);
    std::uint16_t slot_offset(int slot) const;
    std::uint16_t slot_length(int slot) const;
};

static_assert(sizeof(Page) == kPageSize);

}  // namespace db
