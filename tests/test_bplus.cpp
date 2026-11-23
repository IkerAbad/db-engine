#include "db/btree.h"
#include "db/buffer_pool.h"
#include "db/file.h"
#include "db/wal.h"
#include "temp_dir.h"
#include "test.h"

#include <algorithm>
#include <map>
#include <random>
#include <vector>

using namespace db;

namespace {

PageId make_meta(BufferPool& pool) {
    Page& page = pool.fetch(0, true);
    page.init(0, PageType::Meta);
    page.set_meta_u32(kMetaBodyOffset, kDbPageMagic);
    page.set_meta_u32(kMetaBodyOffset + 4, 1);
    page.update_checksum();
    pool.unpin(0, true);
    return BTree::create(pool, 1);
}

std::vector<std::uint8_t> make_payload(std::int64_t key) {
    std::vector<std::uint8_t> payload(24, static_cast<std::uint8_t>(key & 0xFF));
    store_i64(payload.data(), key);
    return payload;
}

struct TreeFixture {
    test::TempDir dir{"btree"};
    File data = File::open(dir.path() / "data.db");
    Wal wal = Wal::open(dir.path() / "wal.log");
    BufferPool pool{data, wal, 48};
    PageId meta = make_meta(pool);
    BTree tree{pool, meta};
};

void require_check(BTree& tree) {
    std::string error;
    if (!tree.check(error)) {
        throw test::Failure(__FILE__, __LINE__, "tree check: " + error);
    }
}

std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> scan_all(
    BTree& tree) {
    std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> rows;
    BTree::Cursor cursor(tree);
    if (!cursor.first()) {
        return rows;
    }
    while (cursor.valid()) {
        const auto payload = cursor.payload();
        rows.emplace_back(cursor.key(),
                          std::vector<std::uint8_t>(payload.begin(), payload.end()));
        if (!cursor.next()) {
            break;
        }
    }
    return rows;
}

}  // namespace

TEST(btree_insert_delete_scan) {
    TreeFixture fixture;
    std::map<std::int64_t, std::vector<std::uint8_t>> reference;
    std::mt19937 rng(12345);

    std::vector<std::int64_t> keys(4000);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        keys[i] = static_cast<std::int64_t>(i);
    }
    std::shuffle(keys.begin(), keys.end(), rng);

    for (const std::int64_t key : keys) {
        const auto payload = make_payload(key);
        CHECK(fixture.tree.insert(key, payload));
        reference[key] = payload;
        CHECK(!fixture.tree.insert(key, payload));
    }
    CHECK_EQ(fixture.tree.row_count(), reference.size());
    require_check(fixture.tree);

    const auto rows = scan_all(fixture.tree);
    CHECK_EQ(rows.size(), reference.size());
    auto expected = reference.begin();
    for (const auto& row : rows) {
        CHECK_EQ(row.first, expected->first);
        CHECK(row.second == expected->second);
        ++expected;
    }

    // Reverse order.
    BTree::Cursor reverse(fixture.tree);
    CHECK(reverse.last());
    auto expected_reverse = reference.rbegin();
    while (reverse.valid()) {
        CHECK_EQ(reverse.key(), expected_reverse->first);
        ++expected_reverse;
        if (!reverse.prev()) {
            break;
        }
    }

    // Range scans.
    for (int i = 0; i < 50; ++i) {
        const std::int64_t lo = static_cast<std::int64_t>(rng() % 4000);
        const std::int64_t hi = lo + 77;
        BTree::Cursor cursor(fixture.tree);
        auto reference_it = reference.lower_bound(lo);
        if (cursor.seek(lo)) {
            while (cursor.valid() && cursor.key() < hi) {
                CHECK(reference_it != reference.end());
                CHECK_EQ(cursor.key(), reference_it->first);
                ++reference_it;
                if (!cursor.next()) {
                    break;
                }
            }
        }
        while (reference_it != reference.end() && reference_it->first < hi) {
            throw test::Failure(__FILE__, __LINE__, "range scan missed a key");
        }
    }

    // Delete 2500 keys, in random order.
    std::shuffle(keys.begin(), keys.end(), rng);
    for (std::size_t i = 0; i < 2500; ++i) {
        CHECK(fixture.tree.remove(keys[i]));
        reference.erase(keys[i]);
        CHECK(!fixture.tree.remove(keys[i]));
        if (i % 200 == 0) {
            require_check(fixture.tree);
        }
    }
    CHECK_EQ(fixture.tree.row_count(), reference.size());
    require_check(fixture.tree);

    const auto after = scan_all(fixture.tree);
    CHECK_EQ(after.size(), reference.size());
    auto after_it = reference.begin();
    for (const auto& row : after) {
        CHECK_EQ(row.first, after_it->first);
        CHECK(row.second == after_it->second);
        ++after_it;
    }

    // Upsert replaces the payload.
    const std::int64_t key = reference.begin()->first;
    const auto replacement = make_payload(999999);
    CHECK(fixture.tree.upsert(key, replacement));
    const auto found = fixture.tree.find(key);
    CHECK(found.has_value());
    CHECK(*found == replacement);
    require_check(fixture.tree);

    fixture.tree.rebuild();
    require_check(fixture.tree);
    CHECK_EQ(fixture.tree.row_count(), reference.size());
    const auto rebuilt = scan_all(fixture.tree);
    CHECK_EQ(rebuilt.size(), reference.size());
}

TEST(btree_empty_tree) {
    TreeFixture fixture;
    CHECK_EQ(fixture.tree.row_count(), 0u);
    require_check(fixture.tree);
    CHECK(!fixture.tree.remove(1));
    CHECK(!fixture.tree.find(1).has_value());
    fixture.tree.rebuild();
    require_check(fixture.tree);
    CHECK(!fixture.tree.find(1).has_value());
}

TEST(btree_persists_across_reopen) {
    test::TempDir dir("btree-persist");
    const auto data_path = dir.path() / "data.db";
    const auto wal_path = dir.path() / "wal.log";
    PageId meta = kInvalidPage;
    std::map<std::int64_t, std::vector<std::uint8_t>> reference;
    {
        File data = File::open(data_path);
        Wal wal = Wal::open(wal_path);
        BufferPool pool{data, wal, 32};
        meta = make_meta(pool);
        BTree tree{pool, meta};
        for (std::int64_t key = 0; key < 2000; ++key) {
            const auto payload = make_payload(key * 7);
            CHECK(tree.insert(key * 7, payload));
            reference[key * 7] = payload;
        }
        require_check(tree);
        pool.flush_all();
        data.sync();
    }
    {
        File data = File::open(data_path);
        Wal wal = Wal::open(wal_path);
        BufferPool pool{data, wal, 16};
        BTree tree{pool, meta};
        require_check(tree);
        CHECK_EQ(tree.row_count(), reference.size());
        const auto rows = scan_all(tree);
        CHECK_EQ(rows.size(), reference.size());
        auto expected = reference.begin();
        for (const auto& row : rows) {
            CHECK_EQ(row.first, expected->first);
            CHECK(row.second == expected->second);
            ++expected;
        }
    }
}
