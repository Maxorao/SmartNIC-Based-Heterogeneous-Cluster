/*
 * e2e_nic.c — NIC-side forwarder for the Comch ↔ RDMA ↔ Comch chained bench.
 *
 * Two roles:
 *   --mode=server   (runs on BF2-B / ponger side):
 *                   1. Start RDMA server, wait for incoming connection.
 *                   2. Start Comch NIC endpoint (to local host ponger).
 *                   3. Loop:
 *                      a. Receive RDMA Send from peer BF2
 *                      b. Forward to host via Comch
 *                      c. Receive Comch reply from host
 *                      d. Forward back via RDMA Send
 *
 *   --mode=client   (runs on BF2-A / pinger side):
 *                   1. Start Comch NIC endpoint first (wait for host connection).
 *                   2. Connect to remote BF2 via RDMA.
 *                   3. Loop:
 *                      a. Receive Comch from host
 *                      b. Forward via RDMA Send to remote BF2
 *                      c. Receive RDMA reply
 *                      d. Forward back to host via Comch
 *
 * Usage:
 *   e2e_nic --mode=client --peer-ip=<remote-SF-IP> --dev-pci=03:00.0
 *           [--rep-pci=auto] [--port=7888] [--msg-size=4096]
 *   e2e_nic --mode=server --bind-ip=<local-SF-IP>  --dev-pci=03:00.0
 *           [--rep-pci=auto] [--port=7888] [--msg-size=4096]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <inttypes.h>
#include <time.h>

#include "../../common/protocol.h"
#include "../../common/timing.h"
#include "../../common/rdma_transport.h"
#include "../../tunnel/comch_api.h"

#define DEFAULT_SERVICE  "e2e-latency"
#define DEFAULT_PORT     7888
#define DEFAULT_MSG_SIZE 4096
#define RECV_DEPTH       128

static volatile int g_running = 1;
static void sig_handler(int sig) { (void)sig; g_running = 0; }

typedef enum { NIC_CLIENT, NIC_SERVER } nic_mode_t;

static int run_client(const char* dev_pci, const char* rep_pci,
                       const char* peer_ip, uint16_t port,
                       uint32_t msg_size, const char* service)
{
    comch_nic_ctx_t* cctx = NULL;
    rdma_endpoint_t* rep = NULL;

    fprintf(stderr, "[nic-client] starting Comch (listening for host)...\n");
    if (comch_nic_init(&cctx, dev_pci, rep_pci, service) != DOCA_SUCCESS) {
        fprintf(stderr, "comch_nic_init failed\n"); return 1;
    }

    fprintf(stderr, "[nic-client] connecting RDMA to %s:%u ...\n", peer_ip, port);
    rep = rdma_endpoint_create_client(peer_ip, port, msg_size);
    if (!rep) {
        fprintf(stderr, "rdma_endpoint_create_client failed\n");
        comch_nic_destroy(cctx);
        return 1;
    }
    fprintf(stderr, "[nic-client] ready. Forwarding Comch<->RDMA...\n");

    uint8_t* buf = malloc(msg_size);
    if (!buf) { fprintf(stderr, "malloc failed\n"); return 1; }

    uint64_t forwarded = 0;
    while (g_running) {
        size_t len = msg_size;
        doca_error_t crc = comch_nic_recv_blocking(cctx, buf, &len, 1000);
        if (crc != DOCA_SUCCESS) continue;

        if (rdma_endpoint_send(rep, buf, (uint32_t)len) != 0) {
            fprintf(stderr, "[nic-client] rdma send err: %s\n",
                    rdma_endpoint_last_error(rep));
            break;
        }

        uint32_t rlen = 0;
        int rc = rdma_endpoint_recv(rep, buf, msg_size, &rlen, 2000000);
        if (rc != 0) {
            fprintf(stderr, "[nic-client] rdma recv err: %s\n",
                    rdma_endpoint_last_error(rep));
            continue;
        }

        (void)comch_nic_send(cctx, buf, rlen);
        forwarded++;
        if ((forwarded & 0xFFF) == 0) {
            fprintf(stderr, "[nic-client] forwarded %" PRIu64 "\r", forwarded);
        }
    }
    fprintf(stderr, "\n[nic-client] exiting (forwarded %" PRIu64 ")\n", forwarded);

    free(buf);
    rdma_endpoint_destroy(rep);
    comch_nic_destroy(cctx);
    return 0;
}

static int run_server(const char* dev_pci, const char* rep_pci,
                       const char* bind_ip, uint16_t port,
                       uint32_t msg_size, const char* service)
{
    comch_nic_ctx_t* cctx = NULL;
    rdma_endpoint_t* rep = NULL;

    fprintf(stderr, "[nic-server] starting RDMA server on %s:%u ...\n",
            bind_ip ? bind_ip : "*", port);
    rep = rdma_endpoint_create_server(bind_ip, port, msg_size, RECV_DEPTH);
    if (!rep) {
        fprintf(stderr, "rdma_endpoint_create_server failed\n"); return 1;
    }
    fprintf(stderr, "[nic-server] RDMA connection established. Starting Comch...\n");

    if (comch_nic_init(&cctx, dev_pci, rep_pci, service) != DOCA_SUCCESS) {
        fprintf(stderr, "comch_nic_init failed\n");
        rdma_endpoint_destroy(rep);
        return 1;
    }
    fprintf(stderr, "[nic-server] ready. Forwarding RDMA<->Comch...\n");

    uint8_t* buf = malloc(msg_size);
    if (!buf) { fprintf(stderr, "malloc failed\n"); return 1; }

    uint64_t forwarded = 0;
    while (g_running) {
        uint32_t rlen = 0;
        int rc = rdma_endpoint_recv(rep, buf, msg_size, &rlen, 2000000);
        if (rc != 0) continue;

        if (comch_nic_send(cctx, buf, rlen) != DOCA_SUCCESS) {
            fprintf(stderr, "[nic-server] comch send fail\n"); continue;
        }

        size_t clen = msg_size;
        if (comch_nic_recv_blocking(cctx, buf, &clen, 2000) != DOCA_SUCCESS) {
            fprintf(stderr, "[nic-server] comch recv timeout\n"); continue;
        }

        if (rdma_endpoint_send(rep, buf, (uint32_t)clen) != 0) {
            fprintf(stderr, "[nic-server] rdma send fail\n"); continue;
        }
        forwarded++;
        if ((forwarded & 0xFFF) == 0) {
            fprintf(stderr, "[nic-server] forwarded %" PRIu64 "\r", forwarded);
        }
    }
    fprintf(stderr, "\n[nic-server] exiting (forwarded %" PRIu64 ")\n", forwarded);

    free(buf);
    comch_nic_destroy(cctx);
    rdma_endpoint_destroy(rep);
    return 0;
}

int main(int argc, char** argv)
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    nic_mode_t mode = NIC_CLIENT;
    const char* dev_pci = "03:00.0";
    const char* rep_pci = "auto";
    const char* peer_ip = NULL;
    const char* bind_ip = NULL;
    const char* service = DEFAULT_SERVICE;
    uint16_t port = DEFAULT_PORT;
    uint32_t msg_size = DEFAULT_MSG_SIZE;

    static struct option longopts[] = {
        {"mode",     required_argument, 0, 'm'},
        {"dev-pci",  required_argument, 0, 'd'},
        {"rep-pci",  required_argument, 0, 'r'},
        {"peer-ip",  required_argument, 0, 'P'},
        {"bind-ip",  required_argument, 0, 'B'},
        {"port",     required_argument, 0, 'p'},
        {"msg-size", required_argument, 0, 'z'},
        {"service",  required_argument, 0, 's'},
        {"help",     no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "m:d:r:P:B:p:z:s:h",
                               longopts, NULL)) != -1) {
        switch (opt) {
        case 'm':
            if (strcmp(optarg, "client") == 0) mode = NIC_CLIENT;
            else if (strcmp(optarg, "server") == 0) mode = NIC_SERVER;
            else { fprintf(stderr, "unknown mode\n"); return 1; }
            break;
        case 'd': dev_pci = optarg; break;
        case 'r': rep_pci = optarg; break;
        case 'P': peer_ip = optarg; break;
        case 'B': bind_ip = optarg; break;
        case 'p': port = (uint16_t)atoi(optarg); break;
        case 'z': msg_size = (uint32_t)atoi(optarg); break;
        case 's': service = optarg; break;
        case 'h':
            fprintf(stderr,
                "Usage: %s --mode=client --peer-ip=IP [options]\n"
                "       %s --mode=server --bind-ip=IP [options]\n",
                argv[0], argv[0]);
            return 0;
        }
    }

    if (mode == NIC_CLIENT && !peer_ip) {
        fprintf(stderr, "client mode requires --peer-ip\n"); return 1;
    }

    return mode == NIC_CLIENT ?
        run_client(dev_pci, rep_pci, peer_ip, port, msg_size, service) :
        run_server(dev_pci, rep_pci, bind_ip, port, msg_size, service);
}
