#include "cpm/oms/bookmark.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cpm::oms {
namespace {

// Deliberately boring: two lines of key=value, so the file can be read by eye
// when something has gone wrong at four in the morning.
constexpr const char* kExecKey = "last_exec_id=";
constexpr const char* kCounterKey = "cl_ord_counter=";

}  // namespace

BookmarkFile::BookmarkFile(std::string path) : path_(std::move(path)) {}

std::optional<Bookmark> BookmarkFile::read() const {
    std::ifstream in(path_);
    if (!in.is_open()) return std::nullopt;

    Bookmark bookmark;
    bool saw_counter = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(kExecKey, 0) == 0) {
            bookmark.last_exec_id = line.substr(std::string(kExecKey).size());
        } else if (line.rfind(kCounterKey, 0) == 0) {
            try {
                bookmark.cl_ord_counter = std::stoull(line.substr(std::string(kCounterKey).size()));
            } catch (const std::exception&) {
                throw std::runtime_error("bookmark " + path_ + " has a malformed cl_ord_counter");
            }
            saw_counter = true;
        } else if (!line.empty()) {
            throw std::runtime_error("bookmark " + path_ + " has an unrecognised line: " + line);
        }
    }
    if (!saw_counter) throw std::runtime_error("bookmark " + path_ + " has no cl_ord_counter");
    return bookmark;
}

void BookmarkFile::write(const Bookmark& bookmark) const {
    const std::string temporary = path_ + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out.is_open()) throw std::runtime_error("cannot write bookmark " + temporary);
        out << kExecKey << bookmark.last_exec_id << '\n'
            << kCounterKey << bookmark.cl_ord_counter << '\n';
        out.flush();
        if (!out) throw std::runtime_error("cannot write bookmark " + temporary);
    }
    // Flush the file, then rename: the rename is atomic, so a reader sees one
    // version or the other.
    if (const int fd = ::open(temporary.c_str(), O_RDONLY); fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
    if (std::rename(temporary.c_str(), path_.c_str()) != 0) {
        throw std::runtime_error("cannot replace bookmark " + path_);
    }
}

}  // namespace cpm::oms
