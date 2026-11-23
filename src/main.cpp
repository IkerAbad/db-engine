#include "db/engine.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace {

using db::Engine;
using db::EngineOptions;
using db::QueryResult;
using db::Value;

struct Options {
    std::filesystem::path database = "dbdata";
    std::filesystem::path script;
    std::string query;
    bool check = false;
    bool bench = false;
    bool values = false;
    bool help = false;
    bool version = false;
    std::size_t buffer_pages = 4096;
    std::size_t sort_buffer_bytes = 4 * 1024 * 1024;
    std::uint64_t bench_rows = 100000;
    std::uint64_t bench_queries = 1000;
};

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' ||
            text[begin] == '\r')) {
        ++begin;
    }
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' ||
            text[end - 1] == '\n' || text[end - 1] == '\r')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> split_statements(std::string_view text) {
    std::vector<std::string> statements;
    std::string current;
    bool in_string = false;
    bool in_comment = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_comment) {
            current.push_back(c);
            if (c == '\n') {
                in_comment = false;
            }
            continue;
        }
        if (in_string) {
            current.push_back(c);
            if (c == '\'') {
                if (i + 1 < text.size() && text[i + 1] == '\'') {
                    current.push_back(text[++i]);
                } else {
                    in_string = false;
                }
            }
            continue;
        }
        if (c == '\'') {
            in_string = true;
            current.push_back(c);
            continue;
        }
        if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            in_comment = true;
            current.push_back(c);
            current.push_back(text[++i]);
            continue;
        }
        if (c == ';') {
            const std::string statement = trim(current);
            if (!statement.empty()) {
                statements.push_back(statement);
            }
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    const std::string statement = trim(current);
    if (!statement.empty()) {
        statements.push_back(statement);
    }
    return statements;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        db::fail("cannot read " + path.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool stdin_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

std::size_t peak_rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        return static_cast<std::size_t>(counters.PeakWorkingSetSize);
    }
    return 0;
#else
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            std::istringstream values(line.substr(6));
            std::size_t kilobytes = 0;
            values >> kilobytes;
            return kilobytes * 1024;
        }
    }
    return 0;
#endif
}

std::string format_mb(std::uint64_t bytes) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
    return out.str();
}

std::string format_value(const Value& value) {
    return value.to_string();
}

void print_table(const std::vector<std::string>& columns,
                 const std::vector<std::vector<Value>>& rows, bool truncated,
                 std::uint64_t total_rows) {
    std::vector<std::size_t> widths(columns.size(), 0);
    for (std::size_t i = 0; i < columns.size(); ++i) {
        widths[i] = columns[i].size();
    }
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], format_value(row[i]).size());
        }
    }
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            std::cout << " | ";
        }
        std::cout << std::left << std::setw(static_cast<int>(widths[i]))
                  << columns[i];
    }
    std::cout << "\n";
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            std::cout << "-+-";
        }
        std::cout << std::string(widths[i], '-');
    }
    std::cout << "\n";
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            if (i > 0) {
                std::cout << " | ";
            }
            std::cout << std::left << std::setw(static_cast<int>(widths[i]))
                      << format_value(row[i]);
        }
        std::cout << "\n";
    }
    if (truncated) {
        std::cout << "(showing " << rows.size() << " of " << total_rows
                  << " rows)\n";
    } else {
        std::cout << "(" << total_rows << (total_rows == 1 ? " row)" : " rows)")
                  << "\n";
    }
}

void print_values(const std::vector<std::vector<Value>>& rows) {
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i > 0) {
                std::cout << '\t';
            }
            std::cout << format_value(row[i]);
        }
        std::cout << "\n";
    }
}

struct Collected {
    std::vector<std::vector<Value>> rows;
    std::uint64_t total = 0;
    static constexpr std::size_t kMaxRows = 10000;

    void add(const std::vector<Value>& row) {
        ++total;
        if (rows.size() < kMaxRows) {
            rows.push_back(row);
        }
    }
};

void run_sql(Engine& engine, std::string_view sql, const Options& options) {
    for (const std::string& statement : split_statements(sql)) {
        Collected collected;
        const auto results = engine.execute(statement, [&](const std::vector<Value>& row) {
            collected.add(row);
        });
        for (const QueryResult& result : results) {
            if (options.values) {
                continue;
            }
            if (!result.columns.empty()) {
                print_table(result.columns, collected.rows,
                            collected.total > Collected::kMaxRows, result.rows);
            } else if (!result.message.empty()) {
                std::cout << result.message << "\n";
            }
        }
        if (options.values && !collected.rows.empty()) {
            print_values(collected.rows);
        }
    }
}

void print_schema(const db::TableDef& table) {
    std::cout << "CREATE TABLE " << table.name << " (";
    for (std::size_t i = 0; i < table.schema.columns.size(); ++i) {
        const db::Column& column = table.schema.columns[i];
        if (i > 0) {
            std::cout << ", ";
        }
        std::cout << column.name << " " << db::column_type_name(column.type);
        if (column.type == db::ColumnType::Text) {
            std::cout << "(" << column.length << ")";
        }
        if (static_cast<int>(i) == table.schema.pk_index) {
            std::cout << " PRIMARY KEY";
        }
    }
    std::cout << ");\n";
}

bool handle_dot_command(Engine& engine, const std::string& command) {
    std::istringstream input(command);
    std::string name;
    input >> name;
    const std::string lower = db::ascii_lower(name);
    if (lower == ".exit" || lower == ".quit") {
        return false;
    }
    if (lower == ".help") {
        std::cout << ".tables          list tables\n"
                     ".schema [table]  show table definitions\n"
                     ".stats           buffer pool and file statistics\n"
                     ".check           verify page checksums and B+tree links\n"
                     ".exit            leave the shell\n";
        return true;
    }
    if (lower == ".tables") {
        if (engine.catalog().tables().empty()) {
            std::cout << "(no tables)\n";
        } else {
            for (const auto& table : engine.catalog().tables()) {
                std::cout << table.name << "\n";
            }
        }
        return true;
    }
    if (lower == ".schema") {
        std::string table;
        input >> table;
        bool any = false;
        for (const auto& definition : engine.catalog().tables()) {
            if (!table.empty() && db::ascii_lower(table) != definition.name) {
                continue;
            }
            print_schema(definition);
            any = true;
        }
        if (!any) {
            std::cout << "(no matching tables)\n";
        }
        return true;
    }
    if (lower == ".stats") {
        const auto& stats = engine.buffer_stats();
        std::cout << "tables        " << engine.catalog().tables().size() << "\n"
                  << "pages         " << engine.next_page() << "\n"
                  << "data          " << format_mb(engine.data_bytes()) << "\n"
                  << "wal           " << format_mb(engine.wal_bytes()) << "\n"
                  << "buffer hits   " << stats.hits << "\n"
                  << "buffer misses " << stats.misses << "\n"
                  << "hit rate      " << std::fixed << std::setprecision(2)
                  << stats.hit_rate() * 100.0 << "%\n";
        return true;
    }
    if (lower == ".check") {
        std::string error;
        if (engine.check(error)) {
            std::cout << "ok\n";
        } else {
            std::cout << "failed: " << error << "\n";
        }
        return true;
    }
    std::cout << "unknown command: " << name << " (try .help)\n";
    return true;
}

void repl(Engine& engine, const Options& options) {
    const bool interactive = stdin_is_tty();
    std::string buffer;
    std::string line;
    while (true) {
        if (interactive) {
            std::cout << (buffer.empty() ? "db> " : "   > ") << std::flush;
        }
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            if (!handle_dot_command(engine, trim(line))) {
                break;
            }
            continue;
        }
        buffer += line;
        buffer.push_back('\n');
        const std::string trimmed = trim(buffer);
        if (!trimmed.empty() && trimmed.back() == ';') {
            try {
                run_sql(engine, buffer, options);
            } catch (const std::exception& error) {
                std::cerr << "error: " << error.what() << "\n";
            }
            buffer.clear();
        }
    }
    if (!trim(buffer).empty()) {
        try {
            run_sql(engine, buffer, options);
        } catch (const std::exception& error) {
            std::cerr << "error: " << error.what() << "\n";
        }
    }
}

struct LatencyStats {
    double p50_ms = 0;
    double p99_ms = 0;
    double mean_ms = 0;
    double throughput = 0;
};

LatencyStats summarize(std::vector<double> samples, double total_seconds) {
    LatencyStats stats;
    if (samples.empty()) {
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    stats.p50_ms = samples[samples.size() / 2];
    stats.p99_ms = samples[std::min(samples.size() - 1,
                                    static_cast<std::size_t>(
                                        static_cast<double>(samples.size()) * 0.99))];
    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    stats.mean_ms = sum / static_cast<double>(samples.size());
    stats.throughput = total_seconds > 0 ? static_cast<double>(samples.size()) / total_seconds
                                         : 0.0;
    return stats;
}

void print_latency(const std::string& name, const LatencyStats& stats,
                   std::uint64_t ops) {
    std::cout << std::left << std::setw(24) << name << std::right
              << std::setw(10) << ops << std::setw(12) << std::fixed
              << std::setprecision(4) << stats.p50_ms << std::setw(12)
              << stats.p99_ms << std::setw(12) << stats.mean_ms
              << std::setw(14) << std::setprecision(0) << stats.throughput
              << "\n";
}

int run_benchmark(const Options& options) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("dbengine-bench-" + std::to_string(stamp));

    EngineOptions engine_options;
    engine_options.directory = directory;
    engine_options.buffer_pages = options.buffer_pages;
    engine_options.sort_buffer_bytes = options.sort_buffer_bytes;

    const std::uint64_t rows = options.bench_rows;
    const std::uint64_t queries = options.bench_queries;
    std::uint64_t range_rows = 0;
    std::uint64_t point_rows = 0;
    std::uint64_t count_rows = 0;

    {
        Engine engine(engine_options);
        engine.open();
        engine.execute("CREATE TABLE bench (id INT PRIMARY KEY, name TEXT(32), "
                       "score DOUBLE)",
                       nullptr);
        engine.execute("CREATE TABLE single (id INT PRIMARY KEY, value INT)",
                       nullptr);

        std::vector<std::int64_t> keys(rows);
        std::iota(keys.begin(), keys.end(), 0);
        std::mt19937 rng(42);
        std::shuffle(keys.begin(), keys.end(), rng);

        std::vector<double> insert_ms;
        insert_ms.reserve(rows);
        engine.execute("BEGIN", nullptr);
        const auto insert_start = std::chrono::steady_clock::now();
        std::string sql;
        for (std::uint64_t i = 0; i < rows; ++i) {
            const std::int64_t key = keys[i];
            sql.clear();
            sql += "INSERT INTO bench VALUES (";
            sql += std::to_string(key);
            sql += ", 'user";
            sql += std::to_string(key);
            sql += "', ";
            sql += std::to_string(static_cast<double>(key) * 0.5);
            sql += ")";
            const auto start = std::chrono::steady_clock::now();
            engine.execute(sql, nullptr);
            const auto end = std::chrono::steady_clock::now();
            insert_ms.push_back(
                std::chrono::duration<double, std::milli>(end - start).count());
        }
        const auto commit_start = std::chrono::steady_clock::now();
        engine.execute("COMMIT", nullptr);
        const auto commit_end = std::chrono::steady_clock::now();
        const double insert_seconds =
            std::chrono::duration<double>(commit_end - insert_start).count();
        const double commit_ms =
            std::chrono::duration<double, std::milli>(commit_end - commit_start)
                .count();

        std::vector<double> range_ms;
        range_ms.reserve(queries);
        std::uniform_int_distribution<std::int64_t> dist(
            0, static_cast<std::int64_t>(rows) - 101);
        for (std::uint64_t q = 0; q < queries; ++q) {
            const std::int64_t lo = dist(rng);
            const std::int64_t hi = lo + 100;
            std::string query =
                "SELECT id, name, score FROM bench WHERE id >= " +
                std::to_string(lo) + " AND id < " + std::to_string(hi) +
                " ORDER BY id";
            std::uint64_t seen = 0;
            const auto start = std::chrono::steady_clock::now();
            engine.execute(query, [&](const std::vector<Value>&) { ++seen; });
            const auto end = std::chrono::steady_clock::now();
            range_ms.push_back(
                std::chrono::duration<double, std::milli>(end - start).count());
            if (seen != 100) {
                db::fail("range query returned " + std::to_string(seen) +
                         " rows instead of 100");
            }
            range_rows += seen;
        }

        std::vector<double> point_ms;
        point_ms.reserve(queries);
        for (std::uint64_t q = 0; q < queries; ++q) {
            const std::int64_t key =
                static_cast<std::int64_t>(rng() % rows);
            std::string query = "SELECT name FROM bench WHERE id = " +
                                std::to_string(key);
            std::uint64_t seen = 0;
            const auto start = std::chrono::steady_clock::now();
            engine.execute(query, [&](const std::vector<Value>&) { ++seen; });
            const auto end = std::chrono::steady_clock::now();
            point_ms.push_back(
                std::chrono::duration<double, std::milli>(end - start).count());
            if (seen != 1) {
                db::fail("point query returned " + std::to_string(seen) +
                         " rows instead of 1");
            }
            point_rows += seen;
        }

        std::vector<double> count_ms(1);
        {
            const auto start = std::chrono::steady_clock::now();
            engine.execute("SELECT COUNT(*) FROM bench",
                           [&](const std::vector<Value>& row) {
                               count_rows = static_cast<std::uint64_t>(
                                   row.at(0).as_int());
                           });
            const auto end = std::chrono::steady_clock::now();
            count_ms[0] =
                std::chrono::duration<double, std::milli>(end - start).count();
        }
        if (count_rows != rows) {
            db::fail("COUNT(*) returned " + std::to_string(count_rows));
        }

        std::vector<double> autocommit_ms;
        autocommit_ms.reserve(200);
        for (int i = 0; i < 200; ++i) {
            const std::string insert =
                "INSERT INTO single VALUES (" + std::to_string(i) + ", " +
                std::to_string(i * 3) + ")";
            const auto start = std::chrono::steady_clock::now();
            engine.execute(insert, nullptr);
            const auto end = std::chrono::steady_clock::now();
            autocommit_ms.push_back(
                std::chrono::duration<double, std::milli>(end - start).count());
        }

        std::string check_error;
        if (!engine.check(check_error)) {
            db::fail("integrity check failed: " + check_error);
        }

        const auto stats = engine.buffer_stats();
        const std::uint64_t wal_bytes = engine.wal_bytes();
        const std::uint64_t data_bytes = engine.data_bytes();
        const std::uint32_t pages = engine.next_page();

        const LatencyStats insert_stats = summarize(insert_ms, insert_seconds);
        const LatencyStats range_stats =
            summarize(range_ms, std::accumulate(range_ms.begin(), range_ms.end(), 0.0) / 1000.0);
        const LatencyStats point_stats =
            summarize(point_ms, std::accumulate(point_ms.begin(), point_ms.end(), 0.0) / 1000.0);
        const LatencyStats count_stats = summarize(count_ms, count_ms[0] / 1000.0);
        const LatencyStats autocommit_stats =
            summarize(autocommit_ms,
                      std::accumulate(autocommit_ms.begin(), autocommit_ms.end(), 0.0) / 1000.0);
        const std::size_t peak = peak_rss_bytes();

        engine.close();

        std::cout << "db-engine benchmark\n"
                  << "rows: " << rows << ", range queries: " << queries
                  << ", buffer pages: " << options.buffer_pages << "\n\n";
        std::cout << std::left << std::setw(24) << "operation" << std::right
                  << std::setw(10) << "ops" << std::setw(12) << "p50 ms"
                  << std::setw(12) << "p99 ms" << std::setw(12) << "mean ms"
                  << std::setw(14) << "ops/s" << "\n";
        print_latency("insert (one txn)", insert_stats, rows);
        print_latency("insert (autocommit)", autocommit_stats,
                      autocommit_ms.size());
        print_latency("range scan (100 rows)", range_stats, queries);
        print_latency("point lookup", point_stats, queries);
        print_latency("count(*)", count_stats, 1);
        std::cout << "\ncommit (batch)          " << std::fixed
                  << std::setprecision(2) << commit_ms << " ms\n";
        std::cout << "rows returned           " << range_rows << " range, "
                  << point_rows << " point, " << count_rows << " count\n";
        std::cout << "peak RSS                " << format_mb(peak) << "\n";
        std::cout << "data file               " << format_mb(data_bytes)
                  << " (" << pages << " pages)\n";
        std::cout << "wal file                " << format_mb(wal_bytes) << "\n";
        std::cout << "buffer hits/misses      " << stats.hits << " / "
                  << stats.misses << " (" << std::fixed << std::setprecision(1)
                  << stats.hit_rate() * 100.0 << "%)\n";
        std::cout << "integrity               ok\n";
    }

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    return 0;
}

void print_usage() {
    std::cout
        << "usage: db_engine [options]\n\n"
           "  --db DIR            database directory (default: dbdata)\n"
           "  --script FILE       execute a SQL script and exit\n"
           "  --query SQL         execute one SQL statement and exit\n"
           "  --values            print tab-separated values only\n"
           "  --check             verify checksums and B+tree invariants\n"
           "  --bench             run the 100k insert benchmark in a temp db\n"
           "  --rows N            benchmark rows (default: 100000)\n"
           "  --queries N         benchmark queries (default: 1000)\n"
           "  --buffer-pages N    buffer pool size in 4 KB pages (default: 4096)\n"
           "  --sort-buffer BYTES sort buffer size before spilling (default: 4 MiB)\n"
           "  --help              show this help\n"
           "  --version           show the version\n";
}

std::size_t parse_size(const std::string& text, const std::string& option) {
    std::size_t value = 0;
    try {
        value = static_cast<std::size_t>(std::stoull(text));
    } catch (...) {
        db::fail("invalid value for " + option + ": " + text);
    }
    return value;
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                db::fail(std::string(name) + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--db" || arg == "-d") {
            options.database = value("--db");
        } else if (arg == "--script") {
            options.script = value("--script");
        } else if (arg == "--query") {
            options.query = value("--query");
        } else if (arg == "--values") {
            options.values = true;
        } else if (arg == "--check") {
            options.check = true;
        } else if (arg == "--bench") {
            options.bench = true;
        } else if (arg == "--buffer-pages") {
            options.buffer_pages = parse_size(value("--buffer-pages"), "--buffer-pages");
        } else if (arg == "--sort-buffer") {
            options.sort_buffer_bytes =
                parse_size(value("--sort-buffer"), "--sort-buffer");
        } else if (arg == "--rows") {
            options.bench_rows = parse_size(value("--rows"), "--rows");
        } else if (arg == "--queries") {
            options.bench_queries = parse_size(value("--queries"), "--queries");
        } else if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--version") {
            options.version = true;
        } else {
            db::fail("unknown option " + arg);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_args(argc, argv);
        if (options.help) {
            print_usage();
            return 0;
        }
        if (options.version) {
            std::cout << "db_engine " << DB_ENGINE_VERSION << "\n";
            return 0;
        }
        if (options.bench) {
            return run_benchmark(options);
        }

        EngineOptions engine_options;
        engine_options.directory = options.database;
        engine_options.buffer_pages = options.buffer_pages;
        engine_options.sort_buffer_bytes = options.sort_buffer_bytes;
        Engine engine(engine_options);
        engine.open();

        if (options.check) {
            std::string error;
            if (!engine.check(error)) {
                std::cerr << "error: " << error << "\n";
                engine.close();
                return 1;
            }
            std::cout << "ok\n";
            engine.close();
            return 0;
        }

        if (!options.query.empty()) {
            run_sql(engine, options.query, options);
            engine.close();
            return 0;
        }

        if (!options.script.empty()) {
            const std::string text = read_file(options.script);
            const auto statements = split_statements(text);
            for (const std::string& statement : statements) {
                run_sql(engine, statement, options);
            }
            engine.close();
            return 0;
        }

        repl(engine, options);
        engine.close();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
