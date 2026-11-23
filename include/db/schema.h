#pragma once

#include "db/common.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace db {

enum class ColumnType : std::uint8_t {
    Int = 1,
    Double = 2,
    Bool = 3,
    Text = 4,
};

std::string column_type_name(ColumnType type);
ColumnType parse_column_type(std::string_view name);

struct Value {
    ColumnType type = ColumnType::Int;
    std::int64_t integer = 0;
    double real = 0.0;
    bool boolean = false;
    std::string text;

    static Value make_int(std::int64_t v);
    static Value make_double(double v);
    static Value make_bool(bool v);
    static Value make_text(std::string v);

    std::string to_string() const;
    std::int64_t as_int() const;
    double as_double() const;
    bool as_bool() const;
    bool is_null_like() const { return false; }

    bool equals(const Value& other) const;
    int compare(const Value& other) const;
};

struct Column {
    std::string name;
    ColumnType type = ColumnType::Int;
    std::uint16_t length = 0;
};

struct Schema {
    std::vector<Column> columns;
    std::vector<std::uint16_t> offsets;
    std::uint16_t row_size = 0;
    int pk_index = -1;

    void finalize();
    int column_index(std::string_view name) const;
    const Column& column(int index) const;
    const Column& primary_key() const;

    std::vector<std::uint8_t> encode(const std::vector<Value>& row) const;
    std::vector<Value> decode(std::span<const std::uint8_t> bytes) const;
    Value value_at(std::span<const std::uint8_t> bytes, int column) const;

    std::vector<std::uint8_t> serialize() const;
    static Schema deserialize(std::span<const std::uint8_t> bytes,
                              std::size_t& consumed);
};

std::string ascii_lower(std::string_view text);

}  // namespace db
