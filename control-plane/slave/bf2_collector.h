/*
 * bf2_collector.h — BF2 SmartNIC local metrics collector.
 *
 * Collects ARM CPU, memory, temperature, network port stats, and OVS
 * flow counts directly on the BF2 ARM SoC.  Used by slave_agent.
 */

#pragma once

#include <cstdint>

class BF2Collector {
public:
    struct BF2Metrics {
        float    arm_cpu_pct;       // ARM CPU utilisation (0-100%)
        uint64_t arm_mem_total_kb;
        uint64_t arm_mem_avail_kb;
        float    temperature_c;     // SoC junction temperature
        uint64_t port_rx_bytes;     // Physical port p0 counters
        uint64_t port_tx_bytes;
        uint64_t port_rx_drops;
        uint32_t ovs_flow_count;    // OVS datapath flow entries
        uint64_t timestamp_ns;      // CLOCK_REALTIME nanoseconds
    };

    /**
     * init — Take a baseline CPU reading so the first collect() delta
     *        is meaningful.  Call once before entering the reporting loop.
     */
    void init();

    /**
     * collect — Read all metrics and return a snapshot.
     *           Safe to call from a single thread at any cadence.
     */
    BF2Metrics collect();

    struct CpuStat {
        uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    };

private:

    CpuStat prev_cpu_{};

    float    readCpuUsage();
    void     readMem(uint64_t& total, uint64_t& avail);
    float    readTemperature();
    void     readPortStats(uint64_t& rx, uint64_t& tx, uint64_t& drops);
    uint32_t readOvsFlowCount();
};
