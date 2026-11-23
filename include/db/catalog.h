#pragma once

#include "db/common.h"
#include "db/schema.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace db {

struct TableDef {
    std::uint32_t id = 0;
    std::string name;
    Schema schema;
    PageId meta_page = kInvalidPage;
};

std::vector<std::uint8_t> serialize_table_def(const TableDef& table);
TableDef deserialize_table_def(std::span<const std::uint8_t> bytes);

class Catalog {
public:
    explicit Catalog(std::filesystem::path path);

    void load();
    void save();

    bool exists() const;
    const std::vector<TableDef>& tables() const { return tables_; }
    std::vector<TableDef>& tables() { return tables_; }

    TableDef* find(std::string_view name);
    const TableDef* find(std::string_view name) const;
    TableDef* find_by_id(std::uint32_t id);

    void add(TableDef table);
    void remove(std::string_view name);
    void remove_by_id(std::uint32_t id);

    std::uint64_t checkpoint_offset() const { return checkpoint_offset_; }
    void set_checkpoint_offset(std::uint64_t offset) { checkpoint_offset_ = offset; }
    std::uint64_t next_txn_id() const { return next_txn_id_; }
    void set_next_txn_id(std::uint64_t id) { next_txn_id_ = id; }
    std::uint32_t next_table_id() const { return next_table_id_; }
    void set_next_table_id(std::uint32_t id) { next_table_id_ = id; }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::vector<TableDef> tables_;
    std::uint64_t checkpoint_offset_ = 0;
    std::uint64_t next_txn_id_ = 1;
    std::uint32_t next_table_id_ = 1;
};

}  // namespace db
