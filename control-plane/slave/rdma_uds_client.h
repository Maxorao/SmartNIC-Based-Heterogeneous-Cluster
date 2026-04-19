/*
 * rdma_uds_client.h — Minimal helper: send a bridge-formatted message to the
 * local rdma_bridge_slave UDS endpoint.
 *
 * Non-blocking, best-effort. Drops silently if bridge is not up.
 */
#pragma once

#include <string>
#include <cstdint>
#include <mutex>

class RdmaUdsClient {
public:
    RdmaUdsClient() = default;
    ~RdmaUdsClient();

    /* Configure (lazy open on first send). */
    void configure(const std::string& uds_path);

    /* Send a bridge-formatted message (header + payload, header already
     * in-place). Returns 0 on success, -errno on transient failure. */
    int  send(const void* buf, uint32_t len);

    bool is_configured() const { return !uds_path_.empty(); }

private:
    std::mutex  mu_;
    std::string uds_path_;
    int         fd_ = -1;
    uint64_t    sent_ = 0;
    uint64_t    drops_ = 0;

    bool ensure_open_locked();
};
