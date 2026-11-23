#include "db/page.h"
#include "test.h"

#include <cstring>
#include <vector>

using namespace db;

namespace {

std::vector<std::uint8_t> cell(std::int64_t key, std::uint8_t fill,
                               std::size_t size = 24) {
    std::vector<std::uint8_t> bytes(size, fill);
    store_i64(bytes.data(), key);
    return bytes;
}

}  // namespace

TEST(page_layout_and_checksum) {
    Page page;
    page.init(7, PageType::Leaf, 0);
    CHECK_EQ(page.id(), 7u);
    CHECK(page.type() == PageType::Leaf);
    CHECK_EQ(page.level(), 0u);
    CHECK_EQ(page.slot_count(), 0u);
    CHECK(page.verify_checksum());
    CHECK_EQ(page.free_bytes(), kPageSize - Page::kHeaderSize);

    page.raw()[100] ^= 0xFF;
    CHECK(!page.verify_checksum());
    page.update_checksum();
    CHECK(page.verify_checksum());
}

TEST(page_sorted_cells_and_removal) {
    Page page;
    page.init(1, PageType::Leaf);
    for (int i = 0; i < 100; ++i) {
        const auto bytes = cell(i * 2, static_cast<std::uint8_t>(i));
        CHECK(page.insert_cell(i * 2, bytes));
    }
    CHECK_EQ(page.slot_count(), 100);
    CHECK_EQ(page.key_at(0), 0);
    CHECK_EQ(page.key_at(99), 198);
    CHECK_EQ(page.find_key(50), 25);
    CHECK_EQ(page.find_key(51), -1);

    const auto bytes = cell(50, 0);
    CHECK(!page.insert_cell(50, bytes));
    CHECK(page.remove_key(50));
    CHECK_EQ(page.find_key(50), -1);
    CHECK_EQ(page.slot_count(), 99);
    CHECK_EQ(page.free_bytes(), kPageSize - Page::kHeaderSize - 99 * 4 - 99 * 24);
}

TEST(page_compaction_allows_reuse) {
    Page page;
    page.init(2, PageType::Leaf);
    std::vector<std::vector<std::uint8_t>> cells;
    for (int i = 0; i < 80; ++i) {
        cells.push_back(cell(i, 1, 40));
    }
    for (int i = 0; i < 80; ++i) {
        CHECK(page.insert_cell(i, cells[static_cast<std::size_t>(i)]));
    }
    for (int i = 0; i < 80; i += 2) {
        CHECK(page.remove_key(i));
    }
    // The removed cells leave holes; inserting into the holes has to compact.
    for (int i = 0; i < 40; ++i) {
        const auto bytes = cell(1000 + i, 2, 40);
        CHECK(page.insert_cell(1000 + i, bytes));
    }
    CHECK_EQ(page.slot_count(), 80);
    CHECK_EQ(page.find_key(1000), 40);
}

TEST(page_meta_accessors) {
    Page page;
    page.init(0, PageType::Meta);
    page.set_meta_u32(kMetaBodyOffset, kDbPageMagic);
    page.set_meta_u32(kMetaBodyOffset + 4, 42);
    CHECK_EQ(page.meta_u32(kMetaBodyOffset), kDbPageMagic);
    CHECK_EQ(page.meta_u32(kMetaBodyOffset + 4), 42u);
    page.set_meta_u64(128, 0x1122334455667788ull);
    CHECK_EQ(page.meta_u64(128), 0x1122334455667788ull);
}
