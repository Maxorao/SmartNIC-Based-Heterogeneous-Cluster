/*
 * master_monitor.c — Cluster master node for the control-plane
 *
 * Runs on gnode1 (Ubuntu 22.04, x86).
 * Listens for TCP connections from:
 *   - forward_routine (offload mode) running on each SmartNIC
 *   - slave_monitor (direct mode) running on each host (baseline experiments)
 *
 * Features:
 *   - pthread-per-connection for concurrent clients
 *   - Stores all resource reports to TimescaleDB via db.h
 *   - HTTP status endpoint on port 8080
 *   - Node timeout detection (5x report interval)
 *   - 30-second summary to stderr
 *
 * Usage:
 *   master_monitor [--port=PORT] [--db-connstr=CONNSTR] [--http-port=PORT]
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
#include "db.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

#define DEFAULT_PORT      9000
#define DEFAULT_HTTP_PORT 8080
#define MAX_NODES         256
#define NODE_TIMEOUT_NS   (5ULL * 5 * 1000000000ULL)  /* 5 × 5s = 25s */
#define SUMMARY_INTERVAL  30                           /* seconds */

static uint16_t  g_port      = DEFAULT_PORT;
static uint16_t  g_http_port = DEFAULT_HTTP_PORT;
static char      g_connstr[512] =
    "host=localhost dbname=cluster_metrics user=cluster password=cluster";

static volatile int g_running = 1;

/* ------------------------------------------------------------------ */
/* Node registry                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char     node_id[64];
    char     ip_addr[INET_ADDRSTRLEN];
    int      online;
    uint64_t last_seen_ns;
    uint64_t msg_count;
} node_entry_t;

static node_entry_t  g_nodes[MAX_NODES];
static uint32_t      g_node_count = 0;
static pthread_mutex_t g_nodes_lock = PTHREAD_MUTEX_INITIALIZER;

/* Global statistics */
static uint64_t g_total_msgs   = 0;
static uint64_t g_total_writes = 0;
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Database context (single shared connection)                          */
/* ------------------------------------------------------------------ */

static db_ctx_t *g_db = NULL;
static pthread_mutex_t g_db_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */

static void master_log(const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    fprintf(stderr, "[%s.%03ld] master: ", tbuf, ts.tv_nsec / 1000000L);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* Node registry helpers                                                */
/* ------------------------------------------------------------------ */

static node_entry_t *find_or_create_node(const char *node_id,
                                          const char *ip_addr)
{
    pthread_mutex_lock(&g_nodes_lock);
    for (uint32_t i = 0; i < g_node_count; i++) {
        if (strcmp(g_nodes[i].node_id, node_id) == 0) {
            pthread_mutex_unlock(&g_nodes_lock);
            return &g_nodes[i];
        }
    }
    if (g_node_count >= MAX_NODES) {
        pthread_mutex_unlock(&g_nodes_lock);
        return NULL;
    }
    node_entry_t *e = &g_nodes[g_node_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->node_id, node_id, sizeof(e->node_id) - 1);
    if (ip_addr) strncpy(e->ip_addr, ip_addr, sizeof(e->ip_addr) - 1);
    e->online = 1;
    e->last_seen_ns = now_ns();
    pthread_mutex_unlock(&g_nodes_lock);
    return e;
}

/* ------------------------------------------------------------------ */
/* Per-connection handler thread                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int   fd;
    char  peer_ip[INET_ADDRSTRLEN];
} conn_arg_t;

static void *handle_connection(void *arg)
{
    conn_arg_t *ca = (conn_arg_t *)arg;
    int fd = ca->fd;
    char peer_ip[INET_ADDRSTRLEN];
    strncpy(peer_ip, ca->peer_ip, sizeof(peer_ip));
    free(ca);

    master_log("connection from %s", peer_ip);

    char node_id[64] = "<unknown>";
    node_entry_t *node = NULL;

    char payload_buf[PROTO_MAX_PAYLOAD];
    msg_header_t hdr;

    while (1) {
        uint64_t t0 = now_ns();

        if (proto_tcp_recv(fd, &hdr, payload_buf, sizeof(payload_buf)) < 0)
            break;   /* client disconnected or protocol error */

        uint64_t latency_us = (now_ns() - t0) / 1000;

        pthread_mutex_lock(&g_stats_lock);
        g_total_msgs++;
        pthread_mutex_unlock(&g_stats_lock);

        if (node) {
            node->last_seen_ns = now_ns();
            node->msg_count++;
        }

        switch ((msg_type_t)hdr.type) {

        case MSG_REGISTER: {
            if (hdr.payload_len < sizeof(register_payload_t)) break;
            register_payload_t *reg = (register_payload_t *)payload_buf;
            strncpy(node_id, reg->node_id, sizeof(node_id) - 1);
            node = find_or_create_node(node_id, peer_ip);
            master_log("REGISTER: node_id=%s ip=%s", node_id, peer_ip);

            /* Persist to DB */
            pthread_mutex_lock(&g_db_lock);
            if (g_db) db_register_node(g_db, reg, peer_ip);
            pthread_mutex_unlock(&g_db_lock);

            /* Send ACK */
            proto_tcp_send(fd, MSG_REGISTER_ACK, hdr.seq, NULL, 0);
            break;
        }

        case MSG_RESOURCE_REPORT: {
            if (hdr.payload_len < sizeof(resource_report_t)) break;
            resource_report_t *r = (resource_report_t *)payload_buf;

            /* Use node_id from report if not yet registered */
            if (node_id[0] == '<')
                strncpy(node_id, r->node_id, sizeof(node_id) - 1);
            if (!node)
                node = find_or_create_node(node_id, peer_ip);

            pthread_mutex_lock(&g_db_lock);
            int ok = 0;
            if (g_db) ok = (db_insert_resource(g_db, r) == 0);
            pthread_mutex_unlock(&g_db_lock);

            if (ok) {
                pthread_mutex_lock(&g_stats_lock);
                g_total_writes++;
                pthread_mutex_unlock(&g_stats_lock);
            }

            /* ACK the resource report so mock_slave can measure latency */
            proto_tcp_send(fd, MSG_HEARTBEAT_ACK, hdr.seq, NULL, 0);
            break;
        }

        case MSG_HEARTBEAT:
            if (node) node->last_seen_ns = now_ns();
            proto_tcp_send(fd, MSG_HEARTBEAT_ACK, hdr.seq, NULL, 0);
            break;

        case MSG_DEREGISTER:
            master_log("DEREGISTER: node=%s", node_id);
            if (node) node->online = 0;
            pthread_mutex_lock(&g_db_lock);
            if (g_db) db_update_node_status(g_db, node_id, 0);
            pthread_mutex_unlock(&g_db_lock);
            goto done;

        default:
            master_log("unknown msg type %u from %s (lat=%lu us)",
                       hdr.type, node_id, (unsigned long)latency_us);
            break;
        }
    }

done:
    if (node) {
        node->online = 0;
        pthread_mutex_lock(&g_db_lock);
        if (g_db) db_update_node_status(g_db, node_id, 0);
        pthread_mutex_unlock(&g_db_lock);
    }
    master_log("connection closed: %s (%s)", node_id, peer_ip);
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Node timeout watchdog thread                                         */
/* ------------------------------------------------------------------ */

static void *watchdog_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        sleep(5);
        uint64_t now = now_ns();
        pthread_mutex_lock(&g_nodes_lock);
        for (uint32_t i = 0; i < g_node_count; i++) {
            node_entry_t *e = &g_nodes[i];
            if (e->online &&
                (now - e->last_seen_ns) > NODE_TIMEOUT_NS) {
                master_log("TIMEOUT: node %s last seen %lu ms ago — marking offline",
                           e->node_id,
                           (unsigned long)((now - e->last_seen_ns) / 1000000));
                e->online = 0;
                pthread_mutex_lock(&g_db_lock);
                if (g_db) db_update_node_status(g_db, e->node_id, 0);
                pthread_mutex_unlock(&g_db_lock);
            }
        }
        pthread_mutex_unlock(&g_nodes_lock);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Summary thread                                                       */
/* ------------------------------------------------------------------ */

static void *summary_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        sleep(SUMMARY_INTERVAL);
        pthread_mutex_lock(&g_nodes_lock);
        int online = 0;
        for (uint32_t i = 0; i < g_node_count; i++)
            if (g_nodes[i].online) online++;
        uint32_t total = g_node_count;
        pthread_mutex_unlock(&g_nodes_lock);

        pthread_mutex_lock(&g_stats_lock);
        uint64_t msgs   = g_total_msgs;
        uint64_t writes = g_total_writes;
        pthread_mutex_unlock(&g_stats_lock);

        master_log("SUMMARY: nodes=%u/%u online  total_msgs=%" PRIu64
                   "  db_writes=%" PRIu64,
                   online, total, msgs, writes);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Minimal HTTP status server                                           */
/* ------------------------------------------------------------------ */

static void *http_thread(void *arg)
{
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("http socket"); return NULL; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(g_http_port),
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("http bind"); close(srv); return NULL;
    }
    listen(srv, 8);
    master_log("HTTP status on :%u", g_http_port);

    while (g_running) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) continue;

        /* Drain the HTTP request (don't care about content) */
        char rbuf[512];
        recv(cfd, rbuf, sizeof(rbuf), 0);

        /* Build JSON response */
        pthread_mutex_lock(&g_nodes_lock);
        int online = 0;
        char node_list[4096] = "[\n";
        for (uint32_t i = 0; i < g_node_count; i++) {
            if (g_nodes[i].online) online++;
            char entry[256];
            snprintf(entry, sizeof(entry),
                     "  {\"node_id\":\"%s\",\"ip\":\"%s\","
                     "\"online\":%s,\"msgs\":%" PRIu64 "}%s\n",
                     g_nodes[i].node_id,
                     g_nodes[i].ip_addr,
                     g_nodes[i].online ? "true" : "false",
                     g_nodes[i].msg_count,
                     (i + 1 < g_node_count) ? "," : "");
            strncat(node_list, entry,
                    sizeof(node_list) - strlen(node_list) - 1);
        }
        strncat(node_list, "]", sizeof(node_list) - strlen(node_list) - 1);
        uint32_t total = g_node_count;
        pthread_mutex_unlock(&g_nodes_lock);

        pthread_mutex_lock(&g_stats_lock);
        uint64_t msgs   = g_total_msgs;
        uint64_t writes = g_total_writes;
        pthread_mutex_unlock(&g_stats_lock);

        char body[8192];
        snprintf(body, sizeof(body),
                 "{\"nodes_online\":%d,\"nodes_total\":%u,"
                 "\"total_msgs\":%" PRIu64 ",\"db_writes\":%" PRIu64 ","
                 "\"nodes\":%s}",
                 online, total, msgs, writes, node_list);

        char resp[8192 + 256];
        snprintf(resp, sizeof(resp),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n%s",
                 strlen(body), body);
        send(cfd, resp, strlen(resp), MSG_NOSIGNAL);
        close(cfd);
    }
    close(srv);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                       */
/* ------------------------------------------------------------------ */

static void sigterm_handler(int sig) { (void)sig; g_running = 0; }

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"port",       required_argument, 0, 'p'},
        {"db-connstr", required_argument, 0, 'd'},
        {"http-port",  required_argument, 0, 'H'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
        case 'p': g_port = (uint16_t)atoi(optarg); break;
        case 'd': strncpy(g_connstr, optarg, sizeof(g_connstr) - 1); break;
        case 'H': g_http_port = (uint16_t)atoi(optarg); break;
        case 'h':
        default:
            fprintf(stderr,
                "Usage: %s [--port=PORT] [--db-connstr=CONNSTR] [--http-port=PORT]\n",
                argv[0]);
            exit(c == 'h' ? 0 : 1);
        }
    }

    /* Use sigaction without SA_RESTART so accept() returns EINTR on signal */
    struct sigaction sa = { .sa_handler = sigterm_handler, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    master_log("starting on port %u (HTTP :%u)", g_port, g_http_port);

    /* Connect to database */
    g_db = db_connect(g_connstr);
    if (!g_db) {
        master_log("WARNING: DB unavailable — metrics will not be persisted");
    } else {
        if (db_init_schema(g_db) < 0) {
            master_log("WARNING: schema init failed");
        }
    }

    /* Start background threads */
    pthread_t wdog_tid, summ_tid, http_tid;
    pthread_create(&wdog_tid, NULL, watchdog_thread, NULL);
    pthread_create(&summ_tid, NULL, summary_thread,  NULL);
    pthread_create(&http_tid, NULL, http_thread,     NULL);
    pthread_detach(wdog_tid);
    pthread_detach(summ_tid);
    pthread_detach(http_tid);

    /* Listener socket */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(g_port),
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(srv, 64);
    master_log("listening on :%u", g_port);

    while (g_running) {
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        int cfd = accept(srv, (struct sockaddr *)&peer_addr, &peer_len);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }

        conn_arg_t *ca = malloc(sizeof(conn_arg_t));
        if (!ca) { close(cfd); continue; }
        ca->fd = cfd;
        inet_ntop(AF_INET, &peer_addr.sin_addr, ca->peer_ip, sizeof(ca->peer_ip));

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_connection, ca) != 0) {
            free(ca); close(cfd);
        } else {
            pthread_detach(tid);
        }
    }

    close(srv);
    if (g_db) db_disconnect(g_db);
    master_log("exiting");
    return 0;
}
