#include "db/sql.h"

#include <charconv>
#include <cctype>
#include <cstdlib>
#include <limits>

namespace db {
namespace {

enum class TokenKind {
    Identifier,
    Number,
    String,
    Symbol,
    End,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    std::size_t offset = 0;
};

bool is_identifier_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_identifier_char(char c) {
    return is_identifier_start(c) || (c >= '0' && c <= '9');
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

[[noreturn]] void syntax_error(const Token& token, const std::string& what) {
    fail("SQL syntax error at offset " + std::to_string(token.offset) +
         (token.kind == TokenKind::End ? " at end of input" : " near '" + token.text + "'") +
         ": " + what);
}

std::vector<Token> lex(std::string_view text) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (is_identifier_start(c)) {
            const std::size_t start = i;
            ++i;
            while (i < text.size() && is_identifier_char(text[i])) {
                ++i;
            }
            tokens.push_back({TokenKind::Identifier,
                              std::string(text.substr(start, i - start)), start});
            continue;
        }
        if (is_digit(c) || (c == '.' && i + 1 < text.size() && is_digit(text[i + 1]))) {
            const std::size_t start = i;
            bool seen_dot = false;
            if (c == '.') {
                seen_dot = true;
                ++i;
            }
            while (i < text.size() && (is_digit(text[i]) || (text[i] == '.' && !seen_dot))) {
                if (text[i] == '.') {
                    seen_dot = true;
                }
                ++i;
            }
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                ++i;
                if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
                    ++i;
                }
                if (i >= text.size() || !is_digit(text[i])) {
                    tokens.push_back({TokenKind::Number,
                                      std::string(text.substr(start, i - start)), start});
                    continue;
                }
                while (i < text.size() && is_digit(text[i])) {
                    ++i;
                }
            }
            tokens.push_back({TokenKind::Number,
                              std::string(text.substr(start, i - start)), start});
            continue;
        }
        if (c == '\'') {
            const std::size_t start = i;
            ++i;
            std::string value;
            bool closed = false;
            while (i < text.size()) {
                if (text[i] == '\'') {
                    if (i + 1 < text.size() && text[i + 1] == '\'') {
                        value.push_back('\'');
                        i += 2;
                        continue;
                    }
                    ++i;
                    closed = true;
                    break;
                }
                value.push_back(text[i]);
                ++i;
            }
            if (!closed) {
                fail("SQL syntax error: unterminated string literal at offset " +
                     std::to_string(start));
            }
            tokens.push_back({TokenKind::String, std::move(value), start});
            continue;
        }
        std::string symbol(1, c);
        if ((c == '<' || c == '>' || c == '!') && i + 1 < text.size() &&
            text[i + 1] == '=') {
            symbol.push_back(text[i + 1]);
            i += 2;
        } else if (c == '<' && i + 1 < text.size() && text[i + 1] == '>') {
            symbol = "<>";
            i += 2;
        } else {
            ++i;
        }
        if (symbol == "(" || symbol == ")" || symbol == "," || symbol == ";" ||
            symbol == "*" || symbol == "." || symbol == "=" || symbol == "<" ||
            symbol == ">" || symbol == "<=" || symbol == ">=" || symbol == "!=" ||
            symbol == "<>" || symbol == "-" || symbol == "+") {
            tokens.push_back({TokenKind::Symbol, std::move(symbol), i - symbol.size()});
            continue;
        }
        fail("SQL syntax error: unexpected character at offset " +
             std::to_string(i - 1));
    }
    tokens.push_back({TokenKind::End, "", text.size()});
    return tokens;
}

class Parser {
public:
    explicit Parser(std::string_view text) : tokens_(lex(text)) {}

    std::vector<Statement> parse_all() {
        std::vector<Statement> statements;
        while (!at_end()) {
            if (match_symbol(";")) {
                continue;
            }
            statements.push_back(parse_statement());
            if (!at_end() && !match_symbol(";")) {
                syntax_error(current(), "expected ';' or end of input");
            }
        }
        return statements;
    }

private:
    const Token& current() const { return tokens_[pos_]; }

    const Token& peek(std::size_t ahead = 1) const {
        const std::size_t index = pos_ + ahead;
        return index < tokens_.size() ? tokens_[index] : tokens_.back();
    }

    bool at_end() const { return current().kind == TokenKind::End; }

    static bool is_word(const Token& token, std::string_view word) {
        return token.kind == TokenKind::Identifier && ascii_lower(token.text) == word;
    }

    bool match_word(std::string_view word) {
        if (is_word(current(), word)) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect_word(std::string_view word) {
        if (!match_word(word)) {
            syntax_error(current(), "expected " + std::string(word));
        }
    }

    std::string expect_identifier() {
        if (current().kind != TokenKind::Identifier) {
            syntax_error(current(), "expected an identifier");
        }
        std::string name = ascii_lower(current().text);
        ++pos_;
        return name;
    }

    ColumnRef parse_column_ref() {
        ColumnRef reference;
        reference.column = expect_identifier();
        if (match_symbol(".")) {
            reference.table = reference.column;
            reference.column = expect_identifier();
        }
        return reference;
    }

    bool match_symbol(std::string_view symbol) {
        if (current().kind == TokenKind::Symbol && current().text == symbol) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect_symbol(std::string_view symbol) {
        if (!match_symbol(symbol)) {
            syntax_error(current(), "expected '" + std::string(symbol) + "'");
        }
    }

    Statement parse_statement() {
        if (match_word("create")) {
            return parse_create();
        }
        if (match_word("drop")) {
            return parse_drop();
        }
        if (match_word("insert")) {
            return parse_insert();
        }
        if (match_word("select")) {
            return parse_select();
        }
        if (match_word("delete")) {
            return parse_delete();
        }
        if (match_word("begin")) {
            match_word("transaction");
            match_word("work");
            Statement statement;
            statement.kind = StatementKind::Begin;
            return statement;
        }
        if (match_word("commit") || match_word("end")) {
            match_word("transaction");
            match_word("work");
            Statement statement;
            statement.kind = StatementKind::Commit;
            return statement;
        }
        if (match_word("rollback")) {
            match_word("transaction");
            match_word("work");
            Statement statement;
            statement.kind = StatementKind::Rollback;
            return statement;
        }
        syntax_error(current(), "unknown statement");
    }

    Statement parse_create() {
        expect_word("table");
        Statement statement;
        statement.kind = StatementKind::CreateTable;
        statement.table = expect_identifier();
        expect_symbol("(");
        while (true) {
            CreateColumn column;
            column.name = expect_identifier();
            if (current().kind != TokenKind::Identifier) {
                syntax_error(current(), "expected a column type");
            }
            column.type = parse_column_type(current().text);
            ++pos_;
            if (match_symbol("(")) {
                const std::uint64_t length = parse_unsigned();
                if (length == 0 || length > 255) {
                    syntax_error(current(), "TEXT length must be between 1 and 255");
                }
                column.length = static_cast<std::uint16_t>(length);
                expect_symbol(")");
            } else if (column.type == ColumnType::Text) {
                column.length = 255;
            }
            if (match_word("primary")) {
                expect_word("key");
                column.primary_key = true;
            }
            statement.create_columns.push_back(std::move(column));
            if (!match_symbol(",")) {
                break;
            }
        }
        expect_symbol(")");
        return statement;
    }

    Statement parse_drop() {
        expect_word("table");
        Statement statement;
        statement.kind = StatementKind::DropTable;
        if (match_word("if")) {
            expect_word("exists");
            statement.if_exists = true;
        }
        statement.table = expect_identifier();
        return statement;
    }

    Statement parse_insert() {
        expect_word("into");
        Statement statement;
        statement.kind = StatementKind::Insert;
        statement.table = expect_identifier();
        if (match_symbol("(")) {
            while (true) {
                statement.insert_columns.push_back(expect_identifier());
                if (!match_symbol(",")) {
                    break;
                }
            }
            expect_symbol(")");
        }
        expect_word("values");
        while (true) {
            expect_symbol("(");
            std::vector<Value> row;
            if (!match_symbol(")")) {
                while (true) {
                    row.push_back(parse_literal());
                    if (!match_symbol(",")) {
                        break;
                    }
                }
                expect_symbol(")");
            }
            statement.insert_rows.push_back(std::move(row));
            if (!match_symbol(",")) {
                break;
            }
        }
        return statement;
    }

    Statement parse_select() {
        Statement statement;
        statement.kind = StatementKind::Select;
        while (true) {
            SelectItem item;
            if (match_symbol("*")) {
                item.star = true;
            } else if (is_word(current(), "count") && peek().text == "(") {
                ++pos_;
                expect_symbol("(");
                expect_symbol("*");
                expect_symbol(")");
                item.count_star = true;
            } else {
                const ColumnRef reference = parse_column_ref();
                item.table = reference.table;
                item.column = reference.column;
            }
            statement.select_items.push_back(std::move(item));
            if (!match_symbol(",")) {
                break;
            }
        }
        expect_word("from");
        statement.table = expect_identifier();
        if (match_word("inner")) {
            expect_word("join");
            parse_join(statement);
        } else if (match_word("join")) {
            parse_join(statement);
        }
        if (match_word("where")) {
            parse_where(statement);
        }
        if (match_word("order")) {
            expect_word("by");
            OrderBy order;
            const ColumnRef reference = parse_column_ref();
            order.table = reference.table;
            order.column = reference.column;
            if (match_word("desc")) {
                order.descending = true;
            } else {
                match_word("asc");
            }
            statement.order_by = std::move(order);
        }
        if (match_word("limit")) {
            statement.limit = parse_unsigned();
        }
        return statement;
    }

    void parse_join(Statement& statement) {
        JoinClause join;
        join.table = expect_identifier();
        expect_word("on");
        join.left = parse_column_ref();
        expect_symbol("=");
        join.right = parse_column_ref();
        statement.join = std::move(join);
    }

    Statement parse_delete() {
        expect_word("from");
        Statement statement;
        statement.kind = StatementKind::Delete;
        statement.table = expect_identifier();
        if (match_word("where")) {
            parse_where(statement);
        }
        return statement;
    }

    void parse_where(Statement& statement) {
        while (true) {
            Comparison comparison;
            const bool literal_first = current().kind == TokenKind::Number ||
                                       current().kind == TokenKind::String ||
                                       is_bool_word(current());
            if (literal_first) {
                const Value value = parse_literal();
                comparison.value = value;
                comparison.op = parse_operator();
                const ColumnRef reference = parse_column_ref();
                comparison.table = reference.table;
                comparison.column = reference.column;
                comparison.op = reverse_operator(comparison.op);
            } else {
                const ColumnRef reference = parse_column_ref();
                comparison.table = reference.table;
                comparison.column = reference.column;
                comparison.op = parse_operator();
                comparison.value = parse_literal();
            }
            statement.where.push_back(std::move(comparison));
            if (!match_word("and")) {
                break;
            }
        }
    }

    static bool is_bool_word(const Token& token) {
        return is_word(token, "true") || is_word(token, "false");
    }

    static std::string reverse_operator(std::string op) {
        if (op == "<") {
            return ">";
        }
        if (op == "<=") {
            return ">=";
        }
        if (op == ">") {
            return "<";
        }
        if (op == ">=") {
            return "<=";
        }
        return op;
    }

    std::string parse_operator() {
        if (current().kind != TokenKind::Symbol) {
            syntax_error(current(), "expected a comparison operator");
        }
        const std::string op = current().text;
        if (op != "=" && op != "!=" && op != "<>" && op != "<" && op != "<=" &&
            op != ">" && op != ">=") {
            syntax_error(current(), "unknown comparison operator");
        }
        ++pos_;
        return op == "<>" ? "!=" : op;
    }

    Value parse_literal() {
        if (current().kind == TokenKind::String) {
            Value value = Value::make_text(current().text);
            ++pos_;
            return value;
        }
        if (current().kind == TokenKind::Number) {
            const std::string number = current().text;
            ++pos_;
            return parse_number(number);
        }
        if (match_symbol("-")) {
            if (current().kind != TokenKind::Number) {
                syntax_error(current(), "expected a number after '-'");
            }
            const std::string number = current().text;
            ++pos_;
            Value value = parse_number(number);
            if (value.type == ColumnType::Int) {
                value.integer = -value.integer;
            } else {
                value.real = -value.real;
            }
            return value;
        }
        if (match_word("true")) {
            return Value::make_bool(true);
        }
        if (match_word("false")) {
            return Value::make_bool(false);
        }
        syntax_error(current(), "expected a literal");
    }

    static Value parse_number(const std::string& text) {
        const bool floating = text.find('.') != std::string::npos ||
                              text.find('e') != std::string::npos ||
                              text.find('E') != std::string::npos;
        if (!floating) {
            std::int64_t value = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
                fail("integer literal out of range: " + text);
            }
            return Value::make_int(value);
        }
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            fail("invalid floating point literal: " + text);
        }
        return Value::make_double(value);
    }

    std::uint64_t parse_unsigned() {
        if (current().kind != TokenKind::Number) {
            syntax_error(current(), "expected an unsigned integer");
        }
        const std::string text = current().text;
        ++pos_;
        if (text.find('.') != std::string::npos || text.find('e') != std::string::npos ||
            text.find('E') != std::string::npos) {
            syntax_error(current(), "expected an unsigned integer");
        }
        std::uint64_t value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
            fail("unsigned integer literal out of range: " + text);
        }
        return value;
    }

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

}  // namespace

std::vector<Statement> parse_sql(std::string_view text) {
    Parser parser(text);
    return parser.parse_all();
}

}  // namespace db
