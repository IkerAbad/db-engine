#include "db/buffer_pool.h"
#include "db/file.h"
#include "db/wal.h"
#include "temp_dir.h"
#include "test.h"

#include <cstdint>

using namespace db;

namespace {

struct Fixture {
    test::TempDir dir{"buffer-pool"};
    File data = File::open(dir.path() / "data.db");
    Wal wal = Wal::open(dir.path() / "wal.log");
    BufferPool pool{data, wal, 4};

    Fixture() {
        Page& page = pool.fetch(0);
        page.init(0, PageType::Meta);
        page.set_meta_u32(kMetaBodyOffset, kDbPageMagic);
        page.set_meta_u32(kMetaBodyOffset + 4, 1);
        pool.unpin(0, true);
    }
};

}  // namespace

TEST(buffer_pool_allocates_pages) {
    Fixture fixture;
    const PageId first = fixture.pool.allocate();
    const PageId second = fixture.pool.allocate();
    CHECK_EQ(first, 1u);
    CHECK_EQ(second, 2u);
    {
        Page& page = fixture.pool.fetch(first);
        page.init(first, PageType::Leaf);
        fixture.pool.unpin(first, true);
    }
    {
        Page& page = fixture.pool.fetch(second);
        page.init(second, PageType::Leaf);
        fixture.pool.unpin(second, true);
    }
    fixture.pool.flush_all();
    CHECK_EQ(fixture.data.size(), 3u * kPageSize);
}

TEST(buffer_pool_lru_and_hits) {
    Fixture fixture;
    for (PageId id = 1; id <= 3; ++id) {
        Page& page = fixture.pool.fetch(id);
        page.init(id, PageType::Leaf);
        fixture.pool.unpin(id, true);
    }
    // Touch 1 and 2 so 3 is the least recently used page.
    fixture.pool.fetch(1);
    fixture.pool.unpin(1, false);
    fixture.pool.fetch(2);
    fixture.pool.unpin(2, false);
    {
        Page& page = fixture.pool.fetch(4);
        page.init(4, PageType::Leaf);
        fixture.pool.unpin(4, true);
    }

    Page& page5 = fixture.pool.fetch(5);
    page5.init(5, PageType::Leaf);
    fixture.pool.unpin(5, true);
    CHECK_EQ(fixture.pool.stats().evictions, 2u);
    CHECK_EQ(fixture.pool.stats().hits, 2u);
    CHECK(fixture.pool.in_memory(1));
    CHECK(!fixture.pool.in_memory(3));

    Page& again = fixture.pool.fetch(3);
    CHECK_EQ(again.id(), 3u);
    fixture.pool.unpin(3, false);
    CHECK_EQ(fixture.pool.stats().misses, 7u);
}

TEST(buffer_pool_writes_a_valid_page) {
    Fixture fixture;
    {
        Page& page = fixture.pool.fetch(5);
        page.init(5, PageType::Leaf);
        for (int i = 0; i < 10; ++i) {
            const std::int64_t key = i;
            std::vector<std::uint8_t> cell(16, 0);
            store_i64(cell.data(), key);
            CHECK(page.insert_cell(key, cell));
        }
        fixture.pool.unpin(5, true);
    }
    fixture.pool.flush(5);
    Page on_disk;
    fixture.data.read_at(5 * kPageSize, on_disk.raw(), kPageSize);
    CHECK_EQ(on_disk.id(), 5u);
    CHECK_EQ(on_disk.slot_count(), 10u);
    CHECK(on_disk.verify_checksum());
    CHECK_EQ(fixture.pool.stats().writes, 1u);
}

TEST(buffer_pool_refuses_to_run_out_of_frames) {
    Fixture fixture;
    fixture.pool.fetch(1);
    fixture.pool.fetch(2);
    fixture.pool.fetch(3);
    fixture.pool.fetch(4);
    CHECK_THROWS(fixture.pool.fetch(5));
}
