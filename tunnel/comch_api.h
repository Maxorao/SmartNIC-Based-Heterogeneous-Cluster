/*
 * comch_api.h — Version-agnostic abstraction layer for DOCA Comm Channel.
 *
 * This header defines a uniform interface used by all upper-layer components
 * (slave_monitor, forward_routine, bench tools).  The concrete implementation
 * is selected at compile time via the macros:
 *
 *   COMCH_HOST_DOCA_VER   Integer version * 10  (e.g. 15 = DOCA 1.5,
 *   COMCH_NIC_DOCA_VER                           31 = DOCA 3.1)
 *
 * Default values match the current lab environment:
 *   Host  = DOCA 3.1   (tianjin / fujian / helong x86)
 *   NIC   = DOCA 1.5   (BF2 ARM)
 *
 * Future migration to BF3 with DOCA 3.x on both sides only requires
 * changing COMCH_NIC_DOCA_VER — all callers stay unchanged.
 *
 * Usage (build system sets these; callers never need to include impl headers):
 *   Host: #include "tunnel/comch_api.h"
 *   NIC:  #include "tunnel/comch_api.h"
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* ── Compile-time version selection ────────────────────────────────────────── */

#ifndef COMCH_HOST_DOCA_VER
#  define COMCH_HOST_DOCA_VER 31   /* DOCA 3.1 on x86 host (current lab) */
#endif

#ifndef COMCH_NIC_DOCA_VER
#  define COMCH_NIC_DOCA_VER 15    /* DOCA 1.5 on BF2 ARM (current lab) */
#endif

/* When both sides run DOCA 3.x (BF3 migration target): set both to 31. */

/* ── DOCA error type ────────────────────────────────────────────────────────── */
#include <doca_error.h>   /* doca_error_t, DOCA_SUCCESS, DOCA_ERROR_AGAIN … */

/* ── Constants ──────────────────────────────────────────────────────────────── */

/*
 * Maximum single-message payload in bytes.
 * BF2 DOCA 1.5 hardware cap is 4080 B; DOCA 3.x raises this, but we keep
 * the conservative value so the same buffer sizes work across all versions.
 */
#define COMCH_MAX_MSG_SIZE  4080U

/* Default service name used by slave_monitor / forward_routine. */
#define COMCH_SERVICE_NAME  "cluster-control"

/* ── Opaque context handles ─────────────────────────────────────────────────── */

/*
 * Each implementation defines 'struct comch_host_ctx' / 'struct comch_nic_ctx'
 * internally.  Callers only hold pointers to them.
 */
typedef struct comch_host_ctx comch_host_ctx_t;
typedef struct comch_nic_ctx  comch_nic_ctx_t;

/* ── Host-side API (runs on x86) ────────────────────────────────────────────── */

/**
 * comch_host_init - Open the BF device and establish a Comch connection.
 *
 * @ctx          [out] Allocated context; caller must eventually call destroy.
 * @pci_addr     [in]  PCI address of the BF PF as seen from the host,
 *                     e.g. "5e:00.0".  Use "auto" to select the first
 *                     suitable BF device found.
 * @service_name [in]  Must match the name used by comch_nic_init on the NIC.
 *
 * Blocks until the connection handshake completes (up to ~5 s timeout).
 */
doca_error_t comch_host_init(comch_host_ctx_t **ctx,
                              const char        *pci_addr,
                              const char        *service_name);

/**
 * comch_host_send - Send a message to the NIC.  Blocks until the hardware
 * acknowledges the send (synchronous wrapper over async DOCA 3.x tasks).
 */
doca_error_t comch_host_send(comch_host_ctx_t *ctx,
                              const void       *msg,
                              size_t            len);

/**
 * comch_host_recv - Non-blocking receive.
 * Returns DOCA_ERROR_AGAIN when the receive queue is empty.
 */
doca_error_t comch_host_recv(comch_host_ctx_t *ctx,
                              void             *msg,
                              size_t           *len);

/**
 * comch_host_recv_blocking - Block until a message arrives or timeout expires.
 * @timeout_ms   0 = wait forever.
 */
doca_error_t comch_host_recv_blocking(comch_host_ctx_t *ctx,
                                       void             *msg,
                                       size_t           *len,
                                       uint32_t          timeout_ms);

/** comch_host_destroy - Disconnect and free all resources. */
void comch_host_destroy(comch_host_ctx_t *ctx);

/* ── NIC-side API (runs on BF2 / BF3 ARM) ───────────────────────────────────── */

/**
 * comch_nic_init - Open the BF device + host representor and start listening.
 *
 * @ctx          [out] Allocated context.
 * @dev_pci      [in]  PCI address of the BF device from the ARM's view,
 *                     e.g. "03:00.0".
 * @rep_pci      [in]  PCI address of the host-facing representor (pf0hpf).
 *                     Pass NULL or "auto" to enumerate and pick automatically.
 * @service_name [in]  Must match the name used by comch_host_init on the host.
 *
 * Returns after starting the listener.  The first call to comch_nic_recv*
 * will also accept the first incoming client connection.
 */
doca_error_t comch_nic_init(comch_nic_ctx_t **ctx,
                              const char       *dev_pci,
                              const char       *rep_pci,
                              const char       *service_name);

/**
 * comch_nic_send - Send a reply to the currently-connected host.
 * peer_addr is tracked internally; callers do not manage it.
 */
doca_error_t comch_nic_send(comch_nic_ctx_t *ctx,
                              const void      *msg,
                              size_t           len);

/**
 * comch_nic_recv - Non-blocking receive.
 * Returns DOCA_ERROR_AGAIN when nothing is available.
 * The first successful recv also implicitly accepts the client connection.
 */
doca_error_t comch_nic_recv(comch_nic_ctx_t *ctx,
                              void            *msg,
                              size_t          *len);

/**
 * comch_nic_recv_blocking - Block until a message arrives or timeout expires.
 * @timeout_ms   0 = wait forever.
 */
doca_error_t comch_nic_recv_blocking(comch_nic_ctx_t *ctx,
                                      void            *msg,
                                      size_t          *len,
                                      uint32_t         timeout_ms);

/** comch_nic_destroy - Disconnect and free all resources. */
void comch_nic_destroy(comch_nic_ctx_t *ctx);
