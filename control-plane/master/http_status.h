/*
 * http_status.h — Lightweight HTTP JSON status server.
 *
 * Serves GET / with the current cluster state from NodeRegistry as JSON.
 * Runs on a background thread; no external framework dependencies.
 */

#pragma once

#include <cstdint>
#include "node_registry.h"

/*
 * Start the HTTP status server on the given port.
 * Spawns a background thread that accepts connections.
 * Returns 0 on success, -1 on failure.
 */
int http_status_start(uint16_t port, NodeRegistry& registry);

/*
 * Stop the HTTP status server and join the background thread.
 */
void http_status_stop();
