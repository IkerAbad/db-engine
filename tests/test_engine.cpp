#include "db/engine.h"
#include "temp_dir.h"
#include "test.h"

#include <string>
#include <vector>

using namespace db;

namespace {

EngineOptions options_for(const test::TempDir& dir) {
    EngineOptions options;
    options.directory = dir.path();
    options.buffer_pages = 64;
    options.sort_buffer_bytes = 64 * 1024;
    return options;
}

std::vector<std::vector<Value>> collect(Engine& engine, const std::string& sql) {
    std::vector<std::vector<Value>> rows;
    engine.execute(sql, [&](const std::vector<Value>& row) { rows.push_back(row); });
    return rows;
}

std::int64_t count(Engine& engine, const std::string& table) {
    const auto rows = collect(engine, "SELECT COUNT(*) FROM " + table);
    CHECK_EQ(rows.size(), 1u);
    return rows[0][0].as_int();
}

void require_check(Engine& engine) {
    std::string error;
    if (!engine.check(error)) {
        throw test::Failure(__FILE__, __LINE__, "engine check: " + error);
    }
}

}  // namespace

TEST(engine_crud_and_persistence) {
    test::TempDir dir("engine-crud");
    {
        Engine engine(options_for(dir));
        engine.open();
        engine.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT(16), "
                       "score DOUBLE)",
                       nullptr);
        engine.execute("INSERT INTO users VALUES (1, 'alice', 9.5), "
                       "(2, 'bob', 7.25), (3, 'carol', 8.0)",
                       nullptr);
        CHECK_EQ(count(engine, "users"), 3);

        const auto rows = collect(
            engine, "SELECT id, name FROM users WHERE id >= 2 ORDER BY id");
        CHECK_EQ(rows.size(), 2u);
        CHECK_EQ(rows[0][0].as_int(), 2);
        CHECK_EQ(rows[0][1].text, "bob");
        CHECK_EQ(rows[1][0].as_int(), 3);

        require_check(engine);
        engine.close();
    }
    {
        Engine engine(options_for(dir));
        engine.open();
        CHECK_EQ(count(engine, "users"), 3);
        const auto schema = engine.catalog().find("users");
        CHECK(schema != nullptr);
        CHECK_EQ(schema->schema.row_size, 32u);
        require_check(engine);
    }
}

TEST(engine_transactions) {
    test::TempDir dir("engine-txn");
    Engine engine(options_for(dir));
    engine.open();
    engine.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)", nullptr);
    engine.execute("INSERT INTO t VALUES (1, 10)", nullptr);

    engine.execute("BEGIN", nullptr);
    engine.execute("INSERT INTO t VALUES (2, 20)", nullptr);
    CHECK_EQ(count(engine, "t"), 2);
    engine.execute("ROLLBACK", nullptr);
    CHECK_EQ(count(engine, "t"), 1);
    CHECK(!engine.in_transaction());

    engine.execute("BEGIN", nullptr);
    engine.execute("INSERT INTO t VALUES (2, 20)", nullptr);
    engine.execute("COMMIT", nullptr);
    CHECK_EQ(count(engine, "t"), 2);

    engine.execute("BEGIN", nullptr);
    engine.execute("INSERT INTO t VALUES (3, 30)", nullptr);
    CHECK_THROWS(engine.execute("INSERT INTO t VALUES (3, 99)", nullptr));
    CHECK(!engine.in_transaction());
    CHECK_EQ(count(engine, "t"), 2);

    engine.execute("BEGIN", nullptr);
    CHECK_THROWS(engine.execute("BEGIN", nullptr));
    CHECK(!engine.in_transaction());
    engine.execute("BEGIN", nullptr);
    CHECK_THROWS(engine.execute(
        "CREATE TABLE nested (id INT PRIMARY KEY)", nullptr));
    CHECK(!engine.in_transaction());
    CHECK(engine.catalog().find("nested") == nullptr);
    engine.execute("ROLLBACK", nullptr);
    require_check(engine);
}

TEST(engine_order_by_filter_limit) {
    test::TempDir dir("engine-order");
    Engine engine(options_for(dir));
    engine.open();
    engine.execute("CREATE TABLE numbers (id INT PRIMARY KEY, name TEXT(16), "
                   "score DOUBLE)",
                   nullptr);
    engine.execute("BEGIN", nullptr);
    for (int i = 0; i < 3000; ++i) {
        engine.execute("INSERT INTO numbers VALUES (" + std::to_string(i) +
                           ", 'name" + std::to_string(i % 100) + "', " +
                           std::to_string(static_cast<double>(i) * 1.5) + ")",
                       nullptr);
    }
    engine.execute("COMMIT", nullptr);

    const auto top = collect(
        engine,
        "SELECT id, score FROM numbers ORDER BY score DESC LIMIT 5");
    CHECK_EQ(top.size(), 5u);
    CHECK_EQ(top[0][0].as_int(), 2999);
    CHECK_EQ(top[4][0].as_int(), 2995);

    const auto text = collect(
        engine,
        "SELECT name FROM numbers ORDER BY name ASC LIMIT 1");
    CHECK_EQ(text.size(), 1u);
    CHECK_EQ(text[0][0].text, "name0");

    const auto range = collect(
        engine,
        "SELECT id FROM numbers WHERE id >= 100 AND id < 200 ORDER BY id DESC "
        "LIMIT 3");
    CHECK_EQ(range.size(), 3u);
    CHECK_EQ(range[0][0].as_int(), 199);
    CHECK_EQ(range[2][0].as_int(), 197);

    const auto filtered = collect(
        engine,
        "SELECT COUNT(*) FROM numbers WHERE score >= 4498.5");
    CHECK_EQ(filtered[0][0].as_int(), 1);

    const auto empty = collect(
        engine,
        "SELECT id FROM numbers WHERE id > 10 AND id < 5");
    CHECK(empty.empty());

    const auto limit_zero = collect(engine, "SELECT id FROM numbers LIMIT 0");
    CHECK(limit_zero.empty());

    const auto deleted = engine.execute("DELETE FROM numbers WHERE id >= 100 "
                                        "AND id < 200",
                                        nullptr);
    CHECK_EQ(deleted[0].rows, 100u);
    CHECK_EQ(count(engine, "numbers"), 2900);
    require_check(engine);
}

TEST(engine_rejects_bad_sql) {
    test::TempDir dir("engine-errors");
    Engine engine(options_for(dir));
    engine.open();
    engine.execute("CREATE TABLE t (id INT PRIMARY KEY, name TEXT(4))", nullptr);
    CHECK_THROWS(engine.execute("SELECT * FROM missing", nullptr));
    CHECK_THROWS(engine.execute("SELECT nope FROM t", nullptr));
    CHECK_THROWS(engine.execute("INSERT INTO t VALUES (1, 'toolong')", nullptr));
    CHECK_THROWS(engine.execute("INSERT INTO t VALUES ('x', 'a')", nullptr));
    CHECK_THROWS(engine.execute("CREATE TABLE t (id INT PRIMARY KEY)", nullptr));
    CHECK_THROWS(engine.execute("CREATE TABLE u (id INT)", nullptr));
    CHECK_THROWS(engine.execute("DROP TABLE nope", nullptr));
    CHECK_THROWS(engine.execute("INSERT INTO t VALUES (1)", nullptr));
    CHECK_EQ(engine.catalog().tables().size(), 1u);
    require_check(engine);
}

TEST(engine_delete_everything) {
    test::TempDir dir("engine-delete");
    Engine engine(options_for(dir));
    engine.open();
    engine.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)", nullptr);
    engine.execute("BEGIN", nullptr);
    for (int i = 0; i < 500; ++i) {
        engine.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                           std::to_string(i * 2) + ")",
                       nullptr);
    }
    engine.execute("COMMIT", nullptr);
    CHECK_EQ(count(engine, "t"), 500);
    engine.execute("DELETE FROM t", nullptr);
    CHECK_EQ(count(engine, "t"), 0);
    require_check(engine);
    engine.execute("INSERT INTO t VALUES (1, 1)", nullptr);
    CHECK_EQ(count(engine, "t"), 1);
    require_check(engine);
}
