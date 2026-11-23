#include "db/sql.h"
#include "test.h"

using namespace db;

TEST(parser_create_and_types) {
    const auto statements = parse_sql(
        "CREATE TABLE Users (ID INT PRIMARY KEY, name TEXT(32), score DOUBLE, "
        "active BOOL);");
    CHECK_EQ(statements.size(), 1u);
    const Statement& statement = statements[0];
    CHECK(statement.kind == StatementKind::CreateTable);
    CHECK_EQ(statement.table, "users");
    CHECK_EQ(statement.create_columns.size(), 4u);
    CHECK_EQ(statement.create_columns[0].name, "id");
    CHECK(statement.create_columns[0].primary_key);
    CHECK(statement.create_columns[1].type == ColumnType::Text);
    CHECK_EQ(statement.create_columns[1].length, 32u);
    CHECK(statement.create_columns[2].type == ColumnType::Double);
    CHECK(statement.create_columns[3].type == ColumnType::Bool);
}

TEST(parser_insert_values) {
    const auto statements = parse_sql(
        "INSERT INTO t (id, name) VALUES (1, 'a'), (-2, 'it''s');");
    const Statement& statement = statements[0];
    CHECK(statement.kind == StatementKind::Insert);
    CHECK_EQ(statement.insert_columns.size(), 2u);
    CHECK_EQ(statement.insert_rows.size(), 2u);
    CHECK_EQ(statement.insert_rows[0][0].as_int(), 1);
    CHECK_EQ(statement.insert_rows[1][0].as_int(), -2);
    CHECK_EQ(statement.insert_rows[1][1].text, "it's");
}

TEST(parser_select_where_order_limit) {
    const auto statements = parse_sql(
        "SELECT id, name FROM t WHERE id >= 5 AND 'zzz' > name "
        "ORDER BY name DESC LIMIT 3;");
    const Statement& statement = statements[0];
    CHECK(statement.kind == StatementKind::Select);
    CHECK_EQ(statement.select_items.size(), 2u);
    CHECK_EQ(statement.where.size(), 2u);
    CHECK_EQ(statement.where[0].column, "id");
    CHECK_EQ(statement.where[0].op, ">=");
    CHECK_EQ(statement.where[0].value.as_int(), 5);
    CHECK_EQ(statement.where[1].column, "name");
    CHECK_EQ(statement.where[1].op, "<");
    CHECK(statement.order_by.has_value());
    CHECK_EQ(statement.order_by->column, "name");
    CHECK(statement.order_by->descending);
    CHECK_EQ(*statement.limit, 3u);
}

TEST(parser_count_and_delete) {
    auto statements = parse_sql("SELECT COUNT(*) FROM t; DELETE FROM t WHERE id = 9;");
    CHECK_EQ(statements.size(), 2u);
    CHECK(statements[0].select_items[0].count_star);
    CHECK(statements[1].kind == StatementKind::Delete);
    CHECK_EQ(statements[1].where[0].value.as_int(), 9);
}

TEST(parser_transactions) {
    const auto statements = parse_sql(
        "BEGIN TRANSACTION; INSERT INTO t VALUES (1); COMMIT; ROLLBACK WORK;");
    CHECK_EQ(statements.size(), 4u);
    CHECK(statements[0].kind == StatementKind::Begin);
    CHECK(statements[1].kind == StatementKind::Insert);
    CHECK(statements[2].kind == StatementKind::Commit);
    CHECK(statements[3].kind == StatementKind::Rollback);
}

TEST(parser_join) {
    const auto statements = parse_sql(
        "SELECT a.id, b.v FROM a INNER JOIN b ON a.id = b.id "
        "WHERE b.v > 3 ORDER BY b.v DESC LIMIT 2;");
    CHECK_EQ(statements.size(), 1u);
    const Statement& statement = statements[0];
    CHECK(statement.join.has_value());
    CHECK_EQ(statement.join->table, "b");
    CHECK_EQ(statement.join->left.table, "a");
    CHECK_EQ(statement.join->left.column, "id");
    CHECK_EQ(statement.join->right.table, "b");
    CHECK_EQ(statement.join->right.column, "id");
    CHECK_EQ(statement.select_items[0].table, "a");
    CHECK_EQ(statement.select_items[0].column, "id");
    CHECK_EQ(statement.select_items[1].table, "b");
    CHECK_EQ(statement.select_items[1].column, "v");
    CHECK_EQ(statement.where[0].table, "b");
    CHECK_EQ(statement.where[0].column, "v");
    CHECK_EQ(statement.order_by->table, "b");
    CHECK_EQ(statement.order_by->column, "v");
    CHECK_EQ(*statement.limit, 2u);

    const auto plain = parse_sql("SELECT * FROM a JOIN b ON a.id = b.id;");
    CHECK(plain[0].join.has_value());
    CHECK_THROWS(parse_sql("SELECT * FROM a JOIN b"));
    CHECK_THROWS(parse_sql("SELECT * FROM a JOIN b ON a.id != b.id"));
}

TEST(parser_errors) {
    CHECK_THROWS(parse_sql("SELECT FROM t"));
    CHECK_THROWS(parse_sql("CREATE TABLE t (id INT PRIMARY KEY"));
    CHECK_THROWS(parse_sql("SELECT * FROM t WHERE id = "));
    CHECK_THROWS(parse_sql("INSERT INTO t VALUES ('unterminated)"));
    CHECK_THROWS(parse_sql("CREATE TABLE t (id WAT PRIMARY KEY)"));
    CHECK_THROWS(parse_sql("SELECT * FROM t LIMIT -1"));
    CHECK_THROWS(parse_sql("nonsense"));
}
