/*
 * node_state.h — Node lifecycle state machine and domain health types.
 *
 * Shared between cluster_master (C++) and slave_agent (C++).
 * C-compatible: guarded with extern "C" for inclusion from C code.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Node lifecycle states                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    NODE_STATE_UNKNOWN  = 0,
    NODE_STATE_ONLINE   = 1,   /* Heartbeats arriving normally          */
    NODE_STATE_SUSPECT  = 2,   /* Heartbeat missed, within grace period */
    NODE_STATE_OFFLINE  = 3,   /* Confirmed down                        */
} node_state_t;

/* ------------------------------------------------------------------ */
/* Per-domain health status                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    DOMAIN_UNKNOWN     = 0,
    DOMAIN_OK          = 1,
    DOMAIN_DEGRADED    = 2,    /* High temperature, resource pressure   */
    DOMAIN_UNREACHABLE = 3,    /* Comch timeout or PCIe link failure    */
} domain_status_t;

/* ------------------------------------------------------------------ */
/* Timing thresholds (nanoseconds)                                     */
/* ------------------------------------------------------------------ */

/* Heartbeat interval: 3 seconds (configurable at runtime) */
#define HB_INTERVAL_NS         (3ULL * 1000000000ULL)

/* Node transitions to SUSPECT after 5 missed heartbeats (15s) */
#define SUSPECT_THRESHOLD_NS   (15ULL * 1000000000ULL)

/* Node transitions to OFFLINE after 3x suspect threshold (45s) */
#define OFFLINE_THRESHOLD_NS   (45ULL * 1000000000ULL)

/* Master health check period for watchdog */
#define WATCHDOG_CHECK_NS      (3ULL * 1000000000ULL)

/* Comch receive timeout before marking host as unreachable */
#define COMCH_TIMEOUT_NS       (10ULL * 1000000000ULL)

/* ------------------------------------------------------------------ */
/* State transition function                                           */
/* ------------------------------------------------------------------ */

/*
 * Compute the current node state based on the time since last heartbeat.
 * Returns the new state (may be the same as current).
 */
static inline node_state_t
compute_node_state(uint64_t now_ns, uint64_t last_heartbeat_ns)
{
    if (last_heartbeat_ns == 0)
        return NODE_STATE_UNKNOWN;

    uint64_t elapsed = now_ns - last_heartbeat_ns;

    if (elapsed <= SUSPECT_THRESHOLD_NS)
        return NODE_STATE_ONLINE;
    else if (elapsed <= OFFLINE_THRESHOLD_NS)
        return NODE_STATE_SUSPECT;
    else
        return NODE_STATE_OFFLINE;
}

/* ------------------------------------------------------------------ */
/* String helpers                                                      */
/* ------------------------------------------------------------------ */

static inline const char *node_state_str(node_state_t s)
{
    switch (s) {
    case NODE_STATE_UNKNOWN:  return "unknown";
    case NODE_STATE_ONLINE:   return "online";
    case NODE_STATE_SUSPECT:  return "suspect";
    case NODE_STATE_OFFLINE:  return "offline";
    default:                  return "?";
    }
}

static inline const char *domain_status_str(domain_status_t s)
{
    switch (s) {
    case DOMAIN_UNKNOWN:     return "unknown";
    case DOMAIN_OK:          return "ok";
    case DOMAIN_DEGRADED:    return "degraded";
    case DOMAIN_UNREACHABLE: return "unreachable";
    default:                 return "?";
    }
}

#ifdef __cplusplus
}
#endif
