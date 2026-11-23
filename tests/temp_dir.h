#pragma once

#include <filesystem>
#include <string>

namespace test {

class TempDir {
public:
    explicit TempDir(const std::string& name) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("dbengine-test-" + name + "-" + std::to_string(++counter));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace test
