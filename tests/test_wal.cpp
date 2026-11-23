#include "db/file.h"
#include "db/wal.h"
#include "temp_dir.h"
#include "test.h"

#include <cstdint>
#include <vector>

using namespace db;

TEST(wal_roundtrip) {
    test::TempDir dir("wal");
    const auto path = dir.path() / "wal.log";
    {
        Wal wal = Wal::open(path);
        const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
        wal.append(RecordType::Begin, 7);
        wal.append(RecordType::Insert, 7, kInvalidPage, 3, 42, payload);
        wal.append(RecordType::Delete, 7, kInvalidPage, 3, 99, payload);
        wal.append(RecordType::Commit, 7);
        wal.append(RecordType::Checkpoint);
        wal.sync();
    }

    Wal wal = Wal::open(path);
    CHECK_EQ(wal.record_count(), 5u);
    std::uint64_t offset = 0;
    WalRecord record;
    std::uint64_t next = 0;
    CHECK(wal.read_at(offset, record, next));
    CHECK(record.type == RecordType::Begin);
    CHECK_EQ(record.txn, 7u);
    offset = next;
    CHECK(wal.read_at(offset, record, next));
    CHECK(record.type == RecordType::Insert);
    CHECK_EQ(record.table_id, 3u);
    CHECK_EQ(record.key, 42);
    CHECK_EQ(record.payload.size(), 5u);
    CHECK_EQ(record.payload[0], 1);
    offset = next;
    CHECK(wal.read_at(offset, record, next));
    CHECK(record.type == RecordType::Delete);
    CHECK_EQ(record.key, 99);
    offset = next;
    CHECK(wal.read_at(offset, record, next));
    CHECK(record.type == RecordType::Commit);
    offset = next;
    CHECK(wal.read_at(offset, record, next));
    CHECK(record.type == RecordType::Checkpoint);
    offset = next;
    CHECK_EQ(offset, wal.size());

    std::uint64_t cursor = wal.size();
    std::vector<RecordType> backwards;
    while (cursor > 0) {
        WalRecord back;
        std::uint64_t previous = 0;
        CHECK(wal.read_back(cursor, back, previous));
        backwards.push_back(back.type);
        cursor = previous;
    }
    CHECK_EQ(backwards.size(), 5u);
    CHECK(backwards[0] == RecordType::Checkpoint);
    CHECK(backwards[4] == RecordType::Begin);
}

TEST(wal_truncates_a_torn_tail) {
    test::TempDir dir("wal-torn");
    const auto path = dir.path() / "wal.log";
    std::uint64_t good = 0;
    {
        Wal wal = Wal::open(path);
        wal.append(RecordType::Begin, 1);
        wal.append(RecordType::Insert, 1, kInvalidPage, 1, 1);
        wal.append(RecordType::Commit, 1);
        wal.sync();
        good = wal.size();
        wal.append(RecordType::Begin, 2);
        wal.sync();
    }
    {
        File file = File::open(path);
        file.truncate(good + 10);
    }
    Wal wal = Wal::open(path);
    CHECK_EQ(wal.record_count(), 3u);
    CHECK_EQ(wal.size(), good);
}

TEST(wal_detects_a_corrupt_record) {
    test::TempDir dir("wal-corrupt");
    const auto path = dir.path() / "wal.log";
    std::uint64_t good = 0;
    {
        Wal wal = Wal::open(path);
        wal.append(RecordType::Begin, 1);
        good = wal.size();
        wal.append(RecordType::Commit, 1);
        wal.sync();
    }
    {
        File file = File::open(path);
        std::uint8_t byte = 0;
        file.read_at(good + 20, &byte, 1);
        byte ^= 0xFF;
        file.write_at(good + 20, &byte, 1);
        file.sync();
    }
    Wal wal = Wal::open(path);
    CHECK_EQ(wal.size(), good);
    CHECK_EQ(wal.record_count(), 1u);
}
