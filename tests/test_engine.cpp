#include "db/engine.h"
#include "temp_dir.h"
#include "test.h"

#include <map>
#include <set>
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

std::vector<std::string> column_names(Engine& engine, const std::string& sql) {
    const auto results =
        engine.execute(sql, [](const std::vector<Value>&) {});
    if (results.empty()) {
        return {};
    }
    return results[0].columns;
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

        const auto qualified = collect(
            engine, "SELECT users.id FROM users WHERE users.id = 1");
        CHECK_EQ(qualified.size(), 1u);
        CHECK_EQ(qualified[0][0].as_int(), 1);
        CHECK_THROWS(engine.execute("SELECT other.id FROM users", nullptr));

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

TEST(engine_join_empty) {
    test::TempDir dir("engine-join-empty");
    Engine engine(options_for(dir));
    engine.open();
    engine.execute("CREATE TABLE l (id INT PRIMARY KEY, v INT)", nullptr);
    engine.execute("CREATE TABLE r (id INT PRIMARY KEY, v INT)", nullptr);
    engine.execute("INSERT INTO r VALUES (1, 10), (2, 20)", nullptr);

    auto rows = collect(engine, "SELECT l.id, r.v FROM l JOIN r ON l.id = r.id");
    CHECK(rows.empty());
    auto counted =
        collect(engine, "SELECT COUNT(*) FROM l JOIN r ON l.id = r.id");
    CHECK_EQ(counted.size(), 1u);
    CHECK_EQ(counted[0][0].as_int(), 0);

    engine.execute("INSERT INTO l VALUES (1, 100)", nullptr);
    engine.execute("DELETE FROM r", nullptr);
    rows = collect(engine, "SELECT l.id, r.v FROM l JOIN r ON l.id = r.id");
    CHECK(rows.empty());
    require_check(engine);
}

TEST(engine_join_duplicates) {
    test::TempDir dir("engine-join-duplicates");
    Engine engine(options_for(dir));
    engine.open();
    engine.execute("CREATE TABLE users (id INT PRIMARY KEY, grp INT)", nullptr);
    engine.execute("CREATE TABLE events (id INT PRIMARY KEY, grp INT, "
                   "label TEXT(8))",
                   nullptr);
    engine.execute("INSERT INTO users VALUES (1, 1), (2, 1), (3, 2)", nullptr);
    engine.execute("INSERT INTO events VALUES (10, 1, 'a'), (11, 1, 'b'), "
                   "(12, 2, 'c')",
                   nullptr);

    const auto rows = collect(engine,
                              "SELECT users.id, events.id FROM users JOIN events "
                              "ON users.grp = events.grp");
    CHECK_EQ(rows.size(), 5u);
    std::map<std::int64_t, int> per_user;
    std::set<std::pair<std::int64_t, std::int64_t>> pairs;
    for (const auto& row : rows) {
        const std::int64_t user = row[0].as_int();
        const std::int64_t event = row[1].as_int();
        ++per_user[user];
        pairs.insert({user, event});
    }
    CHECK_EQ(per_user[1], 2);
    CHECK_EQ(per_user[2], 2);
    CHECK_EQ(per_user[3], 1);
    CHECK_EQ(pairs.size(), 5u);
    CHECK(pairs.contains({1, 10}));
    CHECK(pairs.contains({1, 11}));
    CHECK(pairs.contains({2, 10}));
    CHECK(pairs.contains({2, 11}));
    CHECK(pairs.contains({3, 12}));

    const auto counted =
        collect(engine, "SELECT COUNT(*) FROM users JOIN events "
                        "ON users.grp = events.grp");
    CHECK_EQ(counted[0][0].as_int(), 5);
    const auto filtered =
        collect(engine, "SELECT COUNT(*) FROM users JOIN events "
                        "ON users.grp = events.grp WHERE events.label != 'b'");
    CHECK_EQ(filtered[0][0].as_int(), 3);
    require_check(engine);
}

TEST(engine_join_homonymous) {
    test::TempDir dir("engine-join-homonymous");
    Engine engine(options_for(dir));
    engine.open();
    engine.execute("CREATE TABLE left_t (id INT PRIMARY KEY, v INT)", nullptr);
    engine.execute("CREATE TABLE right_t (id INT PRIMARY KEY, v INT)", nullptr);
    engine.execute("INSERT INTO left_t VALUES (1, 100), (2, 200)", nullptr);
    engine.execute("INSERT INTO right_t VALUES (2, 20), (3, 30)", nullptr);

    const auto rows =
        collect(engine, "SELECT left_t.id, left_t.v, right_t.id, right_t.v "
                        "FROM left_t JOIN right_t ON left_t.id = right_t.id");
    CHECK_EQ(rows.size(), 1u);
    CHECK_EQ(rows[0][0].as_int(), 2);
    CHECK_EQ(rows[0][1].as_int(), 200);
    CHECK_EQ(rows[0][2].as_int(), 2);
    CHECK_EQ(rows[0][3].as_int(), 20);

    const auto names = column_names(
        engine, "SELECT * FROM left_t JOIN right_t ON left_t.id = right_t.id");
    CHECK_EQ(names.size(), 4u);
    CHECK_EQ(names[0], "left_t.id");
    CHECK_EQ(names[1], "left_t.v");
    CHECK_EQ(names[2], "right_t.id");
    CHECK_EQ(names[3], "right_t.v");

    const auto star = collect(
        engine, "SELECT * FROM left_t JOIN right_t ON left_t.id = right_t.id");
    CHECK_EQ(star.size(), 1u);
    CHECK_EQ(star[0].size(), 4u);
    CHECK_EQ(star[0][0].as_int(), 2);
    CHECK_EQ(star[0][1].as_int(), 200);
    CHECK_EQ(star[0][2].as_int(), 2);
    CHECK_EQ(star[0][3].as_int(), 20);

    const auto reversed = collect(
        engine, "SELECT left_t.id, right_t.id FROM left_t JOIN right_t "
                "ON right_t.id = left_t.id");
    CHECK_EQ(reversed.size(), 1u);
    CHECK_EQ(reversed[0][0].as_int(), 2);
    CHECK_EQ(reversed[0][1].as_int(), 2);

    CHECK_THROWS(engine.execute(
        "SELECT id FROM left_t JOIN right_t ON left_t.id = right_t.id",
        nullptr));
    require_check(engine);
}

TEST(engine_join_memory_limit) {
    test::TempDir dir("engine-join-memory");
    EngineOptions options = options_for(dir);
    options.join_buffer_bytes = 256;
    Engine engine(options);
    engine.open();
    engine.execute("CREATE TABLE l (id INT PRIMARY KEY, v INT)", nullptr);
    engine.execute("CREATE TABLE r (id INT PRIMARY KEY, v INT)", nullptr);
    engine.execute("INSERT INTO l VALUES (1, 1)", nullptr);
    engine.execute("BEGIN", nullptr);
    for (int i = 0; i < 100; ++i) {
        engine.execute("INSERT INTO r VALUES (" + std::to_string(i) + ", 0)",
                       nullptr);
    }
    engine.execute("COMMIT", nullptr);

    bool threw = false;
    std::string message;
    try {
        (void)collect(engine, "SELECT l.id FROM l JOIN r ON l.id = r.id");
    } catch (const db::Error& error) {
        threw = true;
        message = error.what();
    }
    CHECK(threw);
    CHECK(message.find("hash join build side") != std::string::npos);
    CHECK(message.find("memory limit") != std::string::npos);
    require_check(engine);
}

TEST(engine_join_filter_order_limit) {
    test::TempDir dir("engine-join-filter");
    Engine engine(options_for(dir));
    engine.open();
    engine.execute("CREATE TABLE users (id INT PRIMARY KEY, grp INT)", nullptr);
    engine.execute("CREATE TABLE events (id INT PRIMARY KEY, grp INT, "
                   "label TEXT(8))",
                   nullptr);
    engine.execute("INSERT INTO users VALUES (1, 1), (2, 1), (3, 2)", nullptr);
    engine.execute("INSERT INTO events VALUES (10, 1, 'a'), (11, 1, 'b'), "
                   "(12, 2, 'c')",
                   nullptr);

    const auto rows =
        collect(engine, "SELECT users.id, events.label FROM users JOIN events "
                        "ON users.grp = events.grp WHERE events.label != 'b' "
                        "ORDER BY users.id DESC LIMIT 2");
    CHECK_EQ(rows.size(), 2u);
    CHECK_EQ(rows[0][0].as_int(), 3);
    CHECK_EQ(rows[0][1].text, "c");
    CHECK_EQ(rows[1][0].as_int(), 2);
    CHECK_EQ(rows[1][1].text, "a");
    require_check(engine);
}

TEST(engine_join_text_key) {
    test::TempDir dir("engine-join-text");
    Engine engine(options_for(dir));
    engine.open();
    engine.execute("CREATE TABLE a (id INT PRIMARY KEY, city TEXT(8))", nullptr);
    engine.execute("CREATE TABLE b (id INT PRIMARY KEY, city TEXT(8))", nullptr);
    engine.execute("INSERT INTO a VALUES (1, 'x'), (2, 'y')", nullptr);
    engine.execute("INSERT INTO b VALUES (10, 'y'), (11, 'y'), (12, 'z')",
                   nullptr);

    const auto rows = collect(engine,
                              "SELECT a.id, b.id FROM a JOIN b "
                              "ON a.city = b.city ORDER BY b.id");
    CHECK_EQ(rows.size(), 2u);
    CHECK_EQ(rows[0][0].as_int(), 2);
    CHECK_EQ(rows[0][1].as_int(), 10);
    CHECK_EQ(rows[1][0].as_int(), 2);
    CHECK_EQ(rows[1][1].as_int(), 11);
    require_check(engine);
}
