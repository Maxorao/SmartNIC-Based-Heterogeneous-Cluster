/*
 * mock_slave.c — Experiment C: Scalability test
 *
 * Simulates N slave-monitor nodes as pthreads.  Each thread opens a TCP
 * connection to master_monitor and sends periodic MSG_RESOURCE_REPORT
 * messages at 1-second intervals for the test duration.
 *
 * This lets us measure master_monitor's CPU usage and response latency as
 * the number of connected nodes scales from 4 to 256 without needing a
 * physical cluster of that size.
 *
 * Usage:
 *   mock_slave --master-ip=IP --master-port=PORT --nodes=N --duration=SECS
 *
 * Output (stdout): summary table of sent/received/error counts per node
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
#include <getopt.h>

#include "../../common/protocol.h"
#include "../../common/timing.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

static char     g_master_ip[64]   = "192.168.1.1";
static uint16_t g_master_port     = 9000;
static int      g_nodes           = 4;
static int      g_duration        = 60;    /* seconds */
static uint32_t g_interval_ms     = 1000;  /* report interval */

static volatile int g_stop = 0;

/* ------------------------------------------------------------------ */
/* Per-node statistics                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t msgs_sent;
    uint64_t msgs_acked;
    uint64_t send_errors;
    uint64_t connect_errors;
    uint64_t latency_sum_us;   /* sum of send→ACK latency */
} node_stats_t;

static node_stats_t *g_stats = NULL;   /* array[g_nodes] */

/* ------------------------------------------------------------------ */
/* Signal handler                                                       */
/* ------------------------------------------------------------------ */

static void sig_handler(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------------ */
/* Per-node thread                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int    node_index;
    char   node_id[32];
} thread_arg_t;

static void *node_thread(void *arg_ptr)
{
    thread_arg_t *arg = (thread_arg_t *)arg_ptr;
    int idx = arg->node_index;
    char node_id[32];
    strncpy(node_id, arg->node_id, sizeof(node_id) - 1);
    free(arg_ptr);

    node_stats_t *st = &g_stats[idx];

    /* Connect to master */
    int fd = -1;
    while (!g_stop) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { st->connect_errors++; sleep_ms(500); continue; }

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port   = htons(g_master_port),
        };
        if (inet_pton(AF_INET, g_master_ip, &addr.sin_addr) != 1) {
            fprintf(stderr, "invalid master IP\n");
            close(fd); fd = -1; break;
        }
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            break;
        close(fd); fd = -1;
        st->connect_errors++;
        sleep_ms(200 + (uint32_t)(idx % 50) * 10);  /* stagger reconnects */
    }
    if (fd < 0) return NULL;

    /* REGISTER */
    char buf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];
    register_payload_t reg = {0};
    strncpy(reg.node_id, node_id, sizeof(reg.node_id) - 1);
    snprintf(reg.version, sizeof(reg.version), "mock-1.0");

    uint32_t seq = 0;
    int total = proto_build(buf, sizeof(buf), MSG_REGISTER, seq++,
                             &reg, sizeof(reg));
    send(fd, buf, (size_t)total, MSG_NOSIGNAL);

    /* Receive REGISTER_ACK (best effort) */
    {
        msg_header_t hdr;
        char payload[PROTO_MAX_PAYLOAD];
        /* Non-blocking drain — ignore errors at registration */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        proto_tcp_recv(fd, &hdr, payload, sizeof(payload));
        /* Reset to blocking */
        tv.tv_sec = 2; tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    uint64_t deadline = now_ns() + (uint64_t)g_duration * 1000000000ULL;

    while (!g_stop && now_ns() < deadline) {
        /* Build resource report with synthetic data */
        resource_report_t report = {0};
        strncpy(report.node_id, node_id, sizeof(report.node_id) - 1);
        report.timestamp_ns  = unix_ns();
        report.cpu_usage_pct = 20.0f + (float)(idx % 60);
        report.mem_total_kb  = 16ULL * 1024 * 1024;    /* 16 GB */
        report.mem_avail_kb  = 8ULL  * 1024 * 1024;
        report.net_rx_bytes  = (uint64_t)idx * 1000000 + st->msgs_sent;
        report.net_tx_bytes  = (uint64_t)idx * 500000  + st->msgs_sent;

        total = proto_build(buf, sizeof(buf), MSG_RESOURCE_REPORT, seq++,
                             &report, sizeof(report));

        uint64_t t0 = now_ns();
        ssize_t n = send(fd, buf, (size_t)total, MSG_NOSIGNAL);
        if (n != (ssize_t)total) {
            st->send_errors++;
            /* Try to reconnect */
            close(fd);
            fd = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in addr2 = {
                .sin_family = AF_INET,
                .sin_port   = htons(g_master_port),
            };
            inet_pton(AF_INET, g_master_ip, &addr2.sin_addr);
            if (connect(fd, (struct sockaddr *)&addr2, sizeof(addr2)) < 0) {
                close(fd); fd = -1; break;
            }
            sleep_ms(g_interval_ms);
            continue;
        }
        st->msgs_sent++;

        /* Wait for ACK — master does not always ACK resource reports,
         * but may send MSG_HEARTBEAT_ACK; try non-blocking drain */
        {
            msg_header_t hdr;
            char payload[PROTO_MAX_PAYLOAD];
            int saved_errno = errno;
            /* Set 500ms timeout */
            struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (proto_tcp_recv(fd, &hdr, payload, sizeof(payload)) == 0) {
                uint64_t lat_us = (now_ns() - t0) / 1000;
                st->latency_sum_us += lat_us;
                st->msgs_acked++;
            }
            /* Reset timeout to 2s */
            tv.tv_sec = 2; tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            (void)saved_errno;
        }

        sleep_ms(g_interval_ms);
    }

    /* DEREGISTER */
    if (fd >= 0) {
        total = proto_build(buf, sizeof(buf), MSG_DEREGISTER, seq++, NULL, 0);
        send(fd, buf, (size_t)total, MSG_NOSIGNAL);
        close(fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"master-ip",   required_argument, 0, 'i'},
        {"master-port", required_argument, 0, 'p'},
        {"nodes",       required_argument, 0, 'n'},
        {"duration",    required_argument, 0, 'd'},
        {"interval",    required_argument, 0, 't'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
        case 'i': strncpy(g_master_ip, optarg, sizeof(g_master_ip)-1); break;
        case 'p': g_master_port = (uint16_t)atoi(optarg); break;
        case 'n': g_nodes       = atoi(optarg); break;
        case 'd': g_duration    = atoi(optarg); break;
        case 't': g_interval_ms = (uint32_t)atoi(optarg); break;
        case 'h':
        default:
            fprintf(stderr,
                "Usage: %s --master-ip=IP --master-port=PORT "
                "--nodes=N --duration=SECS [--interval=MS]\n",
                argv[0]);
            exit(c == 'h' ? 0 : 1);
        }
    }

    if (g_nodes <= 0 || g_nodes > 65536) {
        fprintf(stderr, "nodes must be 1..65536\n"); return 1;
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr,
            "mock_slave: master=%s:%u  nodes=%d  duration=%ds  interval=%ums\n",
            g_master_ip, g_master_port, g_nodes, g_duration, g_interval_ms);

    g_stats = calloc((size_t)g_nodes, sizeof(node_stats_t));
    if (!g_stats) { perror("calloc"); return 1; }

    pthread_t *tids = calloc((size_t)g_nodes, sizeof(pthread_t));
    if (!tids) { perror("calloc"); return 1; }

    /* Launch all node threads with a small stagger to avoid thundering herd */
    for (int i = 0; i < g_nodes; i++) {
        thread_arg_t *a = malloc(sizeof(thread_arg_t));
        if (!a) { perror("malloc"); continue; }
        a->node_index = i;
        snprintf(a->node_id, sizeof(a->node_id), "mock-node-%04d", i);

        if (pthread_create(&tids[i], NULL, node_thread, a) != 0) {
            perror("pthread_create");
            free(a);
        }

        /* Stagger: 5ms per node, max 500ms total */
        if (g_nodes > 1 && i % 100 == 99)
            sleep_ms(50);
        else
            sleep_ms(5);
    }

    /* Wait for all threads */
    for (int i = 0; i < g_nodes; i++) {
        if (tids[i]) pthread_join(tids[i], NULL);
    }

    /* Print summary */
    uint64_t total_sent  = 0, total_acked = 0, total_errors = 0;
    uint64_t total_lat   = 0, lat_count   = 0;

    printf("\n=== mock_slave results (%d nodes) ===\n", g_nodes);
    printf("%-16s  %8s  %8s  %8s  %12s\n",
           "node_id", "sent", "acked", "errors", "avg_lat_us");

    for (int i = 0; i < g_nodes && i < 20; i++) {  /* print first 20 */
        node_stats_t *s = &g_stats[i];
        double avg_lat = s->msgs_acked > 0
                         ? (double)s->latency_sum_us / (double)s->msgs_acked
                         : 0.0;
        printf("mock-node-%04d  %8" PRIu64 "  %8" PRIu64 "  %8" PRIu64
               "  %12.1f\n",
               i, s->msgs_sent, s->msgs_acked, s->send_errors, avg_lat);
        total_sent   += s->msgs_sent;
        total_acked  += s->msgs_acked;
        total_errors += s->send_errors;
        if (s->msgs_acked > 0) {
            total_lat  += s->latency_sum_us;
            lat_count  += s->msgs_acked;
        }
    }
    if (g_nodes > 20) {
        /* Aggregate remaining */
        for (int i = 20; i < g_nodes; i++) {
            node_stats_t *s = &g_stats[i];
            total_sent   += s->msgs_sent;
            total_acked  += s->msgs_acked;
            total_errors += s->send_errors;
            if (s->msgs_acked > 0) {
                total_lat += s->latency_sum_us;
                lat_count += s->msgs_acked;
            }
        }
        printf("... (%d more nodes not shown individually)\n", g_nodes - 20);
    }

    double overall_avg_lat = lat_count > 0
                             ? (double)total_lat / (double)lat_count
                             : 0.0;
    printf("\nTOTAL: sent=%" PRIu64 "  acked=%" PRIu64
           "  errors=%" PRIu64 "  avg_lat=%.1f us\n",
           total_sent, total_acked, total_errors, overall_avg_lat);

    if (total_sent > 0) {
        printf("Error rate: %.2f%%\n",
               (double)total_errors / (double)total_sent * 100.0);
    }

    free(g_stats);
    free(tids);
    return 0;
}
