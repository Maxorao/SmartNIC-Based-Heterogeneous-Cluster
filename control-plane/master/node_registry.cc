/*
 * node_registry.cc — Thread-safe in-memory node registry implementation.
 */

#include "node_registry.h"

#include <cinttypes>
#include <cstdio>
#include <sstream>

extern "C" {
#include "../../common/timing.h"
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

NodeEntry* NodeRegistry::registerNode(const std::string& uuid,
                                       const std::string& hostname,
                                       const std::string& host_hostname,
                                       const std::string& ip_addr,
                                       const std::string& pci_bus_id,
                                       uint64_t now_ns)
{
    std::lock_guard<std::mutex> lk(mu_);

    auto [it, inserted] = nodes_.try_emplace(uuid);
    NodeEntry& e = it->second;

    if (inserted) {
        e.node_uuid       = uuid;
        e.registered_at_ns = now_ns;
    }

    e.hostname       = hostname;
    e.host_hostname  = host_hostname;
    e.ip_addr        = ip_addr;
    e.pci_bus_id     = pci_bus_id;
    e.last_heartbeat_ns = now_ns;
    e.state          = NODE_STATE_ONLINE;
    e.host_status    = DOMAIN_OK;
    e.bf2_status     = DOMAIN_OK;
    e.msg_count      = 0;

    return &e;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

NodeEntry* NodeRegistry::findNode(const std::string& uuid)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = nodes_.find(uuid);
    return (it != nodes_.end()) ? &it->second : nullptr;
}

/* ------------------------------------------------------------------ */
/* Heartbeat update                                                    */
/* ------------------------------------------------------------------ */

void NodeRegistry::updateHeartbeat(const std::string& uuid,
                                    domain_status_t host_st,
                                    domain_status_t bf2_st,
                                    uint64_t now_ns)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = nodes_.find(uuid);
    if (it == nodes_.end()) return;

    NodeEntry& e = it->second;
    e.last_heartbeat_ns = now_ns;
    e.host_status       = host_st;
    e.bf2_status        = bf2_st;
    e.state             = NODE_STATE_ONLINE;
    e.msg_count++;
}

/* ------------------------------------------------------------------ */
/* State machine sweep                                                 */
/* ------------------------------------------------------------------ */

void NodeRegistry::runStateTransitions(uint64_t now_ns, StateTransitionCB cb)
{
    // Collect transitions under the lock, then fire callbacks outside
    // to avoid holding the mutex during potentially slow DB writes.
    std::vector<std::tuple<std::string, node_state_t, node_state_t>> changes;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [uuid, entry] : nodes_) {
            node_state_t new_state = compute_node_state(now_ns, entry.last_heartbeat_ns);
            if (new_state != entry.state) {
                changes.emplace_back(uuid, entry.state, new_state);
                entry.state = new_state;
            }
        }
    }
    // Fire callbacks without the lock held
    if (cb) {
        for (auto& [uuid, old_s, new_s] : changes) {
            cb(uuid, old_s, new_s);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Removal                                                             */
/* ------------------------------------------------------------------ */

bool NodeRegistry::removeNode(const std::string& uuid)
{
    std::lock_guard<std::mutex> lk(mu_);
    return nodes_.erase(uuid) > 0;
}

/* ------------------------------------------------------------------ */
/* JSON serialisation                                                  */
/* ------------------------------------------------------------------ */

std::string NodeRegistry::toJSON() const
{
    std::lock_guard<std::mutex> lk(mu_);

    Summary s{};
    for (auto& [_, e] : nodes_) {
        s.total++;
        switch (e.state) {
        case NODE_STATE_ONLINE:  s.online++;  break;
        case NODE_STATE_SUSPECT: s.suspect++; break;
        case NODE_STATE_OFFLINE: s.offline++; break;
        default: break;
        }
    }

    std::ostringstream os;
    os << "{\n";
    os << "  \"summary\": {"
       << " \"total\": "   << s.total
       << ", \"online\": " << s.online
       << ", \"suspect\": " << s.suspect
       << ", \"offline\": " << s.offline
       << " },\n";
    os << "  \"nodes\": [\n";

    bool first = true;
    for (auto& [uuid, e] : nodes_) {
        if (!first) os << ",\n";
        first = false;

        os << "    { "
           << "\"uuid\": \""          << e.node_uuid << "\""
           << ", \"hostname\": \""    << e.hostname << "\""
           << ", \"host_hostname\": \"" << e.host_hostname << "\""
           << ", \"ip\": \""          << e.ip_addr << "\""
           << ", \"pci_bus_id\": \""  << e.pci_bus_id << "\""
           << ", \"state\": \""       << node_state_str(e.state) << "\""
           << ", \"host_status\": \"" << domain_status_str(e.host_status) << "\""
           << ", \"bf2_status\": \""  << domain_status_str(e.bf2_status) << "\""
           << ", \"last_heartbeat_ns\": " << e.last_heartbeat_ns
           << ", \"registered_at_ns\": "  << e.registered_at_ns
           << ", \"msg_count\": "         << e.msg_count
           << " }";
    }

    os << "\n  ]\n}\n";
    return os.str();
}

/* ------------------------------------------------------------------ */
/* Summary                                                             */
/* ------------------------------------------------------------------ */

NodeRegistry::Summary NodeRegistry::getSummary() const
{
    std::lock_guard<std::mutex> lk(mu_);

    Summary s{};
    for (auto& [_, e] : nodes_) {
        s.total++;
        switch (e.state) {
        case NODE_STATE_ONLINE:  s.online++;  break;
        case NODE_STATE_SUSPECT: s.suspect++; break;
        case NODE_STATE_OFFLINE: s.offline++; break;
        default: break;
        }
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                             */
/* ------------------------------------------------------------------ */

std::vector<NodeEntry> NodeRegistry::snapshot() const
{
    std::lock_guard<std::mutex> lk(mu_);

    std::vector<NodeEntry> out;
    out.reserve(nodes_.size());
    for (auto& [_, e] : nodes_) {
        out.push_back(e);
    }
    return out;
}
