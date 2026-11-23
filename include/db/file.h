#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace db {

// Thin positional I/O layer. read_at/write_at never pass through the C++
// stream buffer, so the durability rules in the engine are explicit.
class File {
public:
    File() = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    static File open(const std::filesystem::path& path);

    void close();
    bool is_open() const noexcept;
    std::uint64_t size() const noexcept { return size_; }

    void read_at(std::uint64_t offset, void* buffer, std::size_t size) const;
    void write_at(std::uint64_t offset, const void* buffer, std::size_t size);
    void append(const void* buffer, std::size_t size);
    void truncate(std::uint64_t size);
    void sync();

private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
    std::uint64_t size_ = 0;
};

}  // namespace db
