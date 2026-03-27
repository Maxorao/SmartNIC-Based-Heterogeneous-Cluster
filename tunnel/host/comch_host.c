/*
 * comch_host.c — Version dispatcher for host-side DOCA Comch.
 *
 * Selects the concrete implementation at compile time via COMCH_HOST_DOCA_VER:
 *
 *   COMCH_HOST_DOCA_VER >= 30  →  comch_host_doca31.c   (DOCA 3.x, current)
 *
 * Usage: callers include comch_api.h and link against this object.
 * They never include implementation files directly.
 *
 * To add a new DOCA version: create comch_host_docaXX.c and add a branch below.
 */

#include "../comch_api.h"

#if COMCH_HOST_DOCA_VER >= 30
#  include "comch_host_doca31.c"
#elif COMCH_HOST_DOCA_VER >= 20
#  error "DOCA 2.x host-side Comch not implemented. Set COMCH_HOST_DOCA_VER=31."
#else
#  error "DOCA 1.x host-side Comch not implemented. Set COMCH_HOST_DOCA_VER=31."
#endif
