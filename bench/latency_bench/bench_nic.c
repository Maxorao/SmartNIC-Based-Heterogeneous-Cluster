/*
 * bench_nic.c — Experiment A: Echo server (NIC/ARM side)
 *
 * Runs on the BlueField-3 ARM.
 * Echoes every received message back to the sender as MSG_BENCH_PONG.
 *
 * Two modes:
 *   comch  — DOCA Comch echo (PCIe kernel-bypass path)
 *   tcp    — TCP echo server on port 12345 (kernel TCP baseline)
 *
 * Usage:
 *   bench_nic --pci=ADDR [--mode=comch|tcp]
 *
 * Start bench_nic before bench_host.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "../../common/protocol.h"
#include "../../common/timing.h"
#include "../../tunnel/nic/comch_nic.h"

#define TCP_ECHO_PORT 12345

static char g_pci_addr[32] = "03:00.0";
static volatile int g_running = 1;

typedef enum { BENCH_MODE_COMCH, BENCH_MODE_TCP } bench_mode_t;
static bench_mode_t g_mode = BENCH_MODE_COMCH;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

/* ------------------------------------------------------------------ */
/* Comch echo loop                                                      */
/* ------------------------------------------------------------------ */

static void run_comch_echo(void)
{
    comch_nic_ctx_t ctx;
    doca_error_t ret = comch_nic_init(&ctx, g_pci_addr);
    if (ret != DOCA_SUCCESS) {
        fprintf(stderr, "comch_nic_init: %s\n", doca_error_get_descr(ret));
        return;
    }

    fprintf(stderr, "bench_nic (comch): echo server ready, waiting for host...\n");

    char buf[COMCH_MAX_MSG_SIZE];
    char reply_buf[COMCH_MAX_MSG_SIZE];
    uint64_t count = 0;

    while (g_running) {
        size_t len = sizeof(buf);
        ret = comch_nic_recv_blocking(&ctx, buf, &len, 1000);
        if (ret == DOCA_ERROR_TIME_OUT) continue;
        if (ret != DOCA_SUCCESS) {
            fprintf(stderr, "comch_recv error: %s\n", doca_error_get_descr(ret));
            break;
        }

        /* Validate and convert PING → PONG */
        if (len < sizeof(msg_header_t)) continue;
        msg_header_t *hdr = (msg_header_t *)buf;
        if (proto_validate(hdr, len) < 0) continue;
        if (hdr->type != MSG_BENCH_PING) continue;

        /* Build PONG: copy payload verbatim, change type */
        int total = proto_build(reply_buf, sizeof(reply_buf),
                                 MSG_BENCH_PONG, hdr->seq,
                                 buf + sizeof(msg_header_t),
                                 hdr->payload_len);
        if (total < 0) continue;

        ret = comch_nic_send(&ctx, reply_buf, (size_t)total);
        if (ret != DOCA_SUCCESS && ret != DOCA_ERROR_AGAIN)
            fprintf(stderr, "comch_send error: %s\n", doca_error_get_descr(ret));

        count++;
        if (count % 1000 == 0)
            fprintf(stderr, "echoed %lu messages\n", (unsigned long)count);
    }

    comch_nic_destroy(&ctx);
    fprintf(stderr, "bench_nic (comch): done, echoed %lu messages\n",
            (unsigned long)count);
}

/* ------------------------------------------------------------------ */
/* TCP echo: per-client thread                                          */
/* ------------------------------------------------------------------ */

typedef struct { int fd; } tcp_client_arg_t;

static void *tcp_echo_client(void *arg)
{
    tcp_client_arg_t *ca = (tcp_client_arg_t *)arg;
    int fd = ca->fd;
    free(ca);

    char buf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];
    while (g_running) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;  /* client disconnected */

        /* Echo exactly what we received */
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t w = send(fd, buf + sent, (size_t)(n - sent), MSG_NOSIGNAL);
            if (w <= 0) goto done;
            sent += w;
        }
    }
done:
    close(fd);
    return NULL;
}

static void run_tcp_echo(void)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(TCP_ECHO_PORT),
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); return;
    }
    listen(srv, 8);
    fprintf(stderr, "bench_nic (tcp): echo server listening on :%u\n",
            TCP_ECHO_PORT);

    while (g_running) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        tcp_client_arg_t *ca = malloc(sizeof(*ca));
        ca->fd = cfd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, tcp_echo_client, ca) != 0) {
            free(ca); close(cfd);
        } else {
            pthread_detach(tid);
        }
    }
    close(srv);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"pci",  required_argument, 0, 'p'},
        {"mode", required_argument, 0, 'm'},
        {"help", no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
        case 'p': strncpy(g_pci_addr, optarg, sizeof(g_pci_addr)-1); break;
        case 'm':
            if (strcmp(optarg, "comch") == 0) g_mode = BENCH_MODE_COMCH;
            else if (strcmp(optarg, "tcp") == 0) g_mode = BENCH_MODE_TCP;
            else { fprintf(stderr, "Unknown mode: %s\n", optarg); exit(1); }
            break;
        case 'h':
        default:
            fprintf(stderr,
                "Usage: %s --pci=ADDR [--mode=comch|tcp]\n", argv[0]);
            exit(c == 'h' ? 0 : 1);
        }
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (g_mode == BENCH_MODE_COMCH)
        run_comch_echo();
    else
        run_tcp_echo();

    return 0;
}
