#pragma once

#include "db/buffer_pool.h"
#include "db/catalog.h"
#include "db/sql.h"
#include "db/wal.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace db {

struct EngineOptions {
    std::filesystem::path directory;
    std::size_t buffer_pages = 4096;
    std::size_t sort_buffer_bytes = 4 * 1024 * 1024;
    std::size_t join_buffer_bytes = 64 * 1024 * 1024;
};

struct QueryResult {
    std::vector<std::string> columns;
    std::uint64_t rows = 0;
    std::string message;
};

class Engine {
public:
    using RowSink = std::function<void(const std::vector<Value>&)>;

    explicit Engine(EngineOptions options);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void open();
    void close();

    std::vector<QueryResult> execute(std::string_view sql, const RowSink& sink);
    bool check(std::string& error);
    void checkpoint();

    const Catalog& catalog() const { return catalog_; }
    const BufferPoolStats& buffer_stats() const;
    std::uint64_t wal_bytes() const { return wal_ ? wal_->size() : 0; }
    std::uint64_t data_bytes() const { return data_ ? data_->size() : 0; }
    std::uint32_t next_page() const { return pool_ ? pool_->next_page() : 0; }
    bool in_transaction() const { return active_txn_ != 0; }
    const EngineOptions& options() const { return options_; }

private:
    QueryResult execute_statement(const Statement& statement, const RowSink& sink);
    QueryResult create_table(const Statement& statement);
    QueryResult drop_table(const Statement& statement);
    QueryResult insert_rows(const Statement& statement, const RowSink& sink);
    QueryResult select_rows(const Statement& statement, const RowSink& sink);
    QueryResult select_join_rows(const Statement& statement, const RowSink& sink);
    QueryResult delete_rows(const Statement& statement);

    TableDef& require_table(std::string_view name);

    void begin_txn();
    void commit_txn();
    void rollback_txn();
    void maybe_checkpoint();

    void recover();
    void apply_page_image(const WalRecord& record);
    void redo_record(const WalRecord& record);
    void undo_record(const WalRecord& record);
    void undo_transaction(TxnId txn, std::uint64_t end_offset);
    void rebuild_tables();
    void force_pages();

    EngineOptions options_;
    std::unique_ptr<File> data_;
    std::unique_ptr<Wal> wal_;
    std::unique_ptr<BufferPool> pool_;
    Catalog catalog_;
    TxnId active_txn_ = 0;
    TxnId next_txn_ = 1;
    std::uint32_t next_table_id_ = 1;
    std::uint64_t commits_since_checkpoint_ = 0;
    bool open_ = false;
};

}  // namespace db
