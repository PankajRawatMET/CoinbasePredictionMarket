#pragma once

// A directory of its own per test, removed with it. The files these tests
// write are the point of them, so they are written for real rather than
// faked behind an interface.
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace cpm::test {

class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / (name + std::to_string(::getpid()))) {
        std::filesystem::remove_all(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string operator()(const std::string& leaf = {}) const {
        return leaf.empty() ? path_.string() : (path_ / leaf).string();
    }

private:
    std::filesystem::path path_;
};

inline std::string contents(const std::string& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace cpm::test
