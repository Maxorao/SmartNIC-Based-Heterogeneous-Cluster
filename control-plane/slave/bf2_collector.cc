/*
 * bf2_collector.cc — BF2 SmartNIC local metrics collector implementation.
 *
 * All data comes from procfs / sysfs / popen; no DOCA dependency.
 */

#include "bf2_collector.h"

#include <cstdio>
#include <cstring>
#include <cinttypes>

extern "C" {
#include "../../common/timing.h"
}

/* ------------------------------------------------------------------ */
/* CPU usage from /proc/stat                                           */
/* ------------------------------------------------------------------ */

static int read_cpu_stat(BF2Collector::CpuStat* s)
{
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return -1;
    // NOLINTNEXTLINE — fscanf is fine for /proc/stat
    int rc = fscanf(f, "cpu  %" SCNu64 " %" SCNu64 " %" SCNu64
                       " %" SCNu64 " %" SCNu64 " %" SCNu64
                       " %" SCNu64 " %" SCNu64,
                    &s->user, &s->nice, &s->system, &s->idle,
                    &s->iowait, &s->irq, &s->softirq, &s->steal);
    fclose(f);
    return (rc == 8) ? 0 : -1;
}

float BF2Collector::readCpuUsage()
{
    CpuStat cur{};
    if (read_cpu_stat(&cur) != 0)
        return 0.0f;

    uint64_t prev_idle  = prev_cpu_.idle + prev_cpu_.iowait;
    uint64_t cur_idle   = cur.idle  + cur.iowait;
    uint64_t prev_total = prev_cpu_.user + prev_cpu_.nice + prev_cpu_.system
                        + prev_idle + prev_cpu_.irq + prev_cpu_.softirq
                        + prev_cpu_.steal;
    uint64_t cur_total  = cur.user + cur.nice + cur.system + cur_idle
                        + cur.irq + cur.softirq + cur.steal;

    uint64_t total_diff = cur_total - prev_total;
    uint64_t idle_diff  = cur_idle  - prev_idle;

    prev_cpu_ = cur;

    if (total_diff == 0) return 0.0f;
    return static_cast<float>(total_diff - idle_diff) * 100.0f
           / static_cast<float>(total_diff);
}

/* ------------------------------------------------------------------ */
/* Memory from /proc/meminfo                                           */
/* ------------------------------------------------------------------ */

void BF2Collector::readMem(uint64_t& total, uint64_t& avail)
{
    total = 0;
    avail = 0;
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return;

    char key[64];
    uint64_t val;
    char unit[8];
    while (fscanf(f, "%63s %" SCNu64 " %7s", key, &val, unit) >= 2) {
        if (strcmp(key, "MemTotal:") == 0)     total = val;
        if (strcmp(key, "MemAvailable:") == 0) avail = val;
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Temperature from sysfs thermal zone                                 */
/* ------------------------------------------------------------------ */

float BF2Collector::readTemperature()
{
    FILE* f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return 0.0f;

    int millideg = 0;
    if (fscanf(f, "%d", &millideg) != 1)
        millideg = 0;
    fclose(f);

    return static_cast<float>(millideg) / 1000.0f;
}

/* ------------------------------------------------------------------ */
/* Port statistics from sysfs (BF2 physical port p0)                   */
/* ------------------------------------------------------------------ */

static uint64_t read_sysfs_u64(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    uint64_t val = 0;
    if (fscanf(f, "%" SCNu64, &val) != 1)
        val = 0;
    fclose(f);
    return val;
}

void BF2Collector::readPortStats(uint64_t& rx, uint64_t& tx, uint64_t& drops)
{
    rx    = read_sysfs_u64("/sys/class/net/p0/statistics/rx_bytes");
    tx    = read_sysfs_u64("/sys/class/net/p0/statistics/tx_bytes");
    drops = read_sysfs_u64("/sys/class/net/p0/statistics/rx_dropped");
}

/* ------------------------------------------------------------------ */
/* OVS datapath flow count                                             */
/* ------------------------------------------------------------------ */

uint32_t BF2Collector::readOvsFlowCount()
{
    FILE* pipe = popen("ovs-dpctl dump-flows 2>/dev/null | wc -l", "r");
    if (!pipe) return 0;

    uint32_t count = 0;
    if (fscanf(pipe, "%u", &count) != 1)
        count = 0;
    pclose(pipe);
    return count;
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

void BF2Collector::init()
{
    // Take a baseline CPU reading so the first delta is meaningful.
    read_cpu_stat(&prev_cpu_);
}

BF2Collector::BF2Metrics BF2Collector::collect()
{
    BF2Metrics m{};

    m.arm_cpu_pct = readCpuUsage();
    readMem(m.arm_mem_total_kb, m.arm_mem_avail_kb);
    m.temperature_c = readTemperature();
    readPortStats(m.port_rx_bytes, m.port_tx_bytes, m.port_rx_drops);
    m.ovs_flow_count = readOvsFlowCount();
    m.timestamp_ns = unix_ns();

    return m;
}
