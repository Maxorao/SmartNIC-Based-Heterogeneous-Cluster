/*
 * forward_routine.c — SmartNIC-side control-plane forwarder
 *
 * Runs on the BlueField-3 ARM (gnodeX-bf, Ubuntu 20.04).
 *
 * Role:
 *   1. Accepts DOCA Comch connection from the host's slave_monitor
 *   2. Forwards received messages to the cluster master via TCP
 *   3. Forwards master ACKs back to the host via Comch
 *
 * This is the "offload" component: by forwarding through the SmartNIC
 * the host CPU is relieved of the TCP stack processing and any network
 * I/O interrupts associated with the control-plane traffic.
 *
 * Usage:
 *   forward_routine --pci=ADDR --master-ip=IP [--master-port=PORT]
 *
 * Options:
 *   --pci=ADDR         NIC PCI address as seen from BlueField ARM (e.g. 03:00.0)
 *   --master-ip=IP     Cluster master IPv4 address
 *   --master-port=PORT Master TCP port (default 9000)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>

#include "../../common/protocol.h"
#include "../../common/timing.h"
#include "../../tunnel/nic/comch_nic.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

static char     g_pci_addr[32]  = "03:00.0";
static char     g_master_ip[64] = "192.168.1.1";
static uint16_t g_master_port   = 9000;

static volatile int g_running = 1;

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */

static void fwd_log(const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    fprintf(stderr, "[%s.%03ld] forwarder: ", tbuf, ts.tv_nsec / 1000000L);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                       */
/* ------------------------------------------------------------------ */

static void sigterm_handler(int sig) { (void)sig; g_running = 0; }

/* ------------------------------------------------------------------ */
/* TCP to master                                                        */
/* ------------------------------------------------------------------ */

static int tcp_connect_master(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(g_master_port),
    };
    if (inet_pton(AF_INET, g_master_ip, &addr.sin_addr) != 1) {
        fwd_log("invalid master IP: %s", g_master_ip);
        close(fd); return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect to master"); close(fd); return -1;
    }
    fwd_log("connected to master %s:%u", g_master_ip, g_master_port);
    return fd;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t fwd_count;       /* messages forwarded host→master */
    uint64_t ack_count;       /* ACKs forwarded master→host */
    uint64_t fwd_latency_sum; /* sum of forwarding latencies (ns) */
    uint64_t reconnects;
    uint64_t errors;
} fwd_stats_t;

static fwd_stats_t g_stats = {0};

/* ------------------------------------------------------------------ */
/* Stats reporter thread                                                */
/* ------------------------------------------------------------------ */

static void *stats_thread(void *arg)
{
    (void)arg;
    uint64_t prev_fwd = 0;
    while (g_running) {
        sleep(10);
        uint64_t fwd  = g_stats.fwd_count;
        uint64_t acks = g_stats.ack_count;
        uint64_t rate = fwd - prev_fwd;
        double avg_lat = (fwd > 0)
            ? (double)g_stats.fwd_latency_sum / (double)fwd / 1000.0
            : 0.0;
        fwd_log("STATS: fwd=%lu acks=%lu rate=%lu/10s avg_lat=%.1f us errors=%lu",
                (unsigned long)fwd, (unsigned long)acks,
                (unsigned long)rate, avg_lat,
                (unsigned long)g_stats.errors);
        prev_fwd = fwd;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Main forwarding loop                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"pci",         required_argument, 0, 'p'},
        {"master-ip",   required_argument, 0, 'i'},
        {"master-port", required_argument, 0, 'P'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
        case 'p': strncpy(g_pci_addr, optarg, sizeof(g_pci_addr)-1); break;
        case 'i': strncpy(g_master_ip, optarg, sizeof(g_master_ip)-1); break;
        case 'P': g_master_port = (uint16_t)atoi(optarg); break;
        case 'h':
        default:
            fprintf(stderr,
                "Usage: %s --pci=ADDR --master-ip=IP [--master-port=PORT]\n",
                argv[0]);
            exit(c == 'h' ? 0 : 1);
        }
    }

    signal(SIGTERM, sigterm_handler);
    signal(SIGINT,  sigterm_handler);
    signal(SIGPIPE, SIG_IGN);

    fwd_log("starting: pci=%s master=%s:%u",
            g_pci_addr, g_master_ip, g_master_port);

    /* Initialise Comch NIC side */
    comch_nic_ctx_t comch;
    doca_error_t ret = comch_nic_init(&comch, g_pci_addr);
    if (ret != DOCA_SUCCESS) {
        fwd_log("FATAL: comch_nic_init failed: %s", doca_error_get_descr(ret));
        return 1;
    }

    /* Connect to master (retry until success) */
    int tcp_fd = -1;
    while (g_running) {
        tcp_fd = tcp_connect_master();
        if (tcp_fd >= 0) break;
        fwd_log("cannot reach master, retry in 3s...");
        sleep(3);
    }
    if (!g_running) goto cleanup;

    /* Start stats reporter */
    pthread_t stats_tid;
    pthread_create(&stats_tid, NULL, stats_thread, NULL);
    pthread_detach(stats_tid);

    /* Message buffers */
    char comch_buf[COMCH_MAX_MSG_SIZE];
    char ack_buf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];

    fwd_log("forwarding loop started");

    while (g_running) {
        /* ---- Receive from host via Comch ---- */
        size_t msg_len = sizeof(comch_buf);
        doca_error_t cr = comch_nic_recv_blocking(&comch, comch_buf,
                                                   &msg_len, 2000);
        if (cr == DOCA_ERROR_TIME_OUT) continue;
        if (cr != DOCA_SUCCESS) {
            fwd_log("comch_recv error: %s — continuing", doca_error_get_descr(cr));
            g_stats.errors++;
            continue;
        }

        uint64_t t0 = now_ns();

        /* Validate protocol header */
        if (msg_len < sizeof(msg_header_t)) {
            fwd_log("short message (%zu bytes), dropping", msg_len);
            g_stats.errors++;
            continue;
        }
        msg_header_t *hdr = (msg_header_t *)comch_buf;
        if (proto_validate(hdr, msg_len) < 0) {
            fwd_log("invalid magic in message, dropping");
            g_stats.errors++;
            continue;
        }

        /* ---- Forward to master via TCP ---- */
        ssize_t sent = 0;
        while (sent < (ssize_t)msg_len) {
            ssize_t n = send(tcp_fd, comch_buf + sent,
                             msg_len - (size_t)sent, MSG_NOSIGNAL);
            if (n <= 0) {
                fwd_log("TCP send to master failed: %s — reconnecting",
                        strerror(errno));
                g_stats.reconnects++;
                g_stats.errors++;
                close(tcp_fd);
                tcp_fd = -1;
                /* Reconnect loop */
                while (g_running && tcp_fd < 0) {
                    sleep(1);
                    tcp_fd = tcp_connect_master();
                }
                if (!g_running) goto cleanup;
                /* Retry the send */
                sent = 0;
                continue;
            }
            sent += n;
        }
        g_stats.fwd_count++;

        /* ---- Read ACK from master ---- */
        msg_header_t ack_hdr;
        char ack_payload[PROTO_MAX_PAYLOAD];
        if (proto_tcp_recv(tcp_fd, &ack_hdr, ack_payload, sizeof(ack_payload)) < 0) {
            fwd_log("TCP recv ACK failed — reconnecting");
            g_stats.errors++;
            close(tcp_fd);
            tcp_fd = -1;
            while (g_running && tcp_fd < 0) {
                sleep(1);
                tcp_fd = tcp_connect_master();
            }
            if (!g_running) goto cleanup;
            continue;
        }

        /* ---- Forward ACK back to host via Comch ---- */
        size_t ack_total = (size_t)proto_build(ack_buf, sizeof(ack_buf),
                                                (msg_type_t)ack_hdr.type,
                                                ack_hdr.seq,
                                                ack_payload, ack_hdr.payload_len);
        cr = comch_nic_send(&comch, ack_buf, ack_total);
        if (cr != DOCA_SUCCESS && cr != DOCA_ERROR_AGAIN)
            fwd_log("comch ACK send error: %s", doca_error_get_descr(cr));
        else
            g_stats.ack_count++;

        uint64_t latency = now_ns() - t0;
        g_stats.fwd_latency_sum += latency;
    }

cleanup:
    if (tcp_fd >= 0) close(tcp_fd);
    comch_nic_destroy(&comch);
    fwd_log("exiting");
    return 0;
}
