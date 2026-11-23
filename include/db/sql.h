#pragma once

#include "db/schema.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace db {

enum class StatementKind {
    CreateTable,
    DropTable,
    Insert,
    Select,
    Delete,
    Begin,
    Commit,
    Rollback,
};

struct CreateColumn {
    std::string name;
    ColumnType type = ColumnType::Int;
    std::uint16_t length = 0;
    bool primary_key = false;
};

struct ColumnRef {
    std::string table;
    std::string column;
};

struct SelectItem {
    bool star = false;
    bool count_star = false;
    std::string table;
    std::string column;
};

struct Comparison {
    std::string table;
    std::string column;
    std::string op;
    Value value;
};

struct OrderBy {
    std::string table;
    std::string column;
    bool descending = false;
};

struct JoinClause {
    std::string table;
    ColumnRef left;
    ColumnRef right;
};

struct Statement {
    StatementKind kind = StatementKind::Select;
    std::string table;
    bool if_exists = false;

    std::vector<CreateColumn> create_columns;
    std::vector<std::string> insert_columns;
    std::vector<std::vector<Value>> insert_rows;
    std::vector<SelectItem> select_items;
    std::vector<Comparison> where;
    std::optional<OrderBy> order_by;
    std::optional<JoinClause> join;
    std::optional<std::uint64_t> limit;
};

std::vector<Statement> parse_sql(std::string_view text);

}  // namespace db
