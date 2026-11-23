#include "db/engine.h"

#include "db/btree.h"
#include "db/fault.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace db {
namespace {

struct ScanBound {
    bool has = false;
    std::int64_t value = 0;
    bool inclusive = true;
};

void apply_lower(ScanBound& bound, std::int64_t value, bool inclusive) {
    if (!bound.has || value > bound.value) {
        bound.has = true;
        bound.value = value;
        bound.inclusive = inclusive;
    } else if (value == bound.value) {
        bound.inclusive = bound.inclusive && inclusive;
    }
}

void apply_upper(ScanBound& bound, std::int64_t value, bool inclusive) {
    if (!bound.has || value < bound.value) {
        bound.has = true;
        bound.value = value;
        bound.inclusive = inclusive;
    } else if (value == bound.value) {
        bound.inclusive = bound.inclusive && inclusive;
    }
}

struct FilterPredicate {
    int column = 0;
    std::string op;
    Value value;
};

class RowIterator {
public:
    virtual ~RowIterator() = default;
    virtual bool next(std::vector<Value>& row) = 0;
};

class IndexScanIterator : public RowIterator {
public:
    IndexScanIterator(BTree& tree, const Schema& schema, bool reverse,
                      ScanBound lo, ScanBound hi, bool empty)
        : schema_(schema),
          reverse_(reverse),
          lo_(lo),
          hi_(hi),
          empty_(empty),
          cursor_(tree) {}

    bool next(std::vector<Value>& row) override {
        if (done_ || empty_) {
            return false;
        }
        if (!started_) {
            started_ = true;
            if (!position()) {
                done_ = true;
                return false;
            }
            if (cursor_.valid()) {
                if (!reverse_ && lo_.has && !lo_.inclusive &&
                    cursor_.key() == lo_.value) {
                    if (!cursor_.next()) {
                        done_ = true;
                        return false;
                    }
                } else if (reverse_ && hi_.has && !hi_.inclusive &&
                           cursor_.key() == hi_.value) {
                    if (!cursor_.prev()) {
                        done_ = true;
                        return false;
                    }
                }
            }
        } else {
            if (!advance()) {
                done_ = true;
                return false;
            }
        }
        if (!cursor_.valid()) {
            done_ = true;
            return false;
        }
        if (!in_range(cursor_.key())) {
            done_ = true;
            return false;
        }
        row = schema_.decode(cursor_.payload());
        return true;
    }

private:
    bool position() {
        if (reverse_) {
            return hi_.has ? cursor_.seek_le(hi_.value) : cursor_.last();
        }
        return lo_.has ? cursor_.seek(lo_.value) : cursor_.first();
    }

    bool advance() { return reverse_ ? cursor_.prev() : cursor_.next(); }

    bool in_range(std::int64_t key) const {
        if (lo_.has && (key < lo_.value ||
                        (key == lo_.value && !lo_.inclusive))) {
            return false;
        }
        if (hi_.has && (key > hi_.value ||
                        (key == hi_.value && !hi_.inclusive))) {
            return false;
        }
        return true;
    }

    const Schema& schema_;
    bool reverse_ = false;
    ScanBound lo_;
    ScanBound hi_;
    bool empty_ = false;
    BTree::Cursor cursor_;
    bool started_ = false;
    bool done_ = false;
};

class FilterIterator : public RowIterator {
public:
    FilterIterator(std::unique_ptr<RowIterator> child,
                   std::vector<FilterPredicate> predicates)
        : child_(std::move(child)), predicates_(std::move(predicates)) {}

    bool next(std::vector<Value>& row) override {
        while (child_->next(row)) {
            bool matches = true;
            for (const FilterPredicate& predicate : predicates_) {
                const int cmp = row[static_cast<std::size_t>(predicate.column)]
                                    .compare(predicate.value);
                bool ok = false;
                if (predicate.op == "=") {
                    ok = cmp == 0;
                } else if (predicate.op == "!=") {
                    ok = cmp != 0;
                } else if (predicate.op == "<") {
                    ok = cmp < 0;
                } else if (predicate.op == "<=") {
                    ok = cmp <= 0;
                } else if (predicate.op == ">") {
                    ok = cmp > 0;
                } else if (predicate.op == ">=") {
                    ok = cmp >= 0;
                } else {
                    fail("unsupported comparison operator " + predicate.op);
                }
                if (!ok) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return true;
            }
        }
        return false;
    }

private:
    std::unique_ptr<RowIterator> child_;
    std::vector<FilterPredicate> predicates_;
};

class LimitIterator : public RowIterator {
public:
    LimitIterator(std::unique_ptr<RowIterator> child, std::uint64_t limit)
        : child_(std::move(child)), remaining_(limit) {}

    bool next(std::vector<Value>& row) override {
        if (remaining_ == 0) {
            return false;
        }
        if (!child_->next(row)) {
            return false;
        }
        --remaining_;
        return true;
    }

private:
    std::unique_ptr<RowIterator> child_;
    std::uint64_t remaining_;
};

struct JoinKey {
    ColumnType type = ColumnType::Int;
    std::int64_t integer = 0;
    std::uint64_t bits = 0;
    bool boolean = false;
    std::string text;

    static JoinKey from(const Value& value) {
        JoinKey key;
        key.type = value.type;
        switch (value.type) {
            case ColumnType::Int:
                key.integer = value.integer;
                break;
            case ColumnType::Double:
                key.bits = double_to_bits(value.real);
                break;
            case ColumnType::Bool:
                key.boolean = value.boolean;
                break;
            case ColumnType::Text:
                key.text = value.text;
                break;
        }
        return key;
    }

    bool operator==(const JoinKey& other) const {
        if (type != other.type) {
            return false;
        }
        switch (type) {
            case ColumnType::Int:
                return integer == other.integer;
            case ColumnType::Double:
                return bits == other.bits;
            case ColumnType::Bool:
                return boolean == other.boolean;
            case ColumnType::Text:
                return text == other.text;
        }
        return false;
    }
};

struct JoinKeyHash {
    std::size_t operator()(const JoinKey& key) const {
        std::size_t hash = std::hash<int>{}(static_cast<int>(key.type));
        switch (key.type) {
            case ColumnType::Int:
                return mix(hash, std::hash<std::int64_t>{}(key.integer));
            case ColumnType::Double:
                return mix(hash, std::hash<std::uint64_t>{}(key.bits));
            case ColumnType::Bool:
                return mix(hash, std::hash<bool>{}(key.boolean));
            case ColumnType::Text:
                return mix(hash, std::hash<std::string>{}(key.text));
        }
        return hash;
    }

private:
    static std::size_t mix(std::size_t hash, std::size_t value) {
        return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
    }
};

class HashJoinIterator : public RowIterator {
public:
    HashJoinIterator(std::unique_ptr<RowIterator> probe,
                     std::unique_ptr<RowIterator> build,
                     const Schema& build_schema, int probe_key, int build_key,
                     std::size_t memory_limit)
        : probe_(std::move(probe)),
          build_(std::move(build)),
          build_schema_(build_schema),
          probe_key_(probe_key),
          build_key_(build_key),
          limit_(memory_limit) {}

    bool next(std::vector<Value>& row) override {
        if (!built_) {
            build_table();
        }
        if (build_table_.empty()) {
            return false;
        }
        while (true) {
            if (matches_ != nullptr && match_index_ < matches_->size()) {
                row = probe_row_;
                const auto right =
                    build_schema_.decode((*matches_)[match_index_++]);
                row.insert(row.end(), right.begin(), right.end());
                return true;
            }
            matches_ = nullptr;
            match_index_ = 0;
            if (!probe_->next(probe_row_)) {
                return false;
            }
            const JoinKey key =
                JoinKey::from(probe_row_[static_cast<std::size_t>(probe_key_)]);
            const auto found = build_table_.find(key);
            if (found == build_table_.end()) {
                continue;
            }
            matches_ = &found->second;
        }
    }

private:
    void build_table() {
        built_ = true;
        std::vector<Value> row;
        while (build_->next(row)) {
            const JoinKey key =
                JoinKey::from(row[static_cast<std::size_t>(build_key_)]);
            std::size_t cost =
                static_cast<std::size_t>(build_schema_.row_size) + 128;
            if (key.type == ColumnType::Text) {
                cost += key.text.size();
            }
            if (memory_ + cost > limit_) {
                fail("hash join build side exceeds the " +
                     std::to_string(limit_) +
                     "-byte memory limit; increase join_buffer_bytes or use a "
                     "smaller build table");
            }
            memory_ += cost;
            build_table_[key].push_back(build_schema_.encode(row));
        }
        build_.reset();
    }

    std::unique_ptr<RowIterator> probe_;
    std::unique_ptr<RowIterator> build_;
    const Schema& build_schema_;
    int probe_key_ = 0;
    int build_key_ = 0;
    std::size_t limit_ = 0;
    std::size_t memory_ = 0;
    bool built_ = false;
    std::unordered_map<JoinKey, std::vector<std::vector<std::uint8_t>>,
                       JoinKeyHash>
        build_table_;
    std::vector<Value> probe_row_;
    const std::vector<std::vector<std::uint8_t>>* matches_ = nullptr;
    std::size_t match_index_ = 0;
};

class SortIterator : public RowIterator {
public:
    SortIterator(std::unique_ptr<RowIterator> child, const Schema& schema,
                 int key_column, bool descending, std::size_t buffer_bytes,
                 std::filesystem::path tmp_dir)
        : child_(std::move(child)),
          schema_(schema),
          key_column_(key_column),
          descending_(descending),
          limit_(std::max<std::size_t>(buffer_bytes, 64 * 1024)),
          tmp_dir_(std::move(tmp_dir)) {}

    ~SortIterator() override { cleanup(); }

    bool next(std::vector<Value>& row) override {
        if (!materialized_) {
            materialize();
        }
        if (in_memory_) {
            if (index_ >= buffer_.size()) {
                return false;
            }
            row = schema_.decode(buffer_[index_++]);
            return true;
        }
        int best = -1;
        for (std::size_t i = 0; i < runs_.size(); ++i) {
            if (!runs_[i].head.has_value()) {
                continue;
            }
            if (best < 0 ||
                less_bytes(*runs_[i].head, *runs_[static_cast<std::size_t>(best)].head)) {
                best = static_cast<int>(i);
            }
        }
        if (best < 0) {
            return false;
        }
        Run& run = runs_[static_cast<std::size_t>(best)];
        row = schema_.decode(*run.head);
        read_head(run);
        return true;
    }

private:
    struct Run {
        File file;
        std::string path;
        std::uint64_t offset = 0;
        std::optional<std::vector<std::uint8_t>> head;
    };

    void materialize() {
        std::vector<Value> row;
        while (child_->next(row)) {
            buffer_.push_back(schema_.encode(row));
            bytes_ += schema_.row_size;
            if (bytes_ >= limit_) {
                spill();
            }
        }
        if (runs_.empty()) {
            sort_buffer();
            in_memory_ = true;
        } else {
            if (!buffer_.empty()) {
                spill();
            }
            for (Run& run : runs_) {
                run.offset = 0;
                read_head(run);
            }
        }
        materialized_ = true;
    }

    void sort_buffer() {
        std::sort(buffer_.begin(), buffer_.end(),
                  [this](const auto& a, const auto& b) {
                      return less_bytes(a, b);
                  });
    }

    void spill() {
        sort_buffer();
        std::error_code ec;
        std::filesystem::create_directories(tmp_dir_, ec);
        if (ec) {
            fail("cannot create the sort directory: " + ec.message());
        }
        static std::uint64_t counter = 0;
        ++counter;
        Run run;
        run.path = (tmp_dir_ /
                    ("sort_" + std::to_string(counter) + "_" +
                     std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".run"))
                       .string();
        run.file = File::open(run.path);
        run.file.truncate(0);
        for (const auto& bytes : buffer_) {
            std::uint8_t header[2];
            store_u16(header, static_cast<std::uint16_t>(bytes.size()));
            run.file.append(header, sizeof(header));
            run.file.append(bytes.data(), bytes.size());
        }
        buffer_.clear();
        bytes_ = 0;
        runs_.push_back(std::move(run));
    }

    void read_head(Run& run) {
        if (run.offset + 2 > run.file.size()) {
            run.head.reset();
            return;
        }
        std::uint8_t header[2];
        run.file.read_at(run.offset, header, sizeof(header));
        const std::uint16_t length = load_u16(header);
        if (length == 0 || length > kPageSize ||
            run.offset + 2 + length > run.file.size()) {
            run.head.reset();
            return;
        }
        std::vector<std::uint8_t> bytes(length);
        run.file.read_at(run.offset + 2, bytes.data(), length);
        run.offset += 2 + length;
        run.head = std::move(bytes);
    }

    bool less_bytes(const std::vector<std::uint8_t>& a,
                    const std::vector<std::uint8_t>& b) const {
        const int cmp = schema_.value_at(a, key_column_)
                            .compare(schema_.value_at(b, key_column_));
        return descending_ ? cmp > 0 : cmp < 0;
    }

    void cleanup() {
        for (Run& run : runs_) {
            run.file.close();
            std::error_code ec;
            std::filesystem::remove(run.path, ec);
        }
        runs_.clear();
    }

    std::unique_ptr<RowIterator> child_;
    const Schema& schema_;
    int key_column_ = 0;
    bool descending_ = false;
    std::size_t limit_ = 0;
    std::filesystem::path tmp_dir_;
    std::vector<std::vector<std::uint8_t>> buffer_;
    std::size_t bytes_ = 0;
    std::vector<Run> runs_;
    bool materialized_ = false;
    bool in_memory_ = false;
    std::size_t index_ = 0;
};

Value coerce_value(const Value& value, const Column& column,
                   const std::string& context, bool for_storage) {
    if (column.type == ColumnType::Double && value.type == ColumnType::Int) {
        return Value::make_double(static_cast<double>(value.integer));
    }
    if (column.type != value.type) {
        fail("type mismatch for " + context + ": column " + column.name +
             " is " + column_type_name(column.type) + ", value is " +
             column_type_name(value.type));
    }
    if (column.type == ColumnType::Text && value.text.size() > column.length) {
        fail("text value is longer than " + std::to_string(column.length) +
             " bytes for column " + column.name);
    }
    if (for_storage && column.type == ColumnType::Text &&
        value.text.find('\0') != std::string::npos) {
        fail("text values cannot contain NUL bytes");
    }
    return value;
}

void derive_bounds(const Schema& schema,
                   const std::vector<Comparison>& comparisons, ScanBound& lo,
                   ScanBound& hi, bool& empty) {
    empty = false;
    for (const Comparison& comparison : comparisons) {
        const int column = schema.column_index(comparison.column);
        if (column != schema.pk_index || comparison.value.type != ColumnType::Int) {
            continue;
        }
        const std::int64_t value = comparison.value.integer;
        if (comparison.op == "=") {
            apply_lower(lo, value, true);
            apply_upper(hi, value, true);
        } else if (comparison.op == ">") {
            apply_lower(lo, value, false);
        } else if (comparison.op == ">=") {
            apply_lower(lo, value, true);
        } else if (comparison.op == "<") {
            apply_upper(hi, value, false);
        } else if (comparison.op == "<=") {
            apply_upper(hi, value, true);
        }
    }
    if (lo.has && hi.has &&
        (lo.value > hi.value ||
         (lo.value == hi.value && (!lo.inclusive || !hi.inclusive)))) {
        empty = true;
    }
}

std::unique_ptr<RowIterator> make_scan(BTree& tree, const Schema& schema,
                                       bool reverse, const ScanBound& lo,
                                       const ScanBound& hi, bool empty) {
    return std::make_unique<IndexScanIterator>(tree, schema, reverse, lo, hi, empty);
}

struct JoinSide {
    int side = 0;
    int index = 0;
};

JoinSide resolve_join_column(const TableDef& left, const TableDef& right,
                             const ColumnRef& reference, const char* clause) {
    const std::string qualifier = ascii_lower(reference.table);
    const std::string name = ascii_lower(reference.column);
    if (!qualifier.empty()) {
        if (qualifier == left.name) {
            const int index = left.schema.column_index(name);
            if (index < 0) {
                fail("no such column: " + reference.table + "." +
                     reference.column);
            }
            return {0, index};
        }
        if (qualifier == right.name) {
            const int index = right.schema.column_index(name);
            if (index < 0) {
                fail("no such column: " + reference.table + "." +
                     reference.column);
            }
            return {1, index};
        }
        fail("unknown table " + reference.table + " in " + clause);
    }
    const int left_index = left.schema.column_index(name);
    const int right_index = right.schema.column_index(name);
    if (left_index >= 0 && right_index >= 0) {
        fail("ambiguous column " + reference.column + " in " + clause +
             "; qualify it as table.column");
    }
    if (left_index >= 0) {
        return {0, left_index};
    }
    if (right_index >= 0) {
        return {1, right_index};
    }
    fail("no such column: " + reference.column);
}

}  // namespace

std::vector<QueryResult> Engine::execute(std::string_view sql,
                                         const RowSink& sink) {
    const std::vector<Statement> statements = parse_sql(sql);
    std::vector<QueryResult> results;
    results.reserve(statements.size());
    for (const Statement& statement : statements) {
        try {
            results.push_back(execute_statement(statement, sink));
        } catch (...) {
            if (active_txn_ != 0) {
                try {
                    rollback_txn();
                } catch (...) {
                }
            }
            throw;
        }
    }
    return results;
}

QueryResult Engine::execute_statement(const Statement& statement,
                                      const RowSink& sink) {
    switch (statement.kind) {
        case StatementKind::CreateTable:
            return create_table(statement);
        case StatementKind::DropTable:
            return drop_table(statement);
        case StatementKind::Insert:
            return insert_rows(statement, sink);
        case StatementKind::Select:
            return statement.join.has_value()
                       ? select_join_rows(statement, sink)
                       : select_rows(statement, sink);
        case StatementKind::Delete:
            return delete_rows(statement);
        case StatementKind::Begin: {
            if (active_txn_ != 0) {
                fail("a transaction is already open");
            }
            begin_txn();
            QueryResult result;
            result.message = "transaction started";
            return result;
        }
        case StatementKind::Commit: {
            if (active_txn_ == 0) {
                QueryResult result;
                result.message = "no active transaction";
                return result;
            }
            commit_txn();
            QueryResult result;
            result.message = "committed";
            return result;
        }
        case StatementKind::Rollback: {
            if (active_txn_ == 0) {
                QueryResult result;
                result.message = "no active transaction";
                return result;
            }
            rollback_txn();
            QueryResult result;
            result.message = "rolled back";
            return result;
        }
    }
    fail("unsupported statement");
}

QueryResult Engine::create_table(const Statement& statement) {
    if (active_txn_ != 0) {
        fail("CREATE TABLE is autocommit and cannot run inside a transaction");
    }
    if (catalog_.find(statement.table) != nullptr) {
        fail("table " + statement.table + " already exists");
    }

    Schema schema;
    int pk_index = -1;
    int pk_count = 0;
    for (const CreateColumn& definition : statement.create_columns) {
        Column column;
        column.name = definition.name;
        column.type = definition.type;
        column.length = definition.length;
        if (definition.primary_key) {
            ++pk_count;
            pk_index = static_cast<int>(schema.columns.size());
        }
        schema.columns.push_back(std::move(column));
    }
    if (pk_count != 1) {
        fail("exactly one PRIMARY KEY column is required");
    }
    schema.pk_index = pk_index;
    schema.finalize();

    const std::uint32_t table_id = next_table_id_;
    begin_txn();
    PageId meta_page = kInvalidPage;
    bool catalog_added = false;
    try {
        meta_page = BTree::create(*pool_, table_id);
        TableDef table;
        table.id = table_id;
        table.name = statement.table;
        table.schema = schema;
        table.meta_page = meta_page;
        catalog_.add(table);
        catalog_added = true;
        const std::vector<std::uint8_t> payload = serialize_table_def(table);
        wal_->append(RecordType::CreateTable, active_txn_, kInvalidPage,
                     table_id, 0, payload);
        commit_txn();
        next_table_id_ = table_id + 1;
    } catch (...) {
        if (catalog_added) {
            catalog_.remove_by_id(table_id);
        }
        if (active_txn_ != 0) {
            rollback_txn();
        }
        throw;
    }

    QueryResult result;
    result.message = "table " + statement.table + " created";
    return result;
}

QueryResult Engine::drop_table(const Statement& statement) {
    if (active_txn_ != 0) {
        fail("DROP TABLE is autocommit and cannot run inside a transaction");
    }
    TableDef* found = catalog_.find(statement.table);
    if (found == nullptr) {
        if (statement.if_exists) {
            QueryResult result;
            result.message = "table " + statement.table + " does not exist";
            return result;
        }
        fail("no such table: " + statement.table);
    }
    const TableDef table = *found;
    begin_txn();
    try {
        const std::vector<std::uint8_t> payload = serialize_table_def(table);
        wal_->append(RecordType::DropTable, active_txn_, kInvalidPage,
                     table.id, 0, payload);
        commit_txn();
        catalog_.remove_by_id(table.id);
    } catch (...) {
        if (active_txn_ != 0) {
            rollback_txn();
        }
        throw;
    }
    QueryResult result;
    result.message = "table " + statement.table + " dropped";
    return result;
}

QueryResult Engine::insert_rows(const Statement& statement, const RowSink&) {
    TableDef& table = require_table(statement.table);
    const Schema& schema = table.schema;

    std::vector<int> mapping;
    if (statement.insert_columns.empty()) {
        mapping.resize(schema.columns.size());
        for (std::size_t i = 0; i < mapping.size(); ++i) {
            mapping[i] = static_cast<int>(i);
        }
    } else {
        if (statement.insert_columns.size() != schema.columns.size()) {
            fail("INSERT must supply every column (this engine has no defaults)");
        }
        std::unordered_set<int> seen;
        for (const std::string& name : statement.insert_columns) {
            const int index = schema.column_index(name);
            if (index < 0) {
                fail("no such column: " + name);
            }
            if (!seen.insert(index).second) {
                fail("column " + name + " is listed twice");
            }
            mapping.push_back(index);
        }
    }

    struct EncodedRow {
        std::int64_t key = 0;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<EncodedRow> rows;
    rows.reserve(statement.insert_rows.size());
    std::unordered_set<std::int64_t> batch_keys;
    BTree tree(*pool_, table.meta_page);

    for (const std::vector<Value>& input : statement.insert_rows) {
        if (input.size() != mapping.size()) {
            fail("INSERT row has " + std::to_string(input.size()) +
                 " values but " + std::to_string(mapping.size()) +
                 " columns were listed");
        }
        std::vector<Value> full(schema.columns.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            const int target = mapping[i];
            full[static_cast<std::size_t>(target)] =
                coerce_value(input[i], schema.columns[static_cast<std::size_t>(target)],
                             schema.columns[static_cast<std::size_t>(target)].name, true);
        }
        const std::int64_t key =
            full[static_cast<std::size_t>(schema.pk_index)].as_int();
        if (!batch_keys.insert(key).second) {
            fail("duplicate primary key in INSERT: " + std::to_string(key));
        }
        if (tree.contains(key)) {
            fail("duplicate primary key: " + std::to_string(key));
        }
        rows.push_back({key, schema.encode(full)});
    }

    if (rows.empty()) {
        QueryResult result;
        result.message = "0 rows inserted";
        return result;
    }

    const bool owner = active_txn_ == 0;
    if (owner) {
        begin_txn();
    }
    try {
        for (const EncodedRow& row : rows) {
            wal_->append(RecordType::Insert, active_txn_, kInvalidPage,
                         table.id, row.key, row.bytes);
            if (!tree.insert(row.key, row.bytes)) {
                fail("duplicate primary key: " + std::to_string(row.key));
            }
        }
        if (owner) {
            commit_txn();
        }
    } catch (...) {
        if (owner && active_txn_ != 0) {
            rollback_txn();
        }
        throw;
    }

    QueryResult result;
    result.rows = rows.size();
    result.message = std::to_string(rows.size()) +
                     (rows.size() == 1 ? " row inserted" : " rows inserted");
    return result;
}

QueryResult Engine::select_rows(const Statement& statement, const RowSink& sink) {
    TableDef& table = require_table(statement.table);
    const Schema& schema = table.schema;

    const bool count = std::any_of(
        statement.select_items.begin(), statement.select_items.end(),
        [](const SelectItem& item) { return item.count_star; });
    if (count && statement.select_items.size() != 1) {
        fail("COUNT(*) cannot be combined with other select items");
    }

    std::vector<int> projection;
    std::vector<std::string> names;
    if (!count) {
        for (const SelectItem& item : statement.select_items) {
            if (item.star) {
                if (statement.select_items.size() != 1) {
                    fail("'*' cannot be combined with other select items");
                }
                projection.clear();
                names.clear();
                for (std::size_t i = 0; i < schema.columns.size(); ++i) {
                    projection.push_back(static_cast<int>(i));
                    names.push_back(schema.columns[i].name);
                }
                break;
            }
            if (!item.table.empty()) {
                const std::string qualifier = ascii_lower(item.table);
                if (qualifier != table.name) {
                    fail("unknown table " + item.table + " in SELECT");
                }
            }
            const int index = schema.column_index(item.column);
            if (index < 0) {
                fail("no such column: " + item.column);
            }
            projection.push_back(index);
            names.push_back(item.table.empty()
                                ? schema.columns[static_cast<std::size_t>(index)].name
                                : table.name + "." +
                                      schema.columns[static_cast<std::size_t>(index)].name);
        }
    }

    std::vector<FilterPredicate> filters;
    for (const Comparison& comparison : statement.where) {
        if (!comparison.table.empty() &&
            ascii_lower(comparison.table) != table.name) {
            fail("unknown table " + comparison.table + " in WHERE");
        }
        const int index = schema.column_index(comparison.column);
        if (index < 0) {
            fail("no such column: " + comparison.column);
        }
        filters.push_back({index, comparison.op,
                           coerce_value(comparison.value,
                                        schema.columns[static_cast<std::size_t>(index)],
                                        comparison.column, false)});
    }

    ScanBound lo;
    ScanBound hi;
    bool empty = false;
    derive_bounds(schema, statement.where, lo, hi, empty);

    int order_column = -1;
    bool descending = false;
    if (statement.order_by.has_value()) {
        if (!statement.order_by->table.empty() &&
            ascii_lower(statement.order_by->table) != table.name) {
            fail("unknown table " + statement.order_by->table + " in ORDER BY");
        }
        order_column = schema.column_index(statement.order_by->column);
        if (order_column < 0) {
            fail("no such column: " + statement.order_by->column);
        }
        descending = statement.order_by->descending;
    }
    const bool index_order = order_column == schema.pk_index;
    const bool reverse = index_order && descending;

    BTree tree(*pool_, table.meta_page);
    std::unique_ptr<RowIterator> current =
        make_scan(tree, schema, reverse, lo, hi, empty);
    if (!filters.empty()) {
        current = std::make_unique<FilterIterator>(std::move(current),
                                                   std::move(filters));
    }
    if (order_column >= 0 && !index_order && !count) {
        current = std::make_unique<SortIterator>(
            std::move(current), schema, order_column, descending,
            options_.sort_buffer_bytes, options_.directory / "tmp");
    }

    QueryResult result;
    if (count) {
        std::uint64_t total = 0;
        std::vector<Value> row;
        while (current->next(row)) {
            ++total;
        }
        result.columns = {"count"};
        if (!statement.limit.has_value() || *statement.limit > 0) {
            if (sink) {
                sink({Value::make_int(static_cast<std::int64_t>(total))});
            }
            result.rows = 1;
            result.message = "1 row";
        } else {
            result.message = "0 rows";
        }
        return result;
    }

    if (statement.limit.has_value()) {
        current = std::make_unique<LimitIterator>(std::move(current),
                                                  *statement.limit);
    }

    result.columns = names;
    bool all_columns = projection.size() == schema.columns.size();
    for (std::size_t i = 0; i < projection.size() && all_columns; ++i) {
        if (projection[i] != static_cast<int>(i)) {
            all_columns = false;
        }
    }
    std::vector<Value> row;
    while (current->next(row)) {
        if (all_columns) {
            if (sink) {
                sink(row);
            }
        } else {
            std::vector<Value> selected;
            selected.reserve(projection.size());
            for (const int index : projection) {
                selected.push_back(row[static_cast<std::size_t>(index)]);
            }
            if (sink) {
                sink(selected);
            }
        }
        ++result.rows;
    }
    result.message = std::to_string(result.rows) +
                     (result.rows == 1 ? " row" : " rows");
    return result;
}

QueryResult Engine::select_join_rows(const Statement& statement,
                                     const RowSink& sink) {
    TableDef& left = require_table(statement.table);
    TableDef& right = require_table(statement.join->table);
    const Schema& left_schema = left.schema;
    const Schema& right_schema = right.schema;
    const std::size_t left_count = left_schema.columns.size();

    const JoinSide on_left =
        resolve_join_column(left, right, statement.join->left, "JOIN ON");
    const JoinSide on_right =
        resolve_join_column(left, right, statement.join->right, "JOIN ON");
    if (on_left.side == on_right.side) {
        fail("JOIN ON must compare a column from each table");
    }
    const int left_key = on_left.side == 0 ? on_left.index : on_right.index;
    const int right_key = on_left.side == 1 ? on_left.index : on_right.index;
    if (left_schema.columns[static_cast<std::size_t>(left_key)].type !=
        right_schema.columns[static_cast<std::size_t>(right_key)].type) {
        fail("JOIN columns must have the same type");
    }

    Schema combined;
    combined.columns = left_schema.columns;
    combined.columns.insert(combined.columns.end(), right_schema.columns.begin(),
                            right_schema.columns.end());
    combined.offsets = left_schema.offsets;
    for (const std::uint16_t offset : right_schema.offsets) {
        combined.offsets.push_back(
            static_cast<std::uint16_t>(left_schema.row_size + offset));
    }
    combined.row_size = static_cast<std::uint16_t>(
        left_schema.row_size + right_schema.row_size);
    combined.pk_index = -1;

    const bool count = std::any_of(
        statement.select_items.begin(), statement.select_items.end(),
        [](const SelectItem& item) { return item.count_star; });
    if (count && statement.select_items.size() != 1) {
        fail("COUNT(*) cannot be combined with other select items");
    }

    std::vector<int> projection;
    std::vector<std::string> names;
    if (!count) {
        for (const SelectItem& item : statement.select_items) {
            if (item.star) {
                if (statement.select_items.size() != 1) {
                    fail("'*' cannot be combined with other select items");
                }
                projection.clear();
                names.clear();
                for (std::size_t i = 0; i < left_count; ++i) {
                    projection.push_back(static_cast<int>(i));
                    names.push_back(left.name + "." +
                                    left_schema.columns[i].name);
                }
                for (std::size_t i = 0; i < right_schema.columns.size(); ++i) {
                    projection.push_back(static_cast<int>(left_count + i));
                    names.push_back(right.name + "." +
                                    right_schema.columns[i].name);
                }
                break;
            }
            const JoinSide side = resolve_join_column(
                left, right, ColumnRef{item.table, item.column}, "SELECT");
            projection.push_back(
                side.side == 0 ? side.index
                               : static_cast<int>(left_count) + side.index);
            names.push_back(item.table.empty()
                                ? item.column
                                : item.table + "." + item.column);
        }
    }

    std::vector<FilterPredicate> filters;
    for (const Comparison& comparison : statement.where) {
        const JoinSide side = resolve_join_column(
            left, right, ColumnRef{comparison.table, comparison.column},
            "WHERE");
        const Column& column =
            side.side == 0
                ? left_schema.columns[static_cast<std::size_t>(side.index)]
                : right_schema.columns[static_cast<std::size_t>(side.index)];
        const int combined_index =
            side.side == 0 ? side.index
                           : static_cast<int>(left_count) + side.index;
        filters.push_back(
            {combined_index, comparison.op,
             coerce_value(comparison.value, column, comparison.column, false)});
    }

    int order_column = -1;
    bool descending = false;
    if (statement.order_by.has_value()) {
        const JoinSide side = resolve_join_column(
            left, right,
            ColumnRef{statement.order_by->table, statement.order_by->column},
            "ORDER BY");
        order_column = side.side == 0
                           ? side.index
                           : static_cast<int>(left_count) + side.index;
        descending = statement.order_by->descending;
    }

    BTree left_tree(*pool_, left.meta_page);
    BTree right_tree(*pool_, right.meta_page);
    const ScanBound no_bound;
    std::unique_ptr<RowIterator> left_scan =
        make_scan(left_tree, left_schema, false, no_bound, no_bound, false);
    std::unique_ptr<RowIterator> right_scan =
        make_scan(right_tree, right_schema, false, no_bound, no_bound, false);
    std::unique_ptr<RowIterator> current =
        std::make_unique<HashJoinIterator>(
            std::move(left_scan), std::move(right_scan), right_schema,
            left_key, right_key, options_.join_buffer_bytes);
    if (!filters.empty()) {
        current = std::make_unique<FilterIterator>(std::move(current),
                                                   std::move(filters));
    }
    if (order_column >= 0 && !count) {
        current = std::make_unique<SortIterator>(
            std::move(current), combined, order_column, descending,
            options_.sort_buffer_bytes, options_.directory / "tmp");
    }

    QueryResult result;
    if (count) {
        std::uint64_t total = 0;
        std::vector<Value> row;
        while (current->next(row)) {
            ++total;
        }
        result.columns = {"count"};
        if (!statement.limit.has_value() || *statement.limit > 0) {
            if (sink) {
                sink({Value::make_int(static_cast<std::int64_t>(total))});
            }
            result.rows = 1;
            result.message = "1 row";
        } else {
            result.message = "0 rows";
        }
        return result;
    }

    if (statement.limit.has_value()) {
        current = std::make_unique<LimitIterator>(std::move(current),
                                                  *statement.limit);
    }

    result.columns = names;
    bool all_columns = projection.size() == combined.columns.size();
    for (std::size_t i = 0; i < projection.size() && all_columns; ++i) {
        if (projection[i] != static_cast<int>(i)) {
            all_columns = false;
        }
    }
    std::vector<Value> row;
    while (current->next(row)) {
        if (all_columns) {
            if (sink) {
                sink(row);
            }
        } else {
            std::vector<Value> selected;
            selected.reserve(projection.size());
            for (const int index : projection) {
                selected.push_back(row[static_cast<std::size_t>(index)]);
            }
            if (sink) {
                sink(selected);
            }
        }
        ++result.rows;
    }
    result.message = std::to_string(result.rows) +
                     (result.rows == 1 ? " row" : " rows");
    return result;
}

QueryResult Engine::delete_rows(const Statement& statement) {
    TableDef& table = require_table(statement.table);
    const Schema& schema = table.schema;

    std::vector<FilterPredicate> filters;
    for (const Comparison& comparison : statement.where) {
        if (!comparison.table.empty() &&
            ascii_lower(comparison.table) != table.name) {
            fail("unknown table " + comparison.table + " in WHERE");
        }
        const int index = schema.column_index(comparison.column);
        if (index < 0) {
            fail("no such column: " + comparison.column);
        }
        filters.push_back({index, comparison.op,
                           coerce_value(comparison.value,
                                        schema.columns[static_cast<std::size_t>(index)],
                                        comparison.column, false)});
    }
    ScanBound lo;
    ScanBound hi;
    bool empty = false;
    derive_bounds(schema, statement.where, lo, hi, empty);

    BTree tree(*pool_, table.meta_page);
    std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> victims;
    {
        std::unique_ptr<RowIterator> current =
            make_scan(tree, schema, false, lo, hi, empty);
        if (!filters.empty()) {
            current = std::make_unique<FilterIterator>(std::move(current),
                                                       std::move(filters));
        }
        std::vector<Value> row;
        while (current->next(row)) {
            const std::int64_t key =
                row[static_cast<std::size_t>(schema.pk_index)].as_int();
            victims.emplace_back(key, schema.encode(row));
        }
    }

    if (victims.empty()) {
        QueryResult result;
        result.message = "0 rows deleted";
        return result;
    }

    const bool owner = active_txn_ == 0;
    if (owner) {
        begin_txn();
    }
    try {
        for (const auto& [key, before] : victims) {
            wal_->append(RecordType::Delete, active_txn_, kInvalidPage,
                         table.id, key, before);
            if (!tree.remove(key)) {
                fail("row disappeared during DELETE: " + std::to_string(key));
            }
        }
        if (owner) {
            commit_txn();
        }
    } catch (...) {
        if (owner && active_txn_ != 0) {
            rollback_txn();
        }
        throw;
    }

    QueryResult result;
    result.rows = victims.size();
    result.message = std::to_string(victims.size()) +
                     (victims.size() == 1 ? " row deleted" : " rows deleted");
    return result;
}

}  // namespace db
