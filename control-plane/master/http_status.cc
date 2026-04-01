/*
 * http_status.cc — Lightweight HTTP JSON status server using raw sockets.
 *
 * Listens on a TCP port and responds to any GET request with JSON from
 * NodeRegistry::toJSON().  Non-GET requests receive 405.  The server
 * runs on a single background thread with a simple accept loop.
 */

#include "http_status.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static std::atomic<bool> g_running{false};
static std::thread       g_thread;
static int               g_listen_fd = -1;

/* ------------------------------------------------------------------ */
/* Internal: handle one client connection                              */
/* ------------------------------------------------------------------ */

static void handle_client(int client_fd, NodeRegistry& registry)
{
    /* Read the request (we only need the first line to decide) */
    char buf[2048];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = '\0';

    /* Check for GET method */
    bool is_get = (strncmp(buf, "GET ", 4) == 0);

    if (is_get) {
        std::string body = registry.toJSON();

        char header[512];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n",
            body.size());

        send(client_fd, header, (size_t)hlen, 0);
        send(client_fd, body.data(), body.size(), 0);
    } else {
        const char *resp =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        send(client_fd, resp, strlen(resp), 0);
    }

    close(client_fd);
}

/* ------------------------------------------------------------------ */
/* Internal: accept loop                                               */
/* ------------------------------------------------------------------ */

static void accept_loop(NodeRegistry& registry)
{
    while (g_running.load()) {
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(g_listen_fd,
                               reinterpret_cast<struct sockaddr*>(&client_addr),
                               &addr_len);
        if (client_fd < 0) {
            if (g_running.load()) {
                perror("[http] accept");
            }
            continue;
        }

        /* Set a short recv timeout so we don't hang on slow clients */
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_client(client_fd, registry);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int http_status_start(uint16_t port, NodeRegistry& registry)
{
    if (g_running.load()) {
        fprintf(stderr, "[http] server already running\n");
        return -1;
    }

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("[http] socket");
        return -1;
    }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(g_listen_fd, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        perror("[http] bind");
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    if (listen(g_listen_fd, 16) < 0) {
        perror("[http] listen");
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    g_running.store(true);
    g_thread = std::thread(accept_loop, std::ref(registry));

    fprintf(stderr, "[http] status server listening on port %u\n",
            (unsigned)port);
    return 0;
}

void http_status_stop()
{
    if (!g_running.load()) return;

    g_running.store(false);

    /* Close the listen socket to unblock accept() */
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    if (g_thread.joinable()) {
        g_thread.join();
    }

    fprintf(stderr, "[http] status server stopped\n");
}
