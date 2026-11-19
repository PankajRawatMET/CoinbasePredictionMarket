#include "cpm/oms/bookmark.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cpm::oms {
namespace {

// Deliberately boring: a line of key=value, so the file can be read by eye
// when something has gone wrong at four in the morning.
constexpr const char* kExecKey = "last_exec_id=";
// Written by every build before the run tag replaced it. Read and ignored for
// ever, not for one release: a rollback to an older binary writes it again,
// and a bookmark that stops a start is worse than a line nobody reads.
constexpr const char* kLegacyCounterKey = "cl_ord_counter=";

// A temporary name no other write can be using. One fixed ".tmp" was shared by
// every writer, and two saves at the same moment then truncated each other's
// half-written file, so the loser's rename found nothing to rename and the
// winner could carry a torn line into place. Only one caller is left now that
// ClOrdID allocation no longer saves, but save_bookmark() is still reachable
// from the session's callback thread and from a halt on another.
std::string temporary_name(const std::string& path) {
    static std::atomic<std::uint64_t> sequence{0};
    return path + ".tmp." + std::to_string(::getpid()) + "." +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1);
}

// The rename is only as durable as the directory entry that records it.
void sync_directory(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    const std::string directory = parent.empty() ? "." : parent.string();
    if (const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY); fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

}  // namespace

BookmarkFile::BookmarkFile(std::string path) : path_(std::move(path)) {}

std::optional<Bookmark> BookmarkFile::read() const {
    std::ifstream in(path_);
    if (!in.is_open()) return std::nullopt;

    Bookmark bookmark;
    bool saw_exec_id = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(kExecKey, 0) == 0) {
            bookmark.last_exec_id = line.substr(std::string(kExecKey).size());
            saw_exec_id = true;
        } else if (line.rfind(kLegacyCounterKey, 0) == 0) {
            continue;  // an older run's ClOrdID counter; nothing reads it now
        } else if (!line.empty()) {
            throw std::runtime_error("bookmark " + path_ + " has an unrecognised line: " + line);
        }
    }
    // A file with no last_exec_id at all is not a bookmark. An empty value is:
    // it is what a run that processed nothing leaves behind.
    if (!saw_exec_id) throw std::runtime_error("bookmark " + path_ + " has no last_exec_id");
    return bookmark;
}

void BookmarkFile::write(const Bookmark& bookmark) const {
    const std::string temporary = temporary_name(path_);
    {
        errno = 0;
        std::ofstream out(temporary, std::ios::trunc);
        if (!out.is_open()) {
            throw std::runtime_error("cannot write bookmark " + temporary + ": " +
                                     std::strerror(errno));
        }
        out << kExecKey << bookmark.last_exec_id << '\n';
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
        const int failure = errno;
        // Leave no litter beside the bookmark for the next operator to puzzle over.
        std::remove(temporary.c_str());
        throw std::runtime_error("cannot replace bookmark " + path_ + ": " +
                                 std::strerror(failure));
    }
    sync_directory(path_);
}

}  // namespace cpm::oms
