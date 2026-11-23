#pragma once

#include "db/common.h"
#include "db/file.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace db {

enum class RecordType : std::uint8_t {
    Begin = 1,
    Insert = 2,
    Delete = 3,
    Commit = 4,
    Abort = 5,
    Checkpoint = 6,
    PageImage = 7,
    CreateTable = 8,
    DropTable = 9,
};

struct WalRecord {
    Lsn lsn = 0;
    TxnId txn = kNoTxn;
    RecordType type = RecordType::Begin;
    PageId page_id = kInvalidPage;
    std::uint32_t table_id = 0;
    std::int64_t key = 0;
    std::vector<std::uint8_t> payload;
    std::uint64_t offset = 0;
    std::uint64_t end = 0;
};

// Append-only write-ahead log. Every record knows where the previous record
// ends, so rollback and recovery can walk the log backwards without keeping an
// index in memory.
class Wal {
public:
    static constexpr std::size_t kHeaderSize = 56;
    static constexpr std::size_t kTrailerSize = 8;

    static Wal open(const std::filesystem::path& path);

    Wal(Wal&&) noexcept = default;
    Wal& operator=(Wal&&) noexcept = default;
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    Lsn append(RecordType type, TxnId txn = kNoTxn,
               PageId page_id = kInvalidPage, std::uint32_t table_id = 0,
               std::int64_t key = 0,
               std::span<const std::uint8_t> payload = {});
    void sync();

    std::uint64_t size() const noexcept { return file_.size(); }
    Lsn next_lsn() const noexcept { return file_.size() + 1; }
    std::uint64_t record_count() const noexcept { return records_; }

    bool read_at(std::uint64_t offset, WalRecord& record,
                 std::uint64_t& next_offset) const;
    bool read_back(std::uint64_t end, WalRecord& record,
                   std::uint64_t& previous_end) const;

    void close() { file_.close(); }

private:
    Wal() = default;
    void validate_tail();

    File file_;
    std::uint64_t records_ = 0;
    std::uint64_t last_end_ = 0;
};

const char* record_type_name(RecordType type);

}  // namespace db
