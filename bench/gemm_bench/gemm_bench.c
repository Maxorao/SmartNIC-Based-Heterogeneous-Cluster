/*
 * gemm_bench.c — Experiment B: GEMM throughput benchmark
 *
 * Measures sustained double-precision GEMM performance using OpenBLAS.
 * Also tracks LLC (last-level cache) miss rate via perf_event_open.
 *
 * Purpose: quantify compute interference when the control-plane agent
 * (slave_monitor) runs on the same host CPU.
 *
 * Three scenarios driven by exp_B_interference.sh:
 *   Scenario 1: only this benchmark (baseline)
 *   Scenario 2: benchmark + slave_monitor --mode=direct (no offload)
 *   Scenario 3: benchmark + slave_monitor --mode=offload (SmartNIC path)
 *
 * Usage:
 *   gemm_bench [--size=N] [--duration=SECS] [--output=FILE.csv]
 *
 * Output (stdout): one GFLOPS value per second
 * Output (CSV):    second,gflops,llc_miss_rate
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <getopt.h>

/* OpenBLAS */
#include <cblas.h>

/* perf_event_open for LLC miss counter */
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

#include "../../common/timing.h"

/* ------------------------------------------------------------------ */
/* perf_event_open wrapper                                              */
/* ------------------------------------------------------------------ */

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                             int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

static int setup_llc_miss_counter(void)
{
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type           = PERF_TYPE_HW_CACHE;
    pe.size           = sizeof(pe);
    pe.config         = (PERF_COUNT_HW_CACHE_LL)
                      | (PERF_COUNT_HW_CACHE_OP_READ   << 8)
                      | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    pe.disabled       = 1;
    pe.exclude_kernel = 0;
    pe.exclude_hv     = 1;

    int fd = (int)perf_event_open(&pe, 0 /* this process */, -1 /* any cpu */,
                                   -1 /* no group */, 0);
    if (fd < 0) {
        fprintf(stderr, "perf_event_open (LLC-miss): %s — LLC stats disabled\n",
                strerror(errno));
        return -1;
    }
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    return fd;
}

static int setup_llc_load_counter(void)
{
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type           = PERF_TYPE_HW_CACHE;
    pe.size           = sizeof(pe);
    pe.config         = (PERF_COUNT_HW_CACHE_LL)
                      | (PERF_COUNT_HW_CACHE_OP_READ  << 8)
                      | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
    pe.disabled       = 1;
    pe.exclude_kernel = 0;
    pe.exclude_hv     = 1;

    int fd = (int)perf_event_open(&pe, 0, -1, -1, 0);
    if (fd < 0) {
        fprintf(stderr, "perf_event_open (LLC-load): %s\n", strerror(errno));
        return -1;
    }
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    return fd;
}

static uint64_t read_perf_counter(int fd)
{
    if (fd < 0) return 0;
    uint64_t val = 0;
    if (read(fd, &val, sizeof(val)) != sizeof(val))
        return 0;
    return val;
}

static void reset_perf_counter(int fd)
{
    if (fd < 0) return;
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
}

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

static int      g_size     = 1024;    /* N for N×N matrix */
static int      g_duration = 60;      /* seconds */
static char     g_output[256] = "";   /* CSV output path */

static volatile int g_stop = 0;

static void sig_handler(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"size",     required_argument, 0, 's'},
        {"duration", required_argument, 0, 'd'},
        {"output",   required_argument, 0, 'o'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
        case 's': g_size     = atoi(optarg); break;
        case 'd': g_duration = atoi(optarg); break;
        case 'o': strncpy(g_output, optarg, sizeof(g_output) - 1); break;
        case 'h':
        default:
            fprintf(stderr,
                "Usage: %s [--size=N] [--duration=SECS] [--output=FILE.csv]\n",
                argv[0]);
            exit(c == 'h' ? 0 : 1);
        }
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    int N = g_size;
    fprintf(stderr, "gemm_bench: N=%d  duration=%ds  output=%s\n",
            N, g_duration, g_output[0] ? g_output : "(none)");

    /* Allocate matrices */
    double *A = (double *)malloc((size_t)N * N * sizeof(double));
    double *B = (double *)malloc((size_t)N * N * sizeof(double));
    double *C = (double *)malloc((size_t)N * N * sizeof(double));
    if (!A || !B || !C) { perror("malloc"); return 1; }

    /* Initialise with random data */
    srand(42);
    for (int i = 0; i < N * N; i++) {
        A[i] = (double)rand() / RAND_MAX;
        B[i] = (double)rand() / RAND_MAX;
        C[i] = 0.0;
    }

    /* Set up perf counters */
    int fd_miss = setup_llc_miss_counter();
    int fd_load = setup_llc_load_counter();

    /* CSV output file */
    FILE *csv = NULL;
    if (g_output[0]) {
        csv = fopen(g_output, "w");
        if (!csv) {
            perror(g_output);
        } else {
            fprintf(csv, "second,gflops,llc_miss_rate\n");
        }
    }

    /* GFLOPS per dgemm call: 2 * N^3 (multiply-add) */
    double flops_per_call = 2.0 * (double)N * (double)N * (double)N;

    double all_gflops[3600]; /* max 1 hour */
    int    n_seconds = 0;

    uint64_t start_ns  = now_ns();
    uint64_t end_ns    = start_ns + (uint64_t)g_duration * 1000000000ULL;
    uint64_t second_ns = start_ns + 1000000000ULL;

    uint64_t count_in_second = 0;
    double   gflops_sum = 0.0, gflops_min = 1e9, gflops_max = 0.0;

    while (!g_stop) {
        uint64_t t0 = now_ns();
        if (t0 >= end_ns) break;

        /* One DGEMM call: C = alpha*A*B + beta*C */
        cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    N, N, N,
                    1.0, A, N,
                         B, N,
                    1.0, C, N);
        count_in_second++;

        uint64_t now = now_ns();
        if (now >= second_ns) {
            double elapsed_s = (double)(now - (second_ns - 1000000000ULL)) / 1e9;
            double gflops = (flops_per_call * (double)count_in_second) / elapsed_s / 1e9;

            /* Read perf counters */
            uint64_t misses = read_perf_counter(fd_miss);
            uint64_t loads  = read_perf_counter(fd_load);
            double miss_rate = (loads > 0) ? (double)misses / (double)loads * 100.0 : 0.0;
            reset_perf_counter(fd_miss);
            reset_perf_counter(fd_load);

            printf("%.3f\n", gflops);
            fflush(stdout);

            if (csv)
                fprintf(csv, "%d,%.3f,%.3f\n", n_seconds, gflops, miss_rate);

            if (n_seconds < (int)(sizeof(all_gflops)/sizeof(all_gflops[0])))
                all_gflops[n_seconds] = gflops;
            n_seconds++;

            gflops_sum += gflops;
            if (gflops < gflops_min) gflops_min = gflops;
            if (gflops > gflops_max) gflops_max = gflops;

            count_in_second = 0;
            second_ns += 1000000000ULL;
        }
    }

    if (csv) fclose(csv);
    if (fd_miss >= 0) close(fd_miss);
    if (fd_load >= 0) close(fd_load);

    /* Summary to stderr */
    if (n_seconds > 0) {
        fprintf(stderr,
                "gemm_bench summary: n_seconds=%d  avg=%.3f  min=%.3f  max=%.3f GFLOPS\n",
                n_seconds,
                gflops_sum / (double)n_seconds,
                gflops_min, gflops_max);
    }

    free(A); free(B); free(C);
    return 0;
}
