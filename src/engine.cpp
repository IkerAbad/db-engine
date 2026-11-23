#include "db/engine.h"

#include "db/btree.h"
#include "db/fault.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace db {

Engine::Engine(EngineOptions options)
    : options_(std::move(options)),
      catalog_(options_.directory / "catalog.db") {}

Engine::~Engine() {
    try {
        close();
    } catch (...) {
    }
}

void Engine::open() {
    if (open_) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(options_.directory, ec);
    if (ec) {
        fail("cannot create the database directory: " + ec.message());
    }
    // A crashed query may have left spill files behind.
    std::error_code tmp_error;
    std::filesystem::remove_all(options_.directory / "tmp", tmp_error);

    const std::filesystem::path data_path = options_.directory / "data.db";
    data_ = std::make_unique<File>(File::open(data_path));
    if (data_->size() % kPageSize != 0) {
        data_->truncate(data_->size() / kPageSize * kPageSize);
    }
    const bool fresh = data_->size() == 0;

    const std::filesystem::path wal_path = options_.directory / "wal.log";
    if (fresh) {
        std::error_code ignored;
        if (std::filesystem::exists(wal_path, ignored)) {
            File stale = File::open(wal_path);
            stale.truncate(0);
        }
    }
    wal_ = std::make_unique<Wal>(Wal::open(wal_path));
    pool_ = std::make_unique<BufferPool>(*data_, *wal_, options_.buffer_pages);

    if (fresh) {
        Page& page = pool_->fetch(0);
        page.init(0, PageType::Meta, 0);
        page.set_meta_u32(kMetaBodyOffset, kDbPageMagic);
        page.set_meta_u32(kMetaBodyOffset + 4, 1);
        page.update_checksum();
        pool_->unpin(0, true);
    } else {
        Page& page = pool_->fetch(0, true);
        const bool valid = page.verify_checksum() && page.id() == 0 &&
                           page.type() == PageType::Meta &&
                           page.meta_u32(kMetaBodyOffset) == kDbPageMagic;
        const PageId header_next =
            valid ? page.meta_u32(kMetaBodyOffset + 4) : 1;
        pool_->unpin(0, false);
        pool_->set_next_page(
            std::max<PageId>(header_next,
                             static_cast<PageId>(data_->size() / kPageSize)));
    }

    catalog_.load();
    if (fresh && !catalog_.tables().empty()) {
        fail("data.db is empty but catalog.db still describes tables");
    }
    if (fresh) {
        catalog_.set_checkpoint_offset(0);
        catalog_.set_next_txn_id(1);
        catalog_.set_next_table_id(1);
    }
    if (catalog_.checkpoint_offset() > wal_->size()) {
        catalog_.set_checkpoint_offset(0);
    }
    if (wal_->size() > catalog_.checkpoint_offset()) {
        recover();
    }

    {
        Page& page = pool_->fetch(0, true);
        if (page.id() != 0 || page.type() != PageType::Meta ||
            page.meta_u32(kMetaBodyOffset) != kDbPageMagic) {
            pool_->unpin(0, false);
            fail("database page 0 is corrupt");
        }
        PageId next = std::max<PageId>(
            page.meta_u32(kMetaBodyOffset + 4),
            static_cast<PageId>(data_->size() / kPageSize));
        if (next < 1) {
            next = 1;
        }
        page.set_meta_u32(kMetaBodyOffset + 4, next);
        page.update_checksum();
        pool_->unpin(0, true);
        pool_->set_next_page(next);
    }

    next_txn_ = std::max<std::uint64_t>(catalog_.next_txn_id(), 1);
    next_table_id_ = std::max<std::uint32_t>(catalog_.next_table_id(), 1);
    open_ = true;
    if (fresh) {
        checkpoint();
    }
}

void Engine::apply_page_image(const WalRecord& record) {
    if (record.payload.size() != kPageSize) {
        fail("write-ahead log contains a truncated page image");
    }
    const PageId id = record.page_id;
    if (id == kInvalidPage) {
        fail("page image record has no page id");
    }
    Page& page = pool_->fetch(id, true);
    const Lsn image_lsn = static_cast<Lsn>(record.key);
    if (page.lsn() < image_lsn) {
        std::memcpy(page.raw(), record.payload.data(), kPageSize);
        if (page.id() != id || !page.verify_checksum() ||
            page.lsn() != image_lsn) {
            pool_->unpin(id, false);
            fail("write-ahead log page image is inconsistent");
        }
        pool_->unpin(id, true);
    } else {
        pool_->unpin(id, false);
    }
    pool_->note_page_seen(id);
}

void Engine::redo_record(const WalRecord& record) {
    switch (record.type) {
        case RecordType::CreateTable: {
            const TableDef table = deserialize_table_def(record.payload);
            if (catalog_.find_by_id(table.id) != nullptr) {
                return;
            }
            if (catalog_.find(table.name) != nullptr) {
                fail("recovery found two tables named " + table.name);
            }
            catalog_.add(table);
            if (table.id + 1 > catalog_.next_table_id()) {
                catalog_.set_next_table_id(table.id + 1);
            }
            return;
        }
        case RecordType::DropTable: {
            const TableDef table = deserialize_table_def(record.payload);
            catalog_.remove_by_id(table.id);
            return;
        }
        case RecordType::Insert: {
            TableDef* table = catalog_.find_by_id(record.table_id);
            if (table == nullptr) {
                return;
            }
            BTree tree(*pool_, table->meta_page);
            tree.upsert(record.key, record.payload);
            return;
        }
        case RecordType::Delete: {
            TableDef* table = catalog_.find_by_id(record.table_id);
            if (table == nullptr) {
                return;
            }
            BTree tree(*pool_, table->meta_page);
            tree.remove(record.key);
            return;
        }
        default:
            return;
    }
}

void Engine::undo_record(const WalRecord& record) {
    switch (record.type) {
        case RecordType::Insert: {
            TableDef* table = catalog_.find_by_id(record.table_id);
            if (table == nullptr) {
                return;
            }
            BTree tree(*pool_, table->meta_page);
            tree.remove(record.key);
            return;
        }
        case RecordType::Delete: {
            TableDef* table = catalog_.find_by_id(record.table_id);
            if (table == nullptr) {
                return;
            }
            BTree tree(*pool_, table->meta_page);
            tree.upsert(record.key, record.payload);
            return;
        }
        case RecordType::CreateTable: {
            const TableDef table = deserialize_table_def(record.payload);
            if (catalog_.find_by_id(table.id) != nullptr) {
                catalog_.remove_by_id(table.id);
            }
            return;
        }
        case RecordType::DropTable: {
            const TableDef table = deserialize_table_def(record.payload);
            if (catalog_.find_by_id(table.id) == nullptr &&
                catalog_.find(table.name) == nullptr) {
                catalog_.add(table);
            }
            return;
        }
        default:
            return;
    }
}

void Engine::undo_transaction(TxnId txn, std::uint64_t end_offset) {
    const std::uint64_t stop = catalog_.checkpoint_offset();
    std::uint64_t offset = end_offset;
    int undone = 0;
    fault::hit("undo.before");
    while (offset > stop && offset > 0) {
        WalRecord record;
        std::uint64_t previous = 0;
        if (!wal_->read_back(offset, record, previous)) {
            break;
        }
        if (record.offset < stop) {
            break;
        }
        if (record.txn == txn) {
            if (record.type == RecordType::Begin) {
                break;
            }
            undo_record(record);
            ++undone;
            if (undone == 5) {
                fault::hit("undo.during");
            }
        }
        offset = previous;
    }
    fault::hit("undo.after");
}

void Engine::rebuild_tables() {
    for (TableDef& table : catalog_.tables()) {
        BTree tree(*pool_, table.meta_page);
        tree.rebuild();
    }
}

void Engine::recover() {
    fault::hit("recovery.before");
    const std::uint64_t start = catalog_.checkpoint_offset() <= wal_->size()
                                    ? catalog_.checkpoint_offset()
                                    : 0;

    std::unordered_map<TxnId, int> states;  // 1 active, 2 committed, 3 aborted
    std::uint64_t max_txn = 0;
    std::uint64_t offset = start;
    WalRecord record;
    std::uint64_t next = 0;

    // Physical redo of page images and the transaction table.
    while (wal_->read_at(offset, record, next)) {
        if (record.txn > max_txn) {
            max_txn = record.txn;
        }
        switch (record.type) {
            case RecordType::Begin:
                states[record.txn] = 1;
                break;
            case RecordType::Commit:
                states[record.txn] = 2;
                break;
            case RecordType::Abort:
                states[record.txn] = 3;
                break;
            case RecordType::PageImage:
                apply_page_image(record);
                break;
            default:
                break;
        }
        offset = next;
    }
    fault::hit("recovery.after_redo");

    // DDL is committed on its own, so replay it before any row.
    offset = start;    while (wal_->read_at(offset, record, next)) {
        if (record.type == RecordType::CreateTable ||
            record.type == RecordType::DropTable) {
            const auto it = states.find(record.txn);
            if (it != states.end() && it->second == 2) {
                redo_record(record);
            }
        }
        offset = next;
    }

    // Page writes may have raced with structural changes. The leaf chain is
    // the source of truth, so rebuild the internal levels from it.
    rebuild_tables();
    fault::hit("recovery.after_rebuild");

    offset = start;
    while (wal_->read_at(offset, record, next)) {
        if (record.type == RecordType::Insert ||
            record.type == RecordType::Delete) {
            const auto it = states.find(record.txn);
            if (it != states.end() && it->second == 2) {
                redo_record(record);
            }
        }
        offset = next;
    }
    fault::hit("recovery.after_row_redo");

    std::vector<TxnId> losers;
    for (const auto& [txn, state] : states) {
        if (state == 1) {
            losers.push_back(txn);
        }
    }
    std::sort(losers.begin(), losers.end(), std::greater<>());
    for (const TxnId txn : losers) {
        undo_transaction(txn, wal_->size());
    }
    fault::hit("recovery.after_undo");

    pool_->flush_all();
    data_->sync();
    for (const TxnId txn : losers) {
        wal_->append(RecordType::Abort, txn);
    }
    wal_->sync();
    wal_->append(RecordType::Checkpoint);
    wal_->sync();

    catalog_.set_checkpoint_offset(wal_->size());
    catalog_.set_next_txn_id(std::max<std::uint64_t>(
        catalog_.next_txn_id(), max_txn + 1));
    catalog_.save();
    fault::hit("recovery.after_checkpoint");
    next_txn_ = catalog_.next_txn_id();
    next_table_id_ = catalog_.next_table_id();
}

void Engine::begin_txn() {
    if (active_txn_ != 0) {
        fail("a transaction is already open");
    }
    active_txn_ = next_txn_++;
    wal_->append(RecordType::Begin, active_txn_);
}

void Engine::commit_txn() {
    if (active_txn_ == 0) {
        return;
    }
    fault::hit("txn.commit.before");
    wal_->append(RecordType::Commit, active_txn_);
    fault::hit("txn.commit.after_log");
    wal_->sync();
    fault::hit("txn.commit.after_sync");
    active_txn_ = 0;
    ++commits_since_checkpoint_;
    maybe_checkpoint();
}

void Engine::rollback_txn() {
    if (active_txn_ == 0) {
        return;
    }
    fault::hit("txn.rollback.before");
    undo_transaction(active_txn_, wal_->size());
    fault::hit("txn.rollback.after_undo");
    force_pages();
    wal_->append(RecordType::Abort, active_txn_);
    wal_->sync();
    fault::hit("txn.rollback.after_abort");
    active_txn_ = 0;
}

void Engine::force_pages() {
    pool_->flush_all();
    data_->sync();
}

void Engine::maybe_checkpoint() {
    if (commits_since_checkpoint_ >= 1000) {
        checkpoint();
    }
}

void Engine::checkpoint() {
    if (active_txn_ != 0) {
        fail("cannot checkpoint inside a transaction");
    }
    fault::hit("checkpoint.before");
    pool_->flush_all();
    fault::hit("checkpoint.after_flush");
    wal_->append(RecordType::Checkpoint);
    wal_->sync();
    fault::hit("checkpoint.after_log");
    catalog_.set_checkpoint_offset(wal_->size());
    catalog_.set_next_txn_id(next_txn_);
    catalog_.set_next_table_id(next_table_id_);
    catalog_.save();
    commits_since_checkpoint_ = 0;
    fault::hit("checkpoint.after_catalog");
}

void Engine::close() {
    if (!open_) {
        return;
    }
    if (active_txn_ != 0) {
        rollback_txn();
    }
    checkpoint();
    pool_.reset();
    wal_->close();
    wal_.reset();
    data_->close();
    data_.reset();
    open_ = false;
}

bool Engine::check(std::string& error) {
    error.clear();
    {
        Page& page = pool_->fetch(0, true);
        const bool valid = page.id() == 0 &&
                           page.type() == PageType::Meta &&
                           page.meta_u32(kMetaBodyOffset) == kDbPageMagic;
        pool_->unpin(0, false);
        if (!valid) {
            error = "page 0 is corrupt";
            return false;
        }
    }
    // Verify the checksums that are actually on disk. Pages that were never
    // written (allocated and leaked) are all zero and are skipped.
    const std::uint64_t file_pages = data_->size() / kPageSize;
    for (PageId id = 0; id < file_pages; ++id) {
        Page raw;
        data_->read_at(static_cast<std::uint64_t>(id) * kPageSize, raw.raw(),
                       kPageSize);
        if (std::all_of(raw.raw(), raw.raw() + kPageSize,
                        [](std::uint8_t byte) { return byte == 0; })) {
            continue;
        }
        if (!raw.verify_checksum() || raw.id() != id) {
            error = "page " + std::to_string(id) + " failed its checksum";
            return false;
        }
    }
    for (const TableDef& table : catalog_.tables()) {
        BTree tree(*pool_, table.meta_page);
        std::string tree_error;
        if (!tree.check(tree_error)) {
            error = "table " + table.name + ": " + tree_error;
            return false;
        }
    }
    return true;
}

const BufferPoolStats& Engine::buffer_stats() const {
    static const BufferPoolStats empty;
    return pool_ ? pool_->stats() : empty;
}

TableDef& Engine::require_table(std::string_view name) {
    TableDef* table = catalog_.find(name);
    if (table == nullptr) {
        fail("no such table: " + std::string(name));
    }
    return *table;
}

}  // namespace db
