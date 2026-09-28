#include <gtest/gtest.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fast_rdma_endpoint.h"
#include "fast_utils.h"

namespace fast {

// For unit tests only: the production helper operates on an ordinary Linux fd.
class FastRdmaEndpointFdTestPeer {
public:
    static int SetNonBlocking(int fd) {
        return FastRdmaEndpoint::SetNonBlocking(fd);
    }
};

namespace {
using Peer = FastRdmaEndpointFdTestPeer;

TEST(RdmaChannelFd, PreservesExistingFlagsAndIsIdempotent) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    auto cleanup = MakeScopeGuard([&] { close(fds[0]); close(fds[1]); });
    const int original_flags = fcntl(fds[0], F_GETFL);
    ASSERT_GE(original_flags, 0);
    ASSERT_EQ(0, fcntl(fds[0], F_SETFL, original_flags | O_APPEND));
    ASSERT_EQ(0, fcntl(fds[0], F_SETFD, FD_CLOEXEC));
    const int expected_flags = fcntl(fds[0], F_GETFL) | O_NONBLOCK;

    ASSERT_EQ(0, Peer::SetNonBlocking(fds[0]));
    EXPECT_EQ(expected_flags, fcntl(fds[0], F_GETFL));
    EXPECT_EQ(FD_CLOEXEC, fcntl(fds[0], F_GETFD));
    ASSERT_EQ(0, Peer::SetNonBlocking(fds[0]));
    EXPECT_EQ(expected_flags, fcntl(fds[0], F_GETFL));
}

TEST(RdmaChannelFd, DrainingAvailableDataEndsWithEagain) {
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    auto cleanup = MakeScopeGuard([&] { close(fds[0]); close(fds[1]); });
    ASSERT_EQ(0, Peer::SetNonBlocking(fds[0]));
    // Check before read so a regression reports a failure instead of hanging.
    const int flags = fcntl(fds[0], F_GETFL);
    ASSERT_GE(flags, 0);
    ASSERT_NE(0, flags & O_NONBLOCK);

    char data[4];
    EXPECT_EQ(-1, read(fds[0], data, sizeof(data)));
    EXPECT_EQ(EAGAIN, errno);
    ASSERT_EQ(3, write(fds[1], "abc", 3));
    ASSERT_EQ(3, read(fds[0], data, sizeof(data)));
    EXPECT_EQ(-1, read(fds[0], data, sizeof(data)));
    EXPECT_EQ(EAGAIN, errno);
}

TEST(RdmaChannelFd, InvalidDescriptorReturnsOriginalError) {
    errno = 0;
    EXPECT_EQ(-1, Peer::SetNonBlocking(-1));
    EXPECT_EQ(EBADF, errno);
}

TEST(RdmaChannelFd, SetFlagsFailureIsReturned) {
    // Linux O_PATH descriptors support F_GETFL, but reject F_SETFL.
    const int fd = open(".", O_PATH | O_CLOEXEC);
    ASSERT_GE(fd, 0);
    auto cleanup = MakeScopeGuard([&] { close(fd); });
    const int flags = fcntl(fd, F_GETFL);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(0, flags & O_NONBLOCK);
    errno = 0;
    EXPECT_EQ(-1, Peer::SetNonBlocking(fd));
    EXPECT_EQ(EBADF, errno);
    EXPECT_EQ(flags, fcntl(fd, F_GETFL));
}

}  // namespace
}  // namespace fast
