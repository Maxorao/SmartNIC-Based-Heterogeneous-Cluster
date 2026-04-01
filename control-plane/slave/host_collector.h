/*
 * host_collector.h — Receives host metrics from metric_push via DOCA Comch.
 *
 * The host-side metric_push sends resource_report_t messages over the PCIe
 * Comch channel using the binary protocol (common/protocol.h).  This class
 * runs on the BF2 ARM side, listens for those messages, and exposes the
 * latest snapshot to slave_agent.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#ifndef NO_DOCA
extern "C" {
#include "../../tunnel/comch_api.h"
}
#else
// Forward-declare opaque type so the header compiles without DOCA.
struct comch_nic_ctx;
typedef struct comch_nic_ctx comch_nic_ctx_t;
#endif

class HostCollector {
public:
    /**
     * @param dev_pci      BF2 device PCI address from ARM view (e.g. "03:00.0")
     * @param rep_pci      Host representor PCI address (or "auto")
     * @param service_name Comch service name — must match host side
     */
    HostCollector(const char* dev_pci, const char* rep_pci,
                  const char* service_name);
    ~HostCollector();

    // Non-copyable, non-movable
    HostCollector(const HostCollector&) = delete;
    HostCollector& operator=(const HostCollector&) = delete;

    /** Initialise the Comch NIC-side endpoint and spawn the receive thread. */
    bool start();

    /** Signal the receive thread to stop and join it. */
    void stop();

    struct HostMetrics {
        float    cpu_pct;
        uint64_t mem_total_kb;
        uint64_t mem_avail_kb;
        uint64_t net_rx_bytes;
        uint64_t net_tx_bytes;
        uint64_t timestamp_ns;
        bool     valid;           // false until first message received
    };

    /** Return a copy of the most recent host metrics (thread-safe). */
    HostMetrics getLatest() const;

    /**
     * Check whether a Comch message was received within COMCH_TIMEOUT_NS.
     * If not, the host should be considered unreachable.
     */
    bool isAlive() const;

private:
    comch_nic_ctx_t*  ctx_;
    std::atomic<bool> running_;
    std::thread       recv_thread_;

    mutable std::mutex mu_;
    HostMetrics        latest_;
    uint64_t           last_recv_ns_;

    std::string dev_pci_;
    std::string rep_pci_;
    std::string service_name_;

    void recvLoop();
};
