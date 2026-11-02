#include "cpm/common/lock_file.hpp"

#include <gtest/gtest.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "temp_dir.hpp"

namespace {

using cpm::LockFile;
using cpm::test::TempDir;

TEST(LockFileTest, TakingAFreeLockSaysNothing) {
    TempDir dir("cpm-lock-free-");
    LockFile lock(dir("bookmark.txt.lock"));
    EXPECT_EQ(lock.acquire(), "");
    EXPECT_TRUE(lock.held());
    EXPECT_TRUE(std::filesystem::exists(dir("bookmark.txt.lock")));
}

TEST(LockFileTest, AcquiringTwiceOnTheSameLockIsHarmless) {
    TempDir dir("cpm-lock-twice-");
    LockFile lock(dir("l"));
    ASSERT_EQ(lock.acquire(), "");
    EXPECT_EQ(lock.acquire(), "");
    EXPECT_TRUE(lock.held());
}

// flock() binds the open file description, not the process, so a second
// open() of the same path is refused even from here - which is what makes
// the daemon's guard testable without a second daemon.
TEST(LockFileTest, ASecondLockOnTheSameFileIsRefusedAndNamesTheHolder) {
    TempDir dir("cpm-lock-second-");
    LockFile first(dir("bookmark.txt.lock"));
    ASSERT_EQ(first.acquire(), "");

    LockFile second(dir("bookmark.txt.lock"));
    const std::string refused = second.acquire();
    EXPECT_NE(refused.find("pid " + std::to_string(::getpid())), std::string::npos) << refused;
    EXPECT_NE(refused.find(dir("bookmark.txt.lock")), std::string::npos) << refused;
    EXPECT_FALSE(second.held());
}

TEST(LockFileTest, TheHoldersPidIsInTheFile) {
    TempDir dir("cpm-lock-pid-");
    LockFile lock(dir("l"));
    ASSERT_EQ(lock.acquire(), "");
    EXPECT_EQ(cpm::test::contents(dir("l")), std::to_string(::getpid()) + "\n");
}

TEST(LockFileTest, ReleasingLetsTheNextOneInAndLeavesTheFileBehind) {
    TempDir dir("cpm-lock-release-");
    {
        LockFile first(dir("l"));
        ASSERT_EQ(first.acquire(), "");
    }
    EXPECT_TRUE(std::filesystem::exists(dir("l"))) << "the file is never unlinked";

    LockFile second(dir("l"));
    EXPECT_EQ(second.acquire(), "") << "a file left behind holds nothing: the lock was the fd";
}

TEST(LockFileTest, ReleaseIsIdempotentAndTheLockCanBeRetaken) {
    TempDir dir("cpm-lock-idempotent-");
    LockFile lock(dir("l"));
    ASSERT_EQ(lock.acquire(), "");
    lock.release();
    lock.release();
    EXPECT_FALSE(lock.held());
    EXPECT_EQ(lock.acquire(), "");
}

TEST(LockFileTest, ADirectoryThatIsNotThereYetIsMade) {
    TempDir dir("cpm-lock-mkdir-");
    LockFile lock(dir("state/run/bookmark.txt.lock"));
    EXPECT_EQ(lock.acquire(), "");
    EXPECT_TRUE(std::filesystem::exists(dir("state/run/bookmark.txt.lock")));
}

TEST(LockFileTest, APathThatCannotBeOpenedIsReportedNotThrown) {
    TempDir dir("cpm-lock-unopenable-");
    std::filesystem::create_directories(dir("itself-a-directory"));
    LockFile lock(dir("itself-a-directory"));
    const std::string refused = lock.acquire();
    EXPECT_NE(refused.find("cannot open the lock file"), std::string::npos) << refused;
    EXPECT_FALSE(lock.held());
}

// The reason this is flock() and not a pid file: a daemon that dies without
// running a line of its own shutdown leaves nothing to clean up by hand.
TEST(LockFileTest, AKilledHolderFreesTheLockWithNothingToCleanUp) {
    TempDir dir("cpm-lock-killed-");
    const std::string path = dir("bookmark.txt.lock");

    int ready[2] = {-1, -1};
    ASSERT_EQ(::pipe(ready), 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(ready[0]);
        LockFile held(path);
        const char answer = held.acquire().empty() ? 'y' : 'n';
        [[maybe_unused]] const ssize_t sent = ::write(ready[1], &answer, 1);
        // Hold it until killed. _exit, never exit: no test framework
        // teardown is to run in this process.
        for (;;) ::pause();
        ::_exit(0);
    }

    ::close(ready[1]);
    char answer = 'n';
    ASSERT_EQ(::read(ready[0], &answer, 1), 1);
    ASSERT_EQ(answer, 'y') << "the child could not take the lock";
    ::close(ready[0]);

    LockFile ours(path);
    const std::string refused = ours.acquire();
    EXPECT_NE(refused.find("pid " + std::to_string(child)), std::string::npos) << refused;

    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);

    EXPECT_EQ(ours.acquire(), "") << "a SIGKILLed holder leaves no stale lock";
}

}  // namespace
