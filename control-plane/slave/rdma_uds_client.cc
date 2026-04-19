#include "rdma_uds_client.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

RdmaUdsClient::~RdmaUdsClient()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

void RdmaUdsClient::configure(const std::string& uds_path)
{
    std::lock_guard<std::mutex> lk(mu_);
    uds_path_ = uds_path;
}

bool RdmaUdsClient::ensure_open_locked()
{
    if (fd_ >= 0) return true;
    if (uds_path_.empty()) return false;

    fd_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd_ < 0) { perror("rdma_uds socket"); return false; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, uds_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd_); fd_ = -1;
        return false;
    }

    int bufsize = 1 * 1024 * 1024;
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    /* Non-blocking send to avoid head-of-line blocking */
    struct timeval tv = { 0, 5000 };  /* 5 ms max */
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return true;
}

int RdmaUdsClient::send(const void* buf, uint32_t len)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (uds_path_.empty()) return -ENOTCONN;
    if (!ensure_open_locked()) {
        drops_++;
        return -ENOTCONN;
    }
    ssize_t n = ::send(fd_, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == ECONNREFUSED || errno == EPIPE ||
            errno == ENOTCONN || errno == ENOENT) {
            /* Bridge restarted — close and let next call reopen */
            ::close(fd_); fd_ = -1;
        }
        drops_++;
        return -errno;
    }
    sent_++;
    return 0;
}
