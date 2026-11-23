#include "db/schema.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>

namespace db {
namespace {

constexpr std::uint16_t kMaxTextLength = 255;
constexpr std::uint16_t kMaxRowSize = 2048;

void expect_layout(bool condition, const char* message) {
    if (!condition) {
        fail(std::string("invalid schema: ") + message);
    }
}

}  // namespace

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string column_type_name(ColumnType type) {
    switch (type) {
        case ColumnType::Int:
            return "INT";
        case ColumnType::Double:
            return "DOUBLE";
        case ColumnType::Bool:
            return "BOOL";
        case ColumnType::Text:
            return "TEXT";
    }
    return "?";
}

ColumnType parse_column_type(std::string_view name) {
    const std::string lower = ascii_lower(name);
    if (lower == "int" || lower == "integer" || lower == "bigint") {
        return ColumnType::Int;
    }
    if (lower == "double" || lower == "real" || lower == "float") {
        return ColumnType::Double;
    }
    if (lower == "bool" || lower == "boolean") {
        return ColumnType::Bool;
    }
    if (lower == "text" || lower == "varchar" || lower == "string") {
        return ColumnType::Text;
    }
    fail("unknown column type " + std::string(name));
}

Value Value::make_int(std::int64_t v) {
    Value value;
    value.type = ColumnType::Int;
    value.integer = v;
    return value;
}

Value Value::make_double(double v) {
    Value value;
    value.type = ColumnType::Double;
    value.real = v;
    return value;
}

Value Value::make_bool(bool v) {
    Value value;
    value.type = ColumnType::Bool;
    value.boolean = v;
    return value;
}

Value Value::make_text(std::string v) {
    Value value;
    value.type = ColumnType::Text;
    value.text = std::move(v);
    return value;
}

std::string Value::to_string() const {
    switch (type) {
        case ColumnType::Int:
            return std::to_string(integer);
        case ColumnType::Double: {
            std::array<char, 64> buffer{};
            const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                              real);
            if (result.ec == std::errc()) {
                return std::string(buffer.data(), result.ptr);
            }
            return "?";
        }
        case ColumnType::Bool:
            return boolean ? "true" : "false";
        case ColumnType::Text:
            return text;
    }
    return "?";
}

std::int64_t Value::as_int() const {
    if (type != ColumnType::Int) {
        fail("value is not an integer");
    }
    return integer;
}

double Value::as_double() const {
    if (type == ColumnType::Double) {
        return real;
    }
    if (type == ColumnType::Int) {
        return static_cast<double>(integer);
    }
    fail("value is not numeric");
}

bool Value::as_bool() const {
    if (type != ColumnType::Bool) {
        fail("value is not a boolean");
    }
    return boolean;
}

bool Value::equals(const Value& other) const {
    if (type != other.type) {
        return false;
    }
    switch (type) {
        case ColumnType::Int:
            return integer == other.integer;
        case ColumnType::Double:
            return real == other.real;
        case ColumnType::Bool:
            return boolean == other.boolean;
        case ColumnType::Text:
            return text == other.text;
    }
    return false;
}

int Value::compare(const Value& other) const {
    if (type != other.type) {
        fail("cannot compare values of different types");
    }
    switch (type) {
        case ColumnType::Int:
            return integer < other.integer ? -1 : (integer > other.integer ? 1 : 0);
        case ColumnType::Double:
            return real < other.real ? -1 : (real > other.real ? 1 : 0);
        case ColumnType::Bool:
            return boolean == other.boolean ? 0 : (boolean ? 1 : -1);
        case ColumnType::Text:
            return text < other.text ? -1 : (text > other.text ? 1 : 0);
    }
    return 0;
}

void Schema::finalize() {
    expect_layout(!columns.empty(), "a table needs at least one column");
    expect_layout(pk_index >= 0 &&
                      pk_index < static_cast<int>(columns.size()),
                  "missing primary key");
    offsets.clear();
    offsets.reserve(columns.size());
    std::uint32_t offset = 0;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        Column& column = columns[i];
        column.name = ascii_lower(column.name);
        expect_layout(!column.name.empty(), "empty column name");
        for (std::size_t j = 0; j < i; ++j) {
            expect_layout(columns[j].name != column.name,
                          "duplicate column name");
        }
        std::uint16_t width = 0;
        switch (column.type) {
            case ColumnType::Int:
            case ColumnType::Double:
                width = 8;
                break;
            case ColumnType::Bool:
                width = 1;
                break;
            case ColumnType::Text:
                expect_layout(column.length > 0 && column.length <= kMaxTextLength,
                              "TEXT length must be between 1 and 255");
                width = column.length;
                break;
        }
        expect_layout(offset + width <= kMaxRowSize, "row is too wide");
        offsets.push_back(static_cast<std::uint16_t>(offset));
        offset += width;
    }
    row_size = static_cast<std::uint16_t>(offset);
    const Column& key = columns[static_cast<std::size_t>(pk_index)];
    expect_layout(key.type == ColumnType::Int, "primary key must be INT");
}

int Schema::column_index(std::string_view name) const {
    const std::string lower = ascii_lower(name);
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].name == lower) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const Column& Schema::column(int index) const {
    if (index < 0 || index >= static_cast<int>(columns.size())) {
        fail("column index out of range");
    }
    return columns[static_cast<std::size_t>(index)];
}

const Column& Schema::primary_key() const {
    return column(pk_index);
}

std::vector<std::uint8_t> Schema::encode(const std::vector<Value>& row) const {
    if (row.size() != columns.size()) {
        fail("row has the wrong number of values");
    }
    std::vector<std::uint8_t> out(row_size, 0);
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const Column& column = columns[i];
        const Value& value = row[i];
        std::uint8_t* dest = out.data() + offsets[i];
        switch (column.type) {
            case ColumnType::Int:
                if (value.type != ColumnType::Int) {
                    fail("column " + column.name + " expects an integer");
                }
                store_i64(dest, value.integer);
                break;
            case ColumnType::Double:
                if (value.type != ColumnType::Double) {
                    fail("column " + column.name + " expects a double");
                }
                store_u64(dest, double_to_bits(value.real));
                break;
            case ColumnType::Bool:
                if (value.type != ColumnType::Bool) {
                    fail("column " + column.name + " expects a boolean");
                }
                dest[0] = value.boolean ? 1 : 0;
                break;
            case ColumnType::Text:
                if (value.type != ColumnType::Text) {
                    fail("column " + column.name + " expects text");
                }
                if (value.text.size() > column.length) {
                    fail("text value is longer than " + std::to_string(column.length) +
                         " bytes for column " + column.name);
                }
                if (value.text.find('\0') != std::string::npos) {
                    fail("text values cannot contain NUL bytes");
                }
                std::memcpy(dest, value.text.data(), value.text.size());
                break;
        }
    }
    return out;
}

std::vector<Value> Schema::decode(std::span<const std::uint8_t> bytes) const {
    if (bytes.size() < row_size) {
        fail("row is shorter than the schema");
    }
    std::vector<Value> row;
    row.reserve(columns.size());
    for (std::size_t i = 0; i < columns.size(); ++i) {
        row.push_back(value_at(bytes, static_cast<int>(i)));
    }
    return row;
}

Value Schema::value_at(std::span<const std::uint8_t> bytes, int column) const {
    if (bytes.size() < row_size) {
        fail("row is shorter than the schema");
    }
    if (column < 0 || column >= static_cast<int>(columns.size())) {
        fail("column index out of range");
    }
    const Column& col = columns[static_cast<std::size_t>(column)];
    const std::uint8_t* src = bytes.data() + offsets[static_cast<std::size_t>(column)];
    switch (col.type) {
        case ColumnType::Int:
            return Value::make_int(load_i64(src));
        case ColumnType::Double:
            return Value::make_double(bits_to_double(load_u64(src)));
        case ColumnType::Bool:
            return Value::make_bool(src[0] != 0);
        case ColumnType::Text: {
            std::size_t length = 0;
            while (length < col.length && src[length] != 0) {
                ++length;
            }
            return Value::make_text(std::string(reinterpret_cast<const char*>(src), length));
        }
    }
    fail("unknown column type");
}

std::vector<std::uint8_t> Schema::serialize() const {
    std::vector<std::uint8_t> out;
    out.reserve(64);
    const auto put16 = [&out](std::uint16_t v) {
        std::uint8_t bytes[2];
        store_u16(bytes, v);
        out.insert(out.end(), bytes, bytes + 2);
    };
    const auto put8 = [&out](std::uint8_t v) { out.push_back(v); };

    put16(static_cast<std::uint16_t>(columns.size()));
    for (const Column& column : columns) {
        put8(static_cast<std::uint8_t>(column.type));
        put16(column.length);
        put8(static_cast<std::uint8_t>(column.name.size()));
        out.insert(out.end(), column.name.begin(), column.name.end());
    }
    put16(static_cast<std::uint16_t>(pk_index));
    return out;
}

Schema Schema::deserialize(std::span<const std::uint8_t> bytes,
                           std::size_t& consumed) {
    std::size_t pos = 0;
    const auto need = [&](std::size_t n) {
        if (pos + n > bytes.size()) {
            fail("truncated schema");
        }
    };
    need(2);
    const std::uint16_t count = load_u16(bytes.data() + pos);
    pos += 2;
    Schema schema;
    schema.columns.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        need(4);
        Column column;
        column.type = static_cast<ColumnType>(bytes[pos]);
        column.length = load_u16(bytes.data() + pos + 1);
        const std::uint8_t name_length = bytes[pos + 3];
        pos += 4;
        need(name_length);
        column.name.assign(reinterpret_cast<const char*>(bytes.data() + pos),
                           name_length);
        pos += name_length;
        schema.columns.push_back(std::move(column));
    }
    need(2);
    schema.pk_index = static_cast<std::int16_t>(load_u16(bytes.data() + pos));
    pos += 2;
    schema.finalize();
    consumed = pos;
    return schema;
}

}  // namespace db
