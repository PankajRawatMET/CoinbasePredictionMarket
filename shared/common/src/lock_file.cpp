#include "cpm/common/lock_file.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>

namespace cpm {
namespace {

// The pid the holder wrote, for the message only. Anything unreadable or not
// a number gives 0, which reads as "held, by something that did not say what".
long holder_pid(int fd) {
    char buffer[32] = {};
    const ssize_t read = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
    if (read <= 0) return 0;
    char* end = nullptr;
    const long pid = std::strtol(buffer, &end, 10);
    return end == buffer || pid <= 0 ? 0 : pid;
}

}  // namespace

LockFile::LockFile(std::string path) : path_(std::move(path)) {}

LockFile::~LockFile() { release(); }

std::string LockFile::acquire() {
    if (held()) return {};

    // A bookmark may be named into a directory that does not exist yet; the
    // lock must not be the reason that run fails differently from one
    // without it. A failure here is left to open() to report properly.
    const auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) {
        std::error_code ignored;
        std::filesystem::create_directories(parent, ignored);
    }

    // O_CLOEXEC: nothing the daemon ever runs should inherit this fd and
    // hold the lock open past our exit.
    errno = 0;
    const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return "cannot open the lock file " + path_ + ": " + std::strerror(errno);
    }

    errno = 0;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int failure = errno;
        std::string held_by;
        if (failure == EWOULDBLOCK) {
            const long pid = holder_pid(fd);
            held_by = pid > 0 ? "pid " + std::to_string(pid) + " already holds " + path_
                              : "another process already holds " + path_;
        } else {
            held_by = "cannot lock " + path_ + ": " + std::strerror(failure);
        }
        ::close(fd);
        return held_by;
    }

    fd_ = fd;

    // Our pid, for the next contender's message and for an operator with
    // `ps`. Truncate first: the previous holder's pid is longer or shorter
    // than ours, and a half-overwritten number would name a process that
    // has nothing to do with this. A failure to write it costs the message,
    // never the lock, so it is not reported.
    const std::string line = std::to_string(::getpid()) + "\n";
    if (::ftruncate(fd_, 0) == 0) {
        [[maybe_unused]] const ssize_t written = ::pwrite(fd_, line.data(), line.size(), 0);
    }
    return {};
}

void LockFile::release() {
    if (!held()) return;
    // close() drops the lock. The file stays: see the header on why removing
    // it is the one way to break this.
    ::close(fd_);
    fd_ = -1;
}

}  // namespace cpm
