#pragma once

#include <string>

namespace cpm {

// An advisory exclusive lock on one file, held for the life of the process
// that took it: what stops a second daemon from running against the first
// one's durable state.
//
// Two cpm_trader processes sharing one bookmark is the failure this exists
// for. They would allocate ClOrdIDs from the same counter and save over each
// other's last_exec_id, so neither could replay correctly after a restart
// (docs/open-questions-oms.txt OE-Q7). The second damage is at the venue:
// the gateway allows one FIX session per API key and displaces the older one
// WITHOUT firing cancel-on-disconnect, so starting a second daemon silently
// orphans the first one's resting quotes with nobody managing them (spec
// OE:197-201, GUI-Q1). Both are the same mistake - an operator starting the
// daemon twice - and one lock answers it before either can happen.
//
// The lock is the open file description, not the file:
//
//   - A kill -9 frees it, because the kernel closes the fd. There is no
//     stale lock to clean up by hand, which is the whole reason this is
//     flock() and not a pid file that outlives its process.
//   - The file is never unlinked, not even on a clean exit. Removing it
//     would be the one way to break the guarantee: a contender that has
//     already opened the path would be locking an inode nothing else can
//     reach, and two daemons would each believe they held it.
//   - A lock file lying around therefore means nothing on its own. The pid
//     written inside it is for the error message and for an operator with
//     `ps`, never for deciding whether the lock is held.
//
// Advisory and local: it binds the processes that take it, not every writer
// of the file, and flock() over NFS is not to be relied on. Both are fine
// for what this guards - the daemon locks its own sidecar on a local disk,
// and the only process that ever takes it is another cpm_trader.
class LockFile {
public:
    // `path` is a file of its own, never one that is written by rename.
    // BookmarkFile replaces its inode on every save, so a lock taken on the
    // bookmark itself would stop protecting anything after the first write.
    explicit LockFile(std::string path);
    ~LockFile();

    LockFile(const LockFile&) = delete;
    LockFile& operator=(const LockFile&) = delete;

    // Takes the lock, or says why it could not: either another process holds
    // it - named by the pid it wrote, when that can still be read - or the
    // file could not be opened at all. Never blocks and never waits: a
    // second daemon is a mistake to report, not a queue to join. Empty on
    // success, and the message is ready to be logged as it stands.
    std::string acquire();

    bool held() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }

    // Drops the lock early. The destructor does it; this is for a caller
    // that wants the lock gone before the rest of its shutdown runs.
    void release();

private:
    std::string path_;
    int fd_ = -1;
};

}  // namespace cpm
