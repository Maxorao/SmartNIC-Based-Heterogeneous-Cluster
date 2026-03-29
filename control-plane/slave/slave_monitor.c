/*
 * slave_monitor.c — Worker node resource monitor
 *
 * Runs on the x86 host (gnode2-4, Ubuntu 22.04).
 * Collects CPU/memory/network metrics from /proc and sends them to the
 * cluster master — either via the SmartNIC DOCA Comch offload path, or
 * directly over TCP (for baseline/interference experiments).
 *
 * Usage:
 *   slave_monitor [OPTIONS]
 *
 * Options:
 *   --mode=offload|direct   Transmission path (default: offload)
 *   --pci=ADDR              BlueField-3 PCI addr for offload mode (default: 03:00.0)
 *   --master-ip=IP          Master IP for direct mode (default: 192.168.1.1)
 *   --master-port=PORT      Master port (default: 9000)
 *   --interval=MS           Report interval in ms (default: 1000)
 *   --node-id=NAME          Override hostname as node identifier
 *   --iface=NAME            Network interface to monitor (default: eth0)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <getopt.h>
#include <stdarg.h>
#include <math.h>

#include "../../common/protocol.h"
#include "../../common/timing.h"

/* Only pull in DOCA headers when building offload mode */
#ifndef NO_DOCA
#include "../../tunnel/comch_api.h"
#endif

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

typedef enum { MODE_OFFLOAD, MODE_DIRECT } transport_mode_t;

typedef struct {
    transport_mode_t mode;
    char pci_addr[32];
    char master_ip[64];
    uint16_t master_port;
    uint32_t interval_ms;
    char node_id[64];
    char iface[32];
    uint32_t extra_reads;   /* extra /proc reads per cycle (simulate kubelet) */
    uint32_t cache_kb;      /* KB of memory to walk per cycle (cache pressure) */
} config_t;

static config_t g_cfg = {
    .mode        = MODE_OFFLOAD,
    .pci_addr    = "03:00.0",
    .master_ip   = "192.168.1.1",
    .master_port = 9000,
    .interval_ms = 1000,
    .node_id     = "",
    .iface       = "eth0",
    .extra_reads = 0,
    .cache_kb    = 0,
};

static volatile int g_running = 1;

/* ------------------------------------------------------------------ */
/* Signal handling                                                      */
/* ------------------------------------------------------------------ */

static void sigterm_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */

static void log_msg(const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    fprintf(stderr, "[%s.%03ld] slave_monitor: ", tbuf,
            ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* /proc CPU sampling                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_stat_t;

static int read_cpu_stat(cpu_stat_t *s)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    int rc = fscanf(f, "cpu  %" SCNu64 " %" SCNu64 " %" SCNu64
                       " %" SCNu64 " %" SCNu64 " %" SCNu64
                       " %" SCNu64 " %" SCNu64,
                    &s->user, &s->nice, &s->system, &s->idle,
                    &s->iowait, &s->irq, &s->softirq, &s->steal);
    fclose(f);
    return (rc == 8) ? 0 : -1;
}

static float calc_cpu_usage(const cpu_stat_t *prev, const cpu_stat_t *cur)
{
    uint64_t prev_idle  = prev->idle + prev->iowait;
    uint64_t cur_idle   = cur->idle  + cur->iowait;
    uint64_t prev_total = prev->user + prev->nice + prev->system + prev_idle
                        + prev->irq + prev->softirq + prev->steal;
    uint64_t cur_total  = cur->user  + cur->nice  + cur->system  + cur_idle
                        + cur->irq  + cur->softirq  + cur->steal;
    uint64_t total_diff = cur_total - prev_total;
    uint64_t idle_diff  = cur_idle  - prev_idle;
    if (total_diff == 0) return 0.0f;
    return (float)(total_diff - idle_diff) * 100.0f / (float)total_diff;
}

/* ------------------------------------------------------------------ */
/* /proc/meminfo sampling                                               */
/* ------------------------------------------------------------------ */

static void read_mem(uint64_t *total_kb, uint64_t *avail_kb)
{
    *total_kb = 0; *avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char key[64]; uint64_t val; char unit[8];
    while (fscanf(f, "%63s %" SCNu64 " %7s", key, &val, unit) >= 2) {
        if (strcmp(key, "MemTotal:") == 0)     *total_kb = val;
        if (strcmp(key, "MemAvailable:") == 0) *avail_kb = val;
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* /proc/net/dev sampling                                               */
/* ------------------------------------------------------------------ */

typedef struct { uint64_t rx_bytes, tx_bytes; } net_stat_t;

static int read_net_stat(const char *iface, net_stat_t *s)
{
    s->rx_bytes = s->tx_bytes = 0;
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return -1;
    char line[256];
    /* skip 2 header lines */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    while (fgets(line, sizeof(line), f)) {
        char name[32];
        uint64_t rx, dummy[6], tx;
        /* Format: iface: rx_bytes packets errs drop fifo frame compressed multicast
         *                tx_bytes ... */
        int n = sscanf(line, " %31[^:]: %" SCNu64 " %*u %*u %*u %*u %*u %*u %*u"
                             " %" SCNu64,
                       name, &rx, &tx);
        if (n == 3 && strcmp(name, iface) == 0) {
            s->rx_bytes = rx;
            s->tx_bytes = tx;
            fclose(f);
            return 0;
        }
        (void)dummy;
    }
    fclose(f);
    return -1;
}

/* ------------------------------------------------------------------ */
/* TCP direct transport                                                  */
/* ------------------------------------------------------------------ */

static int tcp_connect(const char *ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    /* Enable TCP keepalive */
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid IP: %s\n", ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* Synthetic workload (simulates kubelet + cAdvisor + logging overhead) */
/* ------------------------------------------------------------------ */

/*
 * Extra /proc reads: simulates kubelet reading /proc/[pid]/stat for
 * each container, /proc/diskstats, /proc/net/dev, cgroup files, etc.
 * Each read involves open + read + parse + close syscalls.
 */
static void do_extra_proc_reads(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        FILE *f = fopen("/proc/stat", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                /* Force the compiler to "use" the data */
                volatile char sink = line[0];
                (void)sink;
            }
            fclose(f);
        }
    }
}

/*
 * Cache pressure: walk through a buffer touching every cache line.
 * Simulates metric aggregation, log buffering, JSON serialization —
 * working-set-sized memory access that pollutes shared L3 cache.
 */
static char *g_cache_buf = NULL;
static uint32_t g_cache_buf_size = 0;

static void do_cache_work(uint32_t kb)
{
    if (kb == 0) return;

    uint32_t size = kb * 1024;
    if (!g_cache_buf || g_cache_buf_size != size) {
        free(g_cache_buf);
        g_cache_buf = malloc(size);
        g_cache_buf_size = size;
        if (!g_cache_buf) return;
        memset(g_cache_buf, 0, size);
    }

    /* Read+write every cache line (64B stride) to force evictions */
    volatile uint64_t checksum = 0;
    for (uint32_t i = 0; i < size; i += 64) {
        checksum += (uint64_t)(unsigned char)g_cache_buf[i];
        g_cache_buf[i] = (char)(checksum & 0xFF);
    }
}

/* ------------------------------------------------------------------ */
/* Main monitor loop                                                    */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    /* Parse arguments */
    static struct option long_opts[] = {
        {"mode",        required_argument, 0, 'm'},
        {"pci",         required_argument, 0, 'p'},
        {"master-ip",   required_argument, 0, 'i'},
        {"master-port", required_argument, 0, 'P'},
        {"interval",    required_argument, 0, 't'},
        {"node-id",     required_argument, 0, 'n'},
        {"iface",        required_argument, 0, 'f'},
        {"extra-reads",  required_argument, 0, 'r'},
        {"cache-kb",     required_argument, 0, 'c'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
        case 'm':
            if (strcmp(optarg, "direct") == 0)  g_cfg.mode = MODE_DIRECT;
            else if (strcmp(optarg, "offload") == 0) g_cfg.mode = MODE_OFFLOAD;
            else { fprintf(stderr, "Unknown mode: %s\n", optarg); exit(1); }
            break;
        case 'p': strncpy(g_cfg.pci_addr, optarg, sizeof(g_cfg.pci_addr)-1); break;
        case 'i': strncpy(g_cfg.master_ip, optarg, sizeof(g_cfg.master_ip)-1); break;
        case 'P': g_cfg.master_port = (uint16_t)atoi(optarg); break;
        case 't': g_cfg.interval_ms = (uint32_t)atoi(optarg); break;
        case 'n': strncpy(g_cfg.node_id, optarg, sizeof(g_cfg.node_id)-1); break;
        case 'f': strncpy(g_cfg.iface, optarg, sizeof(g_cfg.iface)-1); break;
        case 'r': g_cfg.extra_reads = (uint32_t)atoi(optarg); break;
        case 'c': g_cfg.cache_kb = (uint32_t)atoi(optarg); break;
        case 'h':
        default:
            fprintf(stderr,
                "Usage: %s [--mode=offload|direct] [--pci=ADDR]\n"
                "          [--master-ip=IP] [--master-port=PORT]\n"
                "          [--interval=MS] [--node-id=NAME] [--iface=IFACE]\n"
                "          [--extra-reads=N] [--cache-kb=KB]\n",
                argv[0]);
            exit(c == 'h' ? 0 : 1);
        }
    }

    /* Default node_id to hostname */
    if (g_cfg.node_id[0] == '\0')
        gethostname(g_cfg.node_id, sizeof(g_cfg.node_id) - 1);

    signal(SIGTERM, sigterm_handler);
    signal(SIGINT,  sigterm_handler);
    signal(SIGPIPE, SIG_IGN);

    log_msg("starting: node_id=%s mode=%s interval=%ums",
            g_cfg.node_id,
            g_cfg.mode == MODE_OFFLOAD ? "offload" : "direct",
            g_cfg.interval_ms);

    /* ---- Transport initialisation ---- */

#ifndef NO_DOCA
    comch_host_ctx_t *comch_ctx = NULL;
#endif
    int tcp_fd = -1;

    if (g_cfg.mode == MODE_OFFLOAD) {
#ifndef NO_DOCA
        doca_error_t ret = comch_host_init(&comch_ctx, g_cfg.pci_addr, COMCH_SERVICE_NAME);
        if (ret != DOCA_SUCCESS) {
            log_msg("FATAL: comch_host_init failed: %s",
                    doca_error_get_name(ret));
            exit(1);
        }
        log_msg("offload path initialised (PCI %s)", g_cfg.pci_addr);
#else
        log_msg("FATAL: built without DOCA; cannot use offload mode");
        exit(1);
#endif
    } else {
        /* Direct TCP mode — connect to master */
        while (g_running) {
            tcp_fd = tcp_connect(g_cfg.master_ip, g_cfg.master_port);
            if (tcp_fd >= 0) break;
            log_msg("connect failed, retrying in 2s...");
            sleep(2);
        }
        if (!g_running) goto cleanup;
        log_msg("direct TCP connected to %s:%u",
                g_cfg.master_ip, g_cfg.master_port);
    }

    /* ---- Send REGISTER ---- */

    register_payload_t reg = {0};
    strncpy(reg.node_id, g_cfg.node_id, sizeof(reg.node_id) - 1);
    snprintf(reg.version, sizeof(reg.version), "1.0");

    uint32_t seq = 0;
    char msg_buf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];

    int total = proto_build(msg_buf, sizeof(msg_buf),
                             MSG_REGISTER, seq++,
                             &reg, sizeof(reg));

    if (g_cfg.mode == MODE_DIRECT) {
        ssize_t sent = send(tcp_fd, msg_buf, (size_t)total, MSG_NOSIGNAL);
        if (sent != total) {
            log_msg("REGISTER send failed");
            goto cleanup;
        }
    } else {
#ifndef NO_DOCA
        doca_error_t ret = comch_host_send(comch_ctx, msg_buf, (size_t)total);
        if (ret != DOCA_SUCCESS) {
            log_msg("REGISTER comch_send failed");
            goto cleanup;
        }
#endif
    }
    log_msg("REGISTER sent");

    /* ---- Resource reporting loop ---- */

    cpu_stat_t prev_cpu, cur_cpu;
    net_stat_t prev_net, cur_net;
    read_cpu_stat(&prev_cpu);
    read_net_stat(g_cfg.iface, &prev_net);

    /* previous report for change detection */
    resource_report_t prev_report = {0};

    while (g_running) {
        sleep_ms(g_cfg.interval_ms);
        if (!g_running) break;

        /* Collect metrics */
        read_cpu_stat(&cur_cpu);
        read_net_stat(g_cfg.iface, &cur_net);

        resource_report_t report = {0};
        strncpy(report.node_id, g_cfg.node_id, sizeof(report.node_id) - 1);
        report.timestamp_ns = unix_ns();
        report.cpu_usage_pct = calc_cpu_usage(&prev_cpu, &cur_cpu);
        read_mem(&report.mem_total_kb, &report.mem_avail_kb);
        report.net_rx_bytes = cur_net.rx_bytes;
        report.net_tx_bytes = cur_net.tx_bytes;
        /* PCIe bytes not available via /proc; set to 0 */
        report.pcie_rx_bytes = 0;
        report.pcie_tx_bytes = 0;

        prev_cpu = cur_cpu;
        prev_net = cur_net;

        /* Synthetic workload: simulate kubelet/cAdvisor overhead */
        do_extra_proc_reads(g_cfg.extra_reads);
        do_cache_work(g_cfg.cache_kb);

        /* Decide: heartbeat (no change) or full report */
        msg_type_t mtype;
        uint32_t payload_len;
        void *payload_ptr;

        int changed = (fabsf(report.cpu_usage_pct - prev_report.cpu_usage_pct) > 0.5f)
                   || (report.mem_avail_kb != prev_report.mem_avail_kb)
                   || (report.net_rx_bytes != prev_report.net_rx_bytes);

        if (changed) {
            mtype = MSG_RESOURCE_REPORT;
            payload_len = sizeof(resource_report_t);
            payload_ptr = &report;
            prev_report = report;
        } else {
            mtype = MSG_HEARTBEAT;
            payload_len = 0;
            payload_ptr = NULL;
        }

        total = proto_build(msg_buf, sizeof(msg_buf),
                             mtype, seq++, payload_ptr, payload_len);

        if (g_cfg.mode == MODE_DIRECT) {
            ssize_t sent = send(tcp_fd, msg_buf, (size_t)total, MSG_NOSIGNAL);
            if (sent != total) {
                log_msg("send error: %s — trying to reconnect", strerror(errno));
                close(tcp_fd);
                tcp_fd = tcp_connect(g_cfg.master_ip, g_cfg.master_port);
                if (tcp_fd < 0) { log_msg("reconnect failed"); break; }
                continue;
            }
        } else {
#ifndef NO_DOCA
            doca_error_t ret = comch_host_send(comch_ctx, msg_buf, (size_t)total);
            if (ret != DOCA_SUCCESS && ret != DOCA_ERROR_AGAIN) {
                log_msg("comch send error: %s", doca_error_get_name(ret));
                break;
            }
#endif
        }

        if (mtype == MSG_RESOURCE_REPORT)
            log_msg("REPORT sent: cpu=%.1f%% mem_avail=%" PRIu64 "kB",
                    report.cpu_usage_pct, report.mem_avail_kb);
    }

    /* ---- Send DEREGISTER ---- */
    total = proto_build(msg_buf, sizeof(msg_buf),
                         MSG_DEREGISTER, seq++, NULL, 0);
    if (g_cfg.mode == MODE_DIRECT && tcp_fd >= 0)
        send(tcp_fd, msg_buf, (size_t)total, MSG_NOSIGNAL);
#ifndef NO_DOCA
    else if (g_cfg.mode == MODE_OFFLOAD)
        comch_host_send(comch_ctx, msg_buf, (size_t)total);
#endif
    log_msg("DEREGISTER sent");

cleanup:
    if (tcp_fd >= 0) close(tcp_fd);
#ifndef NO_DOCA
    if (g_cfg.mode == MODE_OFFLOAD) comch_host_destroy(comch_ctx);
#endif
    log_msg("exiting");
    return 0;
}
