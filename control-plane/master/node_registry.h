/*
 * node_registry.h — Thread-safe in-memory registry of cluster nodes.
 *
 * Maintains per-node state (ONLINE/SUSPECT/OFFLINE) using the state
 * machine from common/node_state.h.  The registry is the single source
 * of truth for the master; the watchdog thread periodically calls
 * runStateTransitions() to drive lifecycle changes.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "../../common/node_state.h"
}

/* ------------------------------------------------------------------ */
/* Per-node entry                                                      */
/* ------------------------------------------------------------------ */

struct NodeEntry {
    std::string    node_uuid;
    std::string    hostname;
    std::string    host_hostname;
    std::string    ip_addr;
    std::string    pci_bus_id;

    node_state_t   state           = NODE_STATE_UNKNOWN;
    domain_status_t host_status    = DOMAIN_UNKNOWN;
    domain_status_t bf2_status     = DOMAIN_UNKNOWN;

    uint64_t       last_heartbeat_ns = 0;
    uint64_t       registered_at_ns  = 0;
    uint64_t       msg_count         = 0;
};

/* ------------------------------------------------------------------ */
/* Transition callback                                                 */
/* ------------------------------------------------------------------ */

/*
 * Called by runStateTransitions() whenever a node's state changes.
 *   uuid      — node identifier
 *   old_state — previous state
 *   new_state — state after transition
 */
using StateTransitionCB = std::function<void(const std::string& uuid,
                                              node_state_t old_state,
                                              node_state_t new_state)>;

/* ------------------------------------------------------------------ */
/* NodeRegistry                                                        */
/* ------------------------------------------------------------------ */

class NodeRegistry {
public:
    /* ---- Cluster summary ---- */
    struct Summary {
        uint32_t total   = 0;
        uint32_t online  = 0;
        uint32_t suspect = 0;
        uint32_t offline = 0;
    };

    /*
     * Register a new node or update an existing one.
     * Returns a pointer to the entry (valid as long as the registry lives
     * and the caller holds no lock — do not store persistently).
     */
    NodeEntry* registerNode(const std::string& uuid,
                            const std::string& hostname,
                            const std::string& host_hostname,
                            const std::string& ip_addr,
                            const std::string& pci_bus_id,
                            uint64_t now_ns);

    /*
     * Look up a node by UUID.  Returns nullptr if not found.
     */
    NodeEntry* findNode(const std::string& uuid);

    /*
     * Update heartbeat timestamp and per-domain status.
     */
    void updateHeartbeat(const std::string& uuid,
                         domain_status_t host_st,
                         domain_status_t bf2_st,
                         uint64_t now_ns);

    /*
     * Evaluate state transitions for every node.  Fires `cb` for each
     * node whose state changed.
     */
    void runStateTransitions(uint64_t now_ns, StateTransitionCB cb);

    /*
     * Remove a node from the registry (e.g. on explicit deregister).
     * Returns true if the node existed.
     */
    bool removeNode(const std::string& uuid);

    /*
     * Return a JSON representation of the full registry.
     * Suitable for the HTTP status endpoint.
     */
    std::string toJSON() const;

    /*
     * Aggregate summary of node states.
     */
    Summary getSummary() const;

    /*
     * Snapshot of all entries (copy) for safe iteration outside the lock.
     */
    std::vector<NodeEntry> snapshot() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, NodeEntry> nodes_;
};
