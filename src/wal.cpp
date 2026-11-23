#include "db/wal.h"

#include "db/fault.h"

#include <array>
#include <cstring>

namespace db {
namespace {

constexpr std::size_t kCrcOffset = 4;

void throw_corrupt(const std::string& why) {
    fail("write-ahead log is corrupt: " + why);
}

}  // namespace

const char* record_type_name(RecordType type) {
    switch (type) {
        case RecordType::Begin:
            return "BEGIN";
        case RecordType::Insert:
            return "INSERT";
        case RecordType::Delete:
            return "DELETE";
        case RecordType::Commit:
            return "COMMIT";
        case RecordType::Abort:
            return "ABORT";
        case RecordType::Checkpoint:
            return "CHECKPOINT";
        case RecordType::PageImage:
            return "PAGE_IMAGE";
        case RecordType::CreateTable:
            return "CREATE_TABLE";
        case RecordType::DropTable:
            return "DROP_TABLE";
    }
    return "UNKNOWN";
}

Wal Wal::open(const std::filesystem::path& path) {
    Wal wal;
    wal.file_ = File::open(path);
    wal.validate_tail();
    return wal;
}

void Wal::validate_tail() {
    std::uint64_t offset = 0;
    std::uint64_t previous_end = 0;
    std::uint64_t last_valid = 0;
    std::uint64_t count = 0;

    std::array<std::uint8_t, kHeaderSize> header{};
    while (offset < file_.size()) {
        if (file_.size() - offset < kHeaderSize + kTrailerSize) {
            break;
        }
        file_.read_at(offset, header.data(), header.size());
        const std::uint32_t total = load_u32(header.data());
        if (total < kHeaderSize + kTrailerSize || total > file_.size() - offset) {
            break;
        }
        std::vector<std::uint8_t> bytes(total);
        file_.read_at(offset, bytes.data(), bytes.size());
        const std::uint32_t stored_crc = load_u32(bytes.data() + kCrcOffset);
        std::vector<std::uint8_t> copy = bytes;
        store_u32(copy.data() + kCrcOffset, 0);
        if (crc32(copy.data(), copy.size()) != stored_crc) {
            break;
        }
        if (load_u64(bytes.data() + 8) != offset + 1) {
            break;
        }
        if (load_u64(bytes.data() + 32) != previous_end) {
            break;
        }
        if (load_u64(bytes.data() + total - 8) != offset) {
            break;
        }
        previous_end = offset + total;
        last_valid = previous_end;
        offset = previous_end;
        ++count;
    }

    if (last_valid < file_.size()) {
        file_.truncate(last_valid);
    }
    records_ = count;
    last_end_ = last_valid;
}

Lsn Wal::append(RecordType type, TxnId txn, PageId page_id,
                std::uint32_t table_id, std::int64_t key,
                std::span<const std::uint8_t> payload) {
    const std::uint64_t offset = file_.size();
    const std::size_t total =
        kHeaderSize + payload.size() + kTrailerSize;
    if (total > 0xFFFFFFFFull) {
        fail("write-ahead log record is too large");
    }
    std::vector<std::uint8_t> bytes(total, 0);
    store_u32(bytes.data(), static_cast<std::uint32_t>(total));
    store_u32(bytes.data() + kCrcOffset, 0);
    store_u64(bytes.data() + 8, offset + 1);
    store_u64(bytes.data() + 16, txn);
    bytes[24] = static_cast<std::uint8_t>(type);
    store_u64(bytes.data() + 32, offset == 0 ? 0 : last_end_);
    store_u32(bytes.data() + 40, page_id);
    store_u32(bytes.data() + 44, table_id);
    store_i64(bytes.data() + 48, key);
    if (!payload.empty()) {
        std::memcpy(bytes.data() + kHeaderSize, payload.data(), payload.size());
    }
    store_u64(bytes.data() + total - kTrailerSize, offset);

    const std::uint32_t sum = crc32(bytes.data(), bytes.size());
    store_u32(bytes.data() + kCrcOffset, sum);

    fault::hit("wal.append.before");
    file_.append(bytes.data(), bytes.size());
    last_end_ = offset + total;
    ++records_;
    fault::hit("wal.append.after");
    return offset + 1;
}

void Wal::sync() {
    fault::hit("wal.sync.before");
    file_.sync();
    fault::hit("wal.sync.after");
}

bool Wal::read_at(std::uint64_t offset, WalRecord& record,
                  std::uint64_t& next_offset) const {
    if (offset >= file_.size()) {
        return false;
    }
    if (file_.size() - offset < kHeaderSize + kTrailerSize) {
        return false;
    }
    std::array<std::uint8_t, kHeaderSize> header{};
    file_.read_at(offset, header.data(), header.size());
    const std::uint32_t total = load_u32(header.data());
    if (total < kHeaderSize + kTrailerSize || total > file_.size() - offset) {
        throw_corrupt("invalid record length at offset " + std::to_string(offset));
    }
    std::vector<std::uint8_t> bytes(total);
    file_.read_at(offset, bytes.data(), bytes.size());
    const std::uint32_t stored_crc = load_u32(bytes.data() + kCrcOffset);
    std::vector<std::uint8_t> copy = bytes;
    store_u32(copy.data() + kCrcOffset, 0);
    if (crc32(copy.data(), copy.size()) != stored_crc) {
        throw_corrupt("checksum mismatch at offset " + std::to_string(offset));
    }
    if (load_u64(bytes.data() + total - kTrailerSize) != offset) {
        throw_corrupt("back pointer mismatch at offset " + std::to_string(offset));
    }
    if (load_u64(bytes.data() + 8) != offset + 1) {
        throw_corrupt("LSN mismatch at offset " + std::to_string(offset));
    }

    record.offset = offset;
    record.end = offset + total;
    record.lsn = offset + 1;
    record.txn = load_u64(bytes.data() + 16);
    record.type = static_cast<RecordType>(bytes[24]);
    record.page_id = load_u32(bytes.data() + 40);
    record.table_id = load_u32(bytes.data() + 44);
    record.key = load_i64(bytes.data() + 48);
    record.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                          bytes.end() - static_cast<std::ptrdiff_t>(kTrailerSize));
    next_offset = offset + total;
    return true;
}

bool Wal::read_back(std::uint64_t end, WalRecord& record,
                    std::uint64_t& previous_end) const {
    if (end < kHeaderSize + kTrailerSize || end > file_.size()) {
        return false;
    }
    std::uint8_t trailer[8];
    file_.read_at(end - kTrailerSize, trailer, sizeof(trailer));
    const std::uint64_t start = load_u64(trailer);
    if (start >= end) {
        throw_corrupt("invalid back pointer");
    }
    std::uint64_t next = 0;
    if (!read_at(start, record, next) || next != end) {
        throw_corrupt("record chain is broken");
    }
    std::array<std::uint8_t, kHeaderSize> header{};
    file_.read_at(start, header.data(), header.size());
    previous_end = load_u64(header.data() + 32);
    return true;
}

}  // namespace db
