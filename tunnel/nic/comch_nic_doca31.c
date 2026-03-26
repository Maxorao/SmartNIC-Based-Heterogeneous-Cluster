/*
 * comch_nic_doca31.c — NIC-side Comch implementation for DOCA 3.x (BF3 ARM)
 *
 * PLACEHOLDER — not yet implemented.  Selected when COMCH_NIC_DOCA_VER >= 30.
 *
 * This file is reserved for the BF3 migration.  When upgrading:
 *   1. Install DOCA 3.x SDK on the BF3 ARM OS.
 *   2. Implement using doca_comch_server (async/PE model), mirroring
 *      comch_host_doca31.c but for the server role.
 *   3. Set COMCH_NIC_DOCA_VER=31 in the NIC Makefile.
 *
 * Key API differences vs DOCA 1.5 (comch_nic_doca15.c):
 *   - Use doca_comch_server instead of doca_comm_channel_ep_t
 *   - Server also needs a Progress Engine (doca_pe)
 *   - doca_comch_server_create(dev, dev_rep, name, &server) for setup
 *   - Receive via callback: doca_comch_server_event_msg_recv_register()
 *   - Send via task: doca_comch_server_task_send_alloc_init()
 *   - Connection tracking via doca_comch_server_event_connection_status_changed_register()
 *
 * Reference: /opt/mellanox/doca/samples/doca_comch/comch_ctrl_path_server/
 *            (available on BF3 nodes once DOCA 3.x is installed)
 */

#include "../comch_api.h"
#include <doca_error.h>

/* Stub implementations that fail loudly — replace when BF3 is available */

doca_error_t comch_nic_init(comch_nic_ctx_t **ctx_out,
                              const char *dev_pci, const char *rep_pci,
                              const char *service_name)
{
    (void)ctx_out; (void)dev_pci; (void)rep_pci; (void)service_name;
    /* This implementation is a placeholder — see file header */
    return DOCA_ERROR_NOT_SUPPORTED;
}

doca_error_t comch_nic_send(comch_nic_ctx_t *ctx,
                              const void *msg, size_t len)
{
    (void)ctx; (void)msg; (void)len;
    return DOCA_ERROR_NOT_SUPPORTED;
}

doca_error_t comch_nic_recv(comch_nic_ctx_t *ctx,
                              void *msg, size_t *len)
{
    (void)ctx; (void)msg; (void)len;
    return DOCA_ERROR_NOT_SUPPORTED;
}

doca_error_t comch_nic_recv_blocking(comch_nic_ctx_t *ctx,
                                       void *msg, size_t *len,
                                       uint32_t timeout_ms)
{
    (void)ctx; (void)msg; (void)len; (void)timeout_ms;
    return DOCA_ERROR_NOT_SUPPORTED;
}

void comch_nic_destroy(comch_nic_ctx_t *ctx)
{
    (void)ctx;
}
