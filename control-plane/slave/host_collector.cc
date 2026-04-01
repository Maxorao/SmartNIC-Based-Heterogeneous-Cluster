/*
 * host_collector.cc — Comch NIC-side receiver for host metrics.
 *
 * Runs a background thread that blocks on comch_nic_recv_blocking().
 * Each received message is validated against the binary protocol and,
 * if it is a MSG_RESOURCE_REPORT, the payload is extracted into the
 * latest HostMetrics snapshot.
 *
 * When compiled with NO_DOCA, start() returns false immediately.
 */

#include "host_collector.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include "../../common/protocol.h"
#include "../../common/timing.h"
#include "../../common/node_state.h"
}

/* Fallback service name when comch_api.h is not included. */
#ifndef COMCH_SERVICE_NAME
#  define COMCH_SERVICE_NAME SERVICE_NAME
#endif

#ifndef COMCH_MAX_MSG_SIZE
#  define COMCH_MAX_MSG_SIZE 4080U
#endif

/* ------------------------------------------------------------------ */
/* Logging helper                                                      */
/* ------------------------------------------------------------------ */

static void hc_log(const char* fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    fprintf(stderr, "[%s.%03ld] host_collector: ", tbuf,
            ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* Construction / destruction                                          */
/* ------------------------------------------------------------------ */

HostCollector::HostCollector(const char* dev_pci, const char* rep_pci,
                             const char* service_name)
    : ctx_(nullptr)
    , running_(false)
    , latest_{}
    , last_recv_ns_(0)
    , dev_pci_(dev_pci ? dev_pci : "03:00.0")
    , rep_pci_(rep_pci ? rep_pci : "auto")
    , service_name_(service_name ? service_name : COMCH_SERVICE_NAME)
{
    latest_.valid = false;
}

HostCollector::~HostCollector()
{
    stop();
}

/* ------------------------------------------------------------------ */
/* Start / stop                                                        */
/* ------------------------------------------------------------------ */

bool HostCollector::start()
{
    if (running_.load())
        return true;  // already running

#ifdef NO_DOCA
    hc_log("compiled without DOCA — cannot start Comch receiver");
    return false;
#else
    doca_error_t err = comch_nic_init(&ctx_,
                                       dev_pci_.c_str(),
                                       rep_pci_.c_str(),
                                       service_name_.c_str());
    if (err != DOCA_SUCCESS) {
        hc_log("comch_nic_init failed: %s", doca_error_get_name(err));
        return false;
    }

    hc_log("Comch NIC endpoint initialised (dev=%s rep=%s svc=%s)",
           dev_pci_.c_str(), rep_pci_.c_str(), service_name_.c_str());

    running_.store(true);
    recv_thread_ = std::thread(&HostCollector::recvLoop, this);
    return true;
#endif
}

void HostCollector::stop()
{
    if (!running_.load())
        return;

    running_.store(false);

    if (recv_thread_.joinable())
        recv_thread_.join();

#ifndef NO_DOCA
    if (ctx_) {
        comch_nic_destroy(ctx_);
        ctx_ = nullptr;
    }
#endif

    hc_log("stopped");
}

/* ------------------------------------------------------------------ */
/* Receive loop (runs in dedicated thread)                             */
/* ------------------------------------------------------------------ */

void HostCollector::recvLoop()
{
#ifdef NO_DOCA
    // Should never be called, but guard defensively.
    return;
#else
    uint8_t buf[COMCH_MAX_MSG_SIZE];
    hc_log("receive thread started");

    while (running_.load()) {
        size_t len = sizeof(buf);
        doca_error_t err = comch_nic_recv_blocking(ctx_, buf, &len, 2000);

        if (err == DOCA_ERROR_AGAIN) {
            // Timeout — no message within 2 seconds.  Just retry.
            continue;
        }
        if (err != DOCA_SUCCESS) {
            hc_log("recv error: %s", doca_error_get_name(err));
            sleep_ms(100);  // avoid tight error loop
            continue;
        }

        // Validate the binary protocol header.
        const auto* hdr = reinterpret_cast<const msg_header_t*>(buf);
        if (proto_validate(hdr, len) != 0) {
            hc_log("received invalid message (len=%zu)", len);
            continue;
        }

        if (hdr->type == MSG_RESOURCE_REPORT) {
            if (hdr->payload_len < sizeof(resource_report_t)) {
                hc_log("RESOURCE_REPORT payload too short (%u < %zu)",
                       hdr->payload_len, sizeof(resource_report_t));
                continue;
            }

            const auto* rr = reinterpret_cast<const resource_report_t*>(
                buf + sizeof(msg_header_t));

            std::lock_guard<std::mutex> lock(mu_);
            latest_.cpu_pct      = rr->cpu_usage_pct;
            latest_.mem_total_kb = rr->mem_total_kb;
            latest_.mem_avail_kb = rr->mem_avail_kb;
            latest_.net_rx_bytes = rr->net_rx_bytes;
            latest_.net_tx_bytes = rr->net_tx_bytes;
            latest_.timestamp_ns = rr->timestamp_ns;
            latest_.valid        = true;
            last_recv_ns_        = now_ns();

        } else if (hdr->type == MSG_HEARTBEAT) {
            // Host heartbeat — just update the "last seen" timestamp.
            std::lock_guard<std::mutex> lock(mu_);
            last_recv_ns_ = now_ns();

        } else if (hdr->type == MSG_REGISTER) {
            // Host registration — update last seen.
            std::lock_guard<std::mutex> lock(mu_);
            last_recv_ns_ = now_ns();
            hc_log("host registered");

        } else if (hdr->type == MSG_DEREGISTER) {
            hc_log("host deregistered");
            std::lock_guard<std::mutex> lock(mu_);
            latest_.valid = false;

        } else {
            hc_log("ignoring message type %s (%u)",
                   msg_type_str(hdr->type), hdr->type);
        }
    }

    hc_log("receive thread exiting");
#endif
}

/* ------------------------------------------------------------------ */
/* Accessors (thread-safe)                                             */
/* ------------------------------------------------------------------ */

HostCollector::HostMetrics HostCollector::getLatest() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return latest_;
}

bool HostCollector::isAlive() const
{
    std::lock_guard<std::mutex> lock(mu_);
    if (last_recv_ns_ == 0)
        return false;  // never received anything
    uint64_t elapsed = now_ns() - last_recv_ns_;
    return elapsed < COMCH_TIMEOUT_NS;
}
