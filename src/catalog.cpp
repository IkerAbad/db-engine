#include "db/catalog.h"

#include "db/file.h"

#include <algorithm>
#include <array>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace db {
namespace {

constexpr std::uint32_t kCatalogMagic = 0x43415431u;  // "CAT1"
constexpr std::uint32_t kCatalogVersion = 1;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    std::uint8_t bytes[4];
    store_u32(bytes, v);
    out.insert(out.end(), bytes, bytes + 4);
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    std::uint8_t bytes[8];
    store_u64(bytes, v);
    out.insert(out.end(), bytes, bytes + 8);
}

std::uint32_t take_u32(std::span<const std::uint8_t> bytes, std::size_t& pos) {
    if (pos + 4 > bytes.size()) {
        fail("truncated catalog");
    }
    const std::uint32_t v = load_u32(bytes.data() + pos);
    pos += 4;
    return v;
}

std::uint64_t take_u64(std::span<const std::uint8_t> bytes, std::size_t& pos) {
    if (pos + 8 > bytes.size()) {
        fail("truncated catalog");
    }
    const std::uint64_t v = load_u64(bytes.data() + pos);
    pos += 8;
    return v;
}

void sync_parent_directory(const std::filesystem::path& path) {
#ifndef _WIN32
    const std::filesystem::path parent = path.parent_path().empty()
                                             ? std::filesystem::path(".")
                                             : path.parent_path();
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int fd = ::open(parent.c_str(), flags);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)path;
#endif
}

}  // namespace

std::vector<std::uint8_t> serialize_table_def(const TableDef& table) {
    std::vector<std::uint8_t> out;
    put_u32(out, table.id);
    put_u32(out, table.meta_page);
    put_u32(out, static_cast<std::uint32_t>(table.name.size()));
    out.insert(out.end(), table.name.begin(), table.name.end());
    const std::vector<std::uint8_t> schema = table.schema.serialize();
    put_u32(out, static_cast<std::uint32_t>(schema.size()));
    out.insert(out.end(), schema.begin(), schema.end());
    return out;
}

TableDef deserialize_table_def(std::span<const std::uint8_t> bytes) {
    std::size_t pos = 0;
    TableDef table;
    table.id = take_u32(bytes, pos);
    table.meta_page = take_u32(bytes, pos);
    const std::uint32_t name_length = take_u32(bytes, pos);
    if (pos + name_length > bytes.size()) {
        fail("truncated table definition");
    }
    table.name.assign(reinterpret_cast<const char*>(bytes.data() + pos), name_length);
    pos += name_length;
    const std::uint32_t schema_length = take_u32(bytes, pos);
    if (pos + schema_length > bytes.size()) {
        fail("truncated table schema");
    }
    std::size_t consumed = 0;
    table.schema = Schema::deserialize(bytes.subspan(pos, schema_length), consumed);
    return table;
}

Catalog::Catalog(std::filesystem::path path) : path_(std::move(path)) {}

bool Catalog::exists() const {
    std::error_code ec;
    return std::filesystem::exists(path_, ec) &&
           std::filesystem::file_size(path_, ec) > 0;
}

void Catalog::load() {
    if (!exists()) {
        return;
    }
    File file = File::open(path_);
    std::vector<std::uint8_t> bytes(file.size());
    file.read_at(0, bytes.data(), bytes.size());
    if (bytes.size() < 8) {
        fail("catalog is truncated");
    }
    const std::uint32_t magic = load_u32(bytes.data());
    const std::uint32_t version = load_u32(bytes.data() + 4);
    if (magic != kCatalogMagic || version != kCatalogVersion) {
        fail("catalog has an unknown format");
    }
    if (bytes.size() < 12) {
        fail("catalog is truncated");
    }
    const std::uint32_t stored_crc = load_u32(bytes.data() + bytes.size() - 4);
    const std::uint32_t crc = crc32(bytes.data(), bytes.size() - 4);
    if (stored_crc != crc) {
        fail("catalog checksum mismatch");
    }

    std::size_t pos = 8;
    checkpoint_offset_ = take_u64(bytes, pos);
    next_txn_id_ = take_u64(bytes, pos);
    next_table_id_ = take_u32(bytes, pos);
    const std::uint32_t count = take_u32(bytes, pos);
    if (count > (bytes.size() - pos) / 8 + 1) {
        fail("catalog table count is implausible");
    }
    tables_.clear();
    tables_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        TableDef table;
        table.id = take_u32(bytes, pos);
        table.meta_page = take_u32(bytes, pos);
        const std::uint32_t name_length = take_u32(bytes, pos);
        if (pos + name_length > bytes.size() - 4) {
            fail("truncated catalog table name");
        }
        table.name.assign(reinterpret_cast<const char*>(bytes.data() + pos),
                          name_length);
        pos += name_length;
        const std::uint32_t schema_length = take_u32(bytes, pos);
        if (pos + schema_length > bytes.size() - 4) {
            fail("truncated catalog schema");
        }
        std::size_t consumed = 0;
        table.schema = Schema::deserialize(
            std::span<const std::uint8_t>(bytes.data() + pos, schema_length),
            consumed);
        pos += schema_length;
        tables_.push_back(std::move(table));
    }
}

void Catalog::save() {
    std::vector<std::uint8_t> out;
    put_u32(out, kCatalogMagic);
    put_u32(out, kCatalogVersion);
    put_u64(out, checkpoint_offset_);
    put_u64(out, next_txn_id_);
    put_u32(out, next_table_id_);
    put_u32(out, static_cast<std::uint32_t>(tables_.size()));
    for (const TableDef& table : tables_) {
        put_u32(out, table.id);
        put_u32(out, table.meta_page);
        put_u32(out, static_cast<std::uint32_t>(table.name.size()));
        out.insert(out.end(), table.name.begin(), table.name.end());
        const std::vector<std::uint8_t> schema = table.schema.serialize();
        put_u32(out, static_cast<std::uint32_t>(schema.size()));
        out.insert(out.end(), schema.begin(), schema.end());
    }
    put_u32(out, crc32(out.data(), out.size()));

    const std::filesystem::path tmp = path_.string() + ".tmp";
    {
        File file = File::open(tmp);
        file.truncate(0);
        file.write_at(0, out.data(), out.size());
        file.sync();
    }
#ifdef _WIN32
    if (!MoveFileExW(tmp.wstring().c_str(), path_.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fail("cannot replace catalog file");
    }
#else
    if (::rename(tmp.c_str(), path_.c_str()) != 0) {
        fail("cannot replace catalog file");
    }
#endif
    sync_parent_directory(path_);
}

TableDef* Catalog::find(std::string_view name) {
    const std::string lower = ascii_lower(name);
    for (TableDef& table : tables_) {
        if (table.name == lower) {
            return &table;
        }
    }
    return nullptr;
}

const TableDef* Catalog::find(std::string_view name) const {
    const std::string lower = ascii_lower(name);
    for (const TableDef& table : tables_) {
        if (table.name == lower) {
            return &table;
        }
    }
    return nullptr;
}

TableDef* Catalog::find_by_id(std::uint32_t id) {
    for (TableDef& table : tables_) {
        if (table.id == id) {
            return &table;
        }
    }
    return nullptr;
}

void Catalog::add(TableDef table) {
    table.name = ascii_lower(table.name);
    if (find(table.name) != nullptr) {
        fail("table " + table.name + " already exists");
    }
    tables_.push_back(std::move(table));
}

void Catalog::remove(std::string_view name) {
    const std::string lower = ascii_lower(name);
    tables_.erase(std::remove_if(tables_.begin(), tables_.end(),
                                 [&](const TableDef& table) {
                                     return table.name == lower;
                                 }),
                  tables_.end());
}

void Catalog::remove_by_id(std::uint32_t id) {
    tables_.erase(std::remove_if(tables_.begin(), tables_.end(),
                                 [&](const TableDef& table) {
                                     return table.id == id;
                                 }),
                  tables_.end());
}

}  // namespace db
