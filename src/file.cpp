#include "db/file.h"

#include "db/common.h"

#include <algorithm>
#include <limits>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace db {
namespace {

#ifdef _WIN32
HANDLE handle_of(const void* p) {
    return const_cast<void*>(p);
}

void throw_win_error(const std::string& what) {
    fail(what + " (Windows error " + std::to_string(GetLastError()) + ")");
}
#endif

}  // namespace

File::~File() {
    try {
        close();
    } catch (...) {
    }
}

File::File(File&& other) noexcept {
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    size_ = other.size_;
    other.size_ = 0;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        try {
            close();
        } catch (...) {
        }
#ifdef _WIN32
        handle_ = other.handle_;
        other.handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        size_ = other.size_;
        other.size_ = 0;
    }
    return *this;
}

File File::open(const std::filesystem::path& path) {
    File file;
#ifdef _WIN32
    HANDLE h = CreateFileW(path.wstring().c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fail("cannot open " + path.string());
    }
    file.handle_ = h;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        throw_win_error("cannot size " + path.string());
    }
    file.size_ = static_cast<std::uint64_t>(size.QuadPart);
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        fail("cannot open " + path.string() + ": " + std::strerror(errno));
    }
    file.fd_ = fd;
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        fail("cannot size " + path.string() + ": " + std::strerror(errno));
    }
    file.size_ = static_cast<std::uint64_t>(st.st_size);
#endif
    return file;
}

void File::close() {
#ifdef _WIN32
    if (handle_ != nullptr) {
        CloseHandle(handle_of(handle_));
        handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    size_ = 0;
}

bool File::is_open() const noexcept {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

void File::read_at(std::uint64_t offset, void* buffer, std::size_t size) const {
    if (!is_open()) {
        fail("read from a closed file");
    }
    if (offset > size_ || size > size_ - offset) {
        fail("read past end of file");
    }
    auto* out = static_cast<std::uint8_t*>(buffer);
    std::size_t done = 0;
#ifdef _WIN32
    while (done < size) {
        OVERLAPPED ov{};
        const std::uint64_t pos = offset + done;
        ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
        const std::size_t chunk = std::min<std::size_t>(size - done, 1u << 20);
        DWORD got = 0;
        if (!ReadFile(handle_of(handle_), out + done, static_cast<DWORD>(chunk), &got, &ov)) {
            throw_win_error("read failed");
        }
        if (got == 0) {
            fail("short read");
        }
        done += got;
    }
#else
    while (done < size) {
        const ssize_t got = ::pread(fd_, out + done, size - done,
                                    static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("read failed: " + std::string(std::strerror(errno)));
        }
        if (got == 0) {
            fail("short read");
        }
        done += static_cast<std::size_t>(got);
    }
#endif
}

void File::write_at(std::uint64_t offset, const void* buffer, std::size_t size) {
    if (!is_open()) {
        fail("write to a closed file");
    }
    const auto* in = static_cast<const std::uint8_t*>(buffer);
    std::size_t done = 0;
#ifdef _WIN32
    while (done < size) {
        OVERLAPPED ov{};
        const std::uint64_t pos = offset + done;
        ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
        const std::size_t chunk = std::min<std::size_t>(size - done, 1u << 20);
        DWORD put = 0;
        if (!WriteFile(handle_of(handle_), in + done, static_cast<DWORD>(chunk), &put, &ov)) {
            throw_win_error("write failed");
        }
        if (put == 0) {
            fail("short write");
        }
        done += put;
    }
#else
    while (done < size) {
        const ssize_t put = ::pwrite(fd_, in + done, size - done,
                                     static_cast<off_t>(offset + done));
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("write failed: " + std::string(std::strerror(errno)));
        }
        if (put == 0) {
            fail("short write");
        }
        done += static_cast<std::size_t>(put);
    }
#endif
    size_ = std::max(size_, offset + size);
}

void File::append(const void* buffer, std::size_t size) {
    write_at(size_, buffer, size);
}

void File::truncate(std::uint64_t size) {
    if (!is_open()) {
        fail("truncate on a closed file");
    }
    if (size > size_) {
        fail("cannot grow a file with truncate");
    }
#ifdef _WIN32
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(handle_of(handle_), pos, nullptr, FILE_BEGIN)) {
        throw_win_error("seek failed");
    }
    if (!SetEndOfFile(handle_of(handle_))) {
        throw_win_error("truncate failed");
    }
#else
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        fail("truncate failed: " + std::string(std::strerror(errno)));
    }
#endif
    size_ = size;
}

void File::sync() {
    if (!is_open()) {
        fail("sync on a closed file");
    }
#ifdef _WIN32
    if (!FlushFileBuffers(handle_of(handle_))) {
        throw_win_error("flush failed");
    }
#else
    if (::fsync(fd_) != 0) {
        fail("fsync failed: " + std::string(std::strerror(errno)));
    }
#endif
}

}  // namespace db
