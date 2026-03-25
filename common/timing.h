// High-resolution timing utilities for latency measurement
// Uses CLOCK_MONOTONIC; suitable for both x86 and ARM (BlueField-3).

#pragma once
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* Returns current time in nanoseconds (CLOCK_MONOTONIC) */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Returns current unix time in nanoseconds (CLOCK_REALTIME) */
static inline uint64_t unix_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Sleep for the given number of milliseconds */
static inline void sleep_ms(uint32_t ms)
{
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

/* qsort comparator for uint64_t */
static inline int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Percentile computation on an array of uint64_t latency values (ns).
 * arr must be sorted ascending before calling. */
static inline uint64_t percentile(const uint64_t *sorted_arr, size_t n, double pct)
{
    if (n == 0) return 0;
    size_t idx = (size_t)(pct / 100.0 * (double)n);
    if (idx >= n) idx = n - 1;
    return sorted_arr[idx];
}

/* Print latency statistics to stdout.
 * samples: array of one-way latency in nanoseconds.
 * The array is copied internally; original is not modified. */
static inline void print_latency_stats(const char *label,
                                        uint64_t *samples, size_t n)
{
    if (n == 0) {
        printf("%-20s  n=0  (no data)\n", label);
        return;
    }

    uint64_t *sorted = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!sorted) { perror("malloc"); return; }
    memcpy(sorted, samples, n * sizeof(uint64_t));
    qsort(sorted, n, sizeof(uint64_t), cmp_u64);

    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += sorted[i];

    printf("%-20s  n=%zu  avg=%7.2f us  "
           "P50=%7.2f  P99=%7.2f  P99.9=%7.2f  max=%7.2f us\n",
           label, n,
           (double)sum / (double)n / 1000.0,
           (double)percentile(sorted, n, 50.0)   / 1000.0,
           (double)percentile(sorted, n, 99.0)   / 1000.0,
           (double)percentile(sorted, n, 99.9)   / 1000.0,
           (double)sorted[n-1] / 1000.0);
    free(sorted);
}

/* Export raw samples to CSV file: one latency value (ns) per line.
 * Returns 0 on success, -1 on error. */
static inline int export_csv(const char *path,
                              const uint64_t *samples, size_t n)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }
    fprintf(f, "latency_ns\n");
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%" PRIu64 "\n", samples[i]);
    fclose(f);
    return 0;
}

/* Compute mean of an array of uint64_t values */
static inline double mean_u64(const uint64_t *arr, size_t n)
{
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += (double)arr[i];
    return sum / (double)n;
}

/* Simple wall-clock rate limiter: busy-waits until at least interval_ns
 * have elapsed since *last_ns, then updates *last_ns. */
static inline void rate_limit(uint64_t *last_ns, uint64_t interval_ns)
{
    uint64_t now;
    while ((now = now_ns()) - *last_ns < interval_ns)
        ; /* busy wait — suitable for sub-ms intervals */
    *last_ns = now;
}
